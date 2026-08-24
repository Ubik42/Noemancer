#include "engine/fbx_asset.hpp"
#include "engine/process_diagnostics.hpp"
#include "engine/simulation_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::size_t warmup_frames = 60U;
constexpr std::size_t sampled_frames = 120U;
constexpr std::size_t poses_per_frame = 64U;
constexpr std::size_t comparison_samples = 257U;
constexpr float allowed_model_error_meters = 0.0011F;

using PoseTimes = std::array<float, poses_per_frame>;

struct TimingDistribution final {
    std::vector<double> frame_milliseconds;
    std::size_t pose_count{};
};

const char* compression_mode_name(const noemancer::AnimationCompressionMode mode) {
    return mode == noemancer::AnimationCompressionMode::ozz_hierarchical_key_reduction
        ? "ozz_hierarchical_key_reduction" : "ozz_runtime_baseline";
}

Json compression_evidence_json(const noemancer::AnimationCompressionEvidence& evidence) {
    return {
        {"schemaVersion", evidence.schema_version},
        {"backend", evidence.backend},
        {"requestedMode", compression_mode_name(evidence.requested_mode)},
        {"optimizerAttempted", evidence.optimizer_attempted},
        {"optimizerApplied", evidence.optimizer_applied},
        {"fallbackUsed", evidence.fallback_used},
        {"hierarchicalToleranceMeters", evidence.hierarchical_tolerance_meters},
        {"measurementDistanceMeters", evidence.measurement_distance_meters},
        {"inputRawResidentBytes", evidence.input_raw_resident_bytes},
        {"optimizedRawResidentBytes", evidence.optimized_raw_resident_bytes},
        {"inputRawArchiveBytes", evidence.input_raw_archive_bytes},
        {"optimizedRawArchiveBytes", evidence.optimized_raw_archive_bytes},
        {"baselineRuntimeResidentBytes", evidence.baseline_runtime_resident_bytes},
        {"selectedRuntimeResidentBytes", evidence.selected_runtime_resident_bytes},
        {"baselineRuntimeArchiveBytes", evidence.baseline_runtime_archive_bytes},
        {"selectedRuntimeArchiveBytes", evidence.selected_runtime_archive_bytes},
        {"skeletonArchiveBytes", evidence.skeleton_archive_bytes},
        {"inputRawArchiveHash", evidence.input_raw_archive_hash},
        {"optimizedRawArchiveHash", evidence.optimized_raw_archive_hash},
        {"baselineRuntimeArchiveHash", evidence.baseline_runtime_archive_hash},
        {"selectedRuntimeArchiveHash", evidence.selected_runtime_archive_hash},
        {"skeletonArchiveHash", evidence.skeleton_archive_hash},
        {"inputTranslationKeys", evidence.input_translation_keys},
        {"inputRotationKeys", evidence.input_rotation_keys},
        {"inputScaleKeys", evidence.input_scale_keys},
        {"selectedTranslationKeys", evidence.selected_translation_keys},
        {"selectedRotationKeys", evidence.selected_rotation_keys},
        {"selectedScaleKeys", evidence.selected_scale_keys}
    };
}

Json compile_result_json(const noemancer::AnimationCompileResult& result) {
    return {
        {"success", result.success},
        {"code", result.code},
        {"detail", result.detail},
        {"skeletonAsset", result.skeleton_asset},
        {"clipAsset", result.clip_asset},
        {"jointCount", result.joint_count},
        {"duration", result.duration},
        {"compression", compression_evidence_json(result.compression)}
    };
}

Json comparison_json(const noemancer::AnimationClipComparisonResult& comparison) {
    return {
        {"success", comparison.success},
        {"code", comparison.code},
        {"detail", comparison.detail},
        {"sampleCount", comparison.sample_count},
        {"jointCount", comparison.joint_count},
        {"maximumLocalTranslationErrorMeters", comparison.maximum_local_translation_error_meters},
        {"maximumLocalRotationErrorDegrees", comparison.maximum_local_rotation_error_degrees},
        {"maximumLocalScaleError", comparison.maximum_local_scale_error},
        {"maximumModelTranslationErrorMeters", comparison.maximum_model_translation_error_meters},
        {"maximumModelProbeErrorMeters", comparison.maximum_model_probe_error_meters},
        {"maximumSkinningMatrixAbsoluteError", comparison.maximum_skinning_matrix_absolute_error},
        {"maximumRootMotionDeltaErrorMeters", comparison.maximum_root_motion_delta_error_meters}
    };
}

double percentile(const std::vector<double>& sorted, const double fraction) {
    if (sorted.empty()) return 0.0;
    const auto position = fraction * static_cast<double>(sorted.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

Json timing_distribution_json(const TimingDistribution& distribution) {
    std::vector<double> sorted = distribution.frame_milliseconds;
    std::ranges::sort(sorted);
    const auto mean = sorted.empty() ? 0.0 :
        std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    return {
        {"unit", "milliseconds"},
        {"frameCount", sorted.size()},
        {"poseCountPerFrame", distribution.pose_count},
        {"mean", mean},
        {"p50", percentile(sorted, 0.50)},
        {"p95", percentile(sorted, 0.95)},
        {"min", sorted.empty() ? 0.0 : sorted.front()},
        {"max", sorted.empty() ? 0.0 : sorted.back()}
    };
}

PoseTimes make_pose_times(const float duration, const std::size_t frame) {
    PoseTimes times{};
    if (duration <= 0.0F) return times;
    const auto frame_phase = std::fmod(static_cast<float>(frame % 120U) / 120.0F, 1.0F);
    for (std::size_t pose = 0; pose < poses_per_frame; ++pose) {
        const auto pose_phase = static_cast<float>(pose) / static_cast<float>(poses_per_frame);
        times[pose] = std::fmod((frame_phase + pose_phase) * duration, duration);
    }
    return times;
}

template <typename PoseSink>
void sample_pose_batch(const noemancer::AnimationRuntime& runtime, const std::string_view clip_asset,
                       const PoseTimes& times, PoseSink&& sink) {
    for (std::size_t pose = 0; pose < poses_per_frame; ++pose)
        sink(runtime.sample_skeletal_pose(clip_asset, times[pose]));
}

// The pose API returns a value, so the benchmark keeps a deterministic scalar
// sink to prevent the compiler from treating the sampled result as unused.
std::uint64_t pose_checksum(const noemancer::SkeletalPose& pose) {
    std::uint64_t result = pose.valid ? 1U : 0U;
    result = result * 1315423911U + pose.skinning_matrices.size();
    result = result * 1315423911U + pose.joints.size();
    if (!pose.joints.empty()) {
        result = result * 1315423911U +
            static_cast<std::uint64_t>(std::llround(pose.joints.front().model_x * 100000.0F));
    }
    return result;
}

std::uint64_t preload_pose_batch(const noemancer::AnimationRuntime& runtime, const std::string_view clip_asset,
                                 const PoseTimes& times,
                                 std::array<noemancer::SkeletalPose, poses_per_frame>& poses) {
    std::uint64_t checksum{};
    for (std::size_t pose = 0; pose < poses_per_frame; ++pose) {
        poses[pose] = runtime.sample_skeletal_pose(clip_asset, times[pose]);
        checksum ^= pose_checksum(poses[pose]);
    }
    return checksum;
}

bool write_evidence(const std::filesystem::path& path, const std::filesystem::path& fixture_path,
                    const noemancer::GltfMeshData& decoded,
                    const noemancer::AnimationCompileResult& baseline,
                    const noemancer::AnimationCompileResult& candidate,
                    const noemancer::AnimationClipComparisonResult& comparison,
                    const TimingDistribution& baseline_timing,
                    const TimingDistribution& candidate_timing,
                    const std::uint64_t checksum,
                    std::string& error) {
    if (path.empty()) return true;
    const bool correctness_passed = !candidate.compression.fallback_used &&
        candidate.compression.selected_runtime_archive_bytes <= baseline.compression.selected_runtime_archive_bytes &&
        comparison.maximum_model_translation_error_meters <= allowed_model_error_meters &&
        comparison.maximum_model_probe_error_meters <= allowed_model_error_meters &&
        comparison.maximum_root_motion_delta_error_meters <= allowed_model_error_meters;
    Json document = {
        {"schemaVersion", "noemancer.animation-compression-evidence/0.1"},
        {"workloadId", "noemancer.animation-compression/0.1"},
        {"runtime", {{"backend", candidate.compression.backend}}},
        {"fixture", {
            {"path", fixture_path.generic_string()},
            {"exists", std::filesystem::is_regular_file(fixture_path)},
            {"decodedValid", decoded.valid},
            {"decodedCode", decoded.code},
            {"decodedDetail", decoded.detail},
            {"vertexCount", decoded.vertices.size()},
            {"indexCount", decoded.indices.size()},
            {"primitiveCount", decoded.primitives.size()},
            {"jointCount", decoded.skins.empty() ? 0U : decoded.skins.front().joints.size()},
            {"animationCount", decoded.animations.size()}}},
        {"compile", {
            {"baseline", compile_result_json(baseline)},
            {"candidate", compile_result_json(candidate)}}},
        {"comparison", comparison_json(comparison)},
        {"timing", {
            {"warmupFrames", warmup_frames},
            {"sampledFrames", sampled_frames},
            {"posesPerFrame", poses_per_frame},
            {"preloadedPoseCount", poses_per_frame},
            {"ioIncluded", false},
            {"scope", "preloaded AnimationRuntime::sample_skeletal_pose"},
            {"order", "baseline-first-on-even-frames;candidate-first-on-odd-frames"},
            {"baseline", timing_distribution_json(baseline_timing)},
            {"candidate", timing_distribution_json(candidate_timing)}}},
        {"correctnessGate", {
            {"fallbackUsed", candidate.compression.fallback_used},
            {"candidateRuntimeArchiveNotLarger", candidate.compression.selected_runtime_archive_bytes <=
                baseline.compression.selected_runtime_archive_bytes},
            {"maximumModelTranslationErrorMeters", comparison.maximum_model_translation_error_meters},
            {"maximumModelProbeErrorMeters", comparison.maximum_model_probe_error_meters},
            {"maximumRootMotionDeltaErrorMeters", comparison.maximum_root_motion_delta_error_meters},
            {"allowedModelErrorMeters", allowed_model_error_meters},
            {"passed", correctness_passed}}},
        {"measurement", {
            {"checksum", checksum},
            {"timingExcludes", nlohmann::json::array({"FBX I/O", "FBX decode", "ozz compilation",
                "257-point comparison", "JSON serialization and file I/O"})},
            {"productionDefault", "ozz_runtime_baseline"}}},
        {"pass", correctness_passed}
    };
    std::error_code filesystem_error;
    auto parent = path.parent_path();
    if (parent.empty()) parent = ".";
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        error = "Could not create evidence parent directory: " + filesystem_error.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not open evidence output path.";
        return false;
    }
    output << document.dump(2) << '\n';
    if (!output) {
        error = "Could not write evidence JSON.";
        return false;
    }
    return true;
}

} // namespace

int main() {
    noemancer::configure_process_diagnostics("test.animation-compression-evidence");
    const auto fixture_path = std::filesystem::path(NOEMANCER_SOURCE_DIR) /
        "assets/local-test/mixamo/rumba-dancing-02.fbx";
    if (!std::filesystem::is_regular_file(fixture_path)) {
        std::cout << "Optional local Mixamo FBX fixture is unavailable; skipping.\n";
        return 77;
    }

    // Decode exactly once. Everything below consumes this resident plain-data
    // payload, keeping source I/O and import work outside timing windows.
    const auto decoded = noemancer::decode_fbx_asset(fixture_path);
    if (!decoded.valid || decoded.skins.empty() || decoded.skins.front().joints.empty() ||
        decoded.animations.empty()) {
        std::cerr << "FBX fixture did not decode a usable skin and animation: "
                  << decoded.code << " - " << decoded.detail << '\n';
        return 1;
    }

    noemancer::AnimationRuntime runtime;
    const auto baseline = runtime.compile_gltf_asset(
        "asset.test.mixamo-rumba-compression-evidence-baseline", decoded, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_runtime_baseline);
    const auto candidate = runtime.compile_gltf_asset(
        "asset.test.mixamo-rumba-compression-evidence-candidate", decoded, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_hierarchical_key_reduction);
    if (!baseline.success || !candidate.success) {
        std::cerr << "FBX ozz compilation failed: baseline=" << baseline.code << " - " << baseline.detail
                  << "; candidate=" << candidate.code << " - " << candidate.detail << '\n';
        return 2;
    }

    const auto comparison = runtime.compare_compiled_clips(
        baseline.clip_asset, candidate.clip_asset, comparison_samples);
    if (!comparison.success || comparison.sample_count != comparison_samples) {
        std::cerr << "FBX ozz clip comparison failed: " << comparison.code << " - " << comparison.detail << '\n';
        return 3;
    }

    // Precompute the fixed 120-frame schedule before timing. Each measured
    // frame samples 64 resident pose slots for each clip; the order alternates
    // so one lane does not always pay first-touch/cache effects.
    std::array<PoseTimes, sampled_frames> schedules{};
    for (std::size_t frame = 0; frame < sampled_frames; ++frame)
        schedules[frame] = make_pose_times(baseline.duration, warmup_frames + frame);

    std::uint64_t checksum{};
    std::array<noemancer::SkeletalPose, poses_per_frame> baseline_preloaded{};
    std::array<noemancer::SkeletalPose, poses_per_frame> candidate_preloaded{};
    checksum ^= preload_pose_batch(runtime, baseline.clip_asset, schedules.front(), baseline_preloaded);
    checksum ^= preload_pose_batch(runtime, candidate.clip_asset, schedules.front(), candidate_preloaded);

    TimingDistribution baseline_timing;
    TimingDistribution candidate_timing;
    for (std::size_t frame = 0; frame < warmup_frames; ++frame) {
        const auto times = make_pose_times(baseline.duration, frame);
        if ((frame % 2U) == 0U) {
            sample_pose_batch(runtime, baseline.clip_asset, times, [&](const noemancer::SkeletalPose& pose) {
                checksum ^= pose_checksum(pose) + static_cast<std::uint64_t>(frame);
            });
            sample_pose_batch(runtime, candidate.clip_asset, times, [&](const noemancer::SkeletalPose& pose) {
                checksum ^= pose_checksum(pose) + static_cast<std::uint64_t>(frame);
            });
        } else {
            sample_pose_batch(runtime, candidate.clip_asset, times, [&](const noemancer::SkeletalPose& pose) {
                checksum ^= pose_checksum(pose) + static_cast<std::uint64_t>(frame);
            });
            sample_pose_batch(runtime, baseline.clip_asset, times, [&](const noemancer::SkeletalPose& pose) {
                checksum ^= pose_checksum(pose) + static_cast<std::uint64_t>(frame);
            });
        }
    }

    baseline_timing.frame_milliseconds.reserve(sampled_frames);
    candidate_timing.frame_milliseconds.reserve(sampled_frames);
    baseline_timing.pose_count = poses_per_frame;
    candidate_timing.pose_count = poses_per_frame;
    for (std::size_t frame = 0; frame < sampled_frames; ++frame) {
        const auto sample_lane = [&](const std::string_view clip_asset, TimingDistribution& distribution) {
            const auto begin = Clock::now();
            sample_pose_batch(runtime, clip_asset, schedules[frame], [&](const noemancer::SkeletalPose& pose) {
                checksum ^= pose_checksum(pose) + static_cast<std::uint64_t>(frame);
            });
            const auto end = Clock::now();
            distribution.frame_milliseconds.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
        };
        if ((frame % 2U) == 0U) {
            sample_lane(baseline.clip_asset, baseline_timing);
            sample_lane(candidate.clip_asset, candidate_timing);
        } else {
            sample_lane(candidate.clip_asset, candidate_timing);
            sample_lane(baseline.clip_asset, baseline_timing);
        }
    }

    const bool correctness_gate = !candidate.compression.fallback_used &&
        candidate.compression.optimizer_attempted && candidate.compression.optimizer_applied &&
        candidate.compression.selected_runtime_archive_bytes <= baseline.compression.selected_runtime_archive_bytes &&
        comparison.maximum_model_translation_error_meters <= allowed_model_error_meters &&
        comparison.maximum_model_probe_error_meters <= allowed_model_error_meters &&
        comparison.maximum_root_motion_delta_error_meters <= allowed_model_error_meters;

    const auto* evidence_path_value = std::getenv("NOEMANCER_ANIMATION_COMPRESSION_EVIDENCE_PATH");
    const std::filesystem::path evidence_path = evidence_path_value == nullptr ?
        std::filesystem::path{} : std::filesystem::path(evidence_path_value);
    std::string evidence_error;
    if (!write_evidence(evidence_path, fixture_path, decoded, baseline, candidate, comparison,
                        baseline_timing, candidate_timing, checksum, evidence_error)) {
        std::cerr << "Could not write NOEMANCER_ANIMATION_COMPRESSION_EVIDENCE_PATH: "
                  << evidence_error << '\n';
        return 10;
    }

    std::cout << "animation_compression_evidence_tests: schema=noemancer.animation-compression-evidence/0.1"
              << " mode=ozz_runtime_baseline+ozz_hierarchical_key_reduction"
              << " fallbackUsed=" << (candidate.compression.fallback_used ? "true" : "false")
              << " baselineRuntimeArchiveBytes=" << baseline.compression.selected_runtime_archive_bytes
              << " candidateRuntimeArchiveBytes=" << candidate.compression.selected_runtime_archive_bytes
              << " maxModelTranslationError=" << comparison.maximum_model_translation_error_meters
              << " maxModelProbeError=" << comparison.maximum_model_probe_error_meters
              << " maxRootMotionError=" << comparison.maximum_root_motion_delta_error_meters
              << " baselineMeanMs=" << timing_distribution_json(baseline_timing).at("mean")
              << " baselineP50Ms=" << timing_distribution_json(baseline_timing).at("p50")
              << " baselineP95Ms=" << timing_distribution_json(baseline_timing).at("p95")
              << " candidateMeanMs=" << timing_distribution_json(candidate_timing).at("mean")
              << " candidateP50Ms=" << timing_distribution_json(candidate_timing).at("p50")
              << " candidateP95Ms=" << timing_distribution_json(candidate_timing).at("p95")
              << " checksum=" << checksum << '\n';
    if (!correctness_gate) {
        std::cerr << "Animation compression evidence correctness gate failed: fallback="
                  << (candidate.compression.fallback_used ? "true" : "false")
                  << " optimizerApplied=" << (candidate.compression.optimizer_applied ? "true" : "false")
                  << " baselineArchive=" << baseline.compression.selected_runtime_archive_bytes
                  << " candidateArchive=" << candidate.compression.selected_runtime_archive_bytes
                  << " modelTranslation=" << comparison.maximum_model_translation_error_meters
                  << " modelProbe=" << comparison.maximum_model_probe_error_meters
                  << " rootMotion=" << comparison.maximum_root_motion_delta_error_meters << '\n';
        return 4;
    }
    return 0;
}
