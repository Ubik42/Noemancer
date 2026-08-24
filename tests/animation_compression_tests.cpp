#include "engine/gltf_mesh.hpp"
#include "engine/simulation_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename T>
void append(std::vector<std::byte>& bytes, const T value) {
    const auto begin = bytes.size();
    bytes.resize(begin + sizeof(T));
    std::memcpy(bytes.data() + begin, &value, sizeof(T));
}

template <typename T, std::size_t N>
void append_all(std::vector<std::byte>& bytes, const std::array<T, N>& values) {
    for (const auto value : values) append(bytes, value);
}

std::filesystem::path make_compression_fixture_glb() {
    // This is the same small, deterministic skinned-triangle shape used by
    // the glTF animation test, with two extra animation samplers.  The root
    // translation supplies root motion; the five-key child rotation gives the
    // ozz optimizer non-trivial keys to inspect while retaining a tiny fixture.
    std::vector<std::byte> binary;
    append_all(binary, std::array<float, 9>{
        -0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F});
    append_all(binary, std::array<std::uint8_t, 12>{0U, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 0U});
    append_all(binary, std::array<float, 12>{
        1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F,
        1.0F, 0.0F, 0.0F, 0.0F});
    append_all(binary, std::array<std::uint16_t, 3>{0U, 1U, 2U});
    while (binary.size() % 4U != 0U) append(binary, std::uint8_t{});

    for (int matrix = 0; matrix < 2; ++matrix) {
        for (int component = 0; component < 16; ++component)
            append(binary, component % 5 == 0 ? 1.0F : 0.0F);
    }

    append_all(binary, std::array<float, 5>{0.0F, 0.5F, 1.0F, 1.5F, 2.0F});
    append_all(binary, std::array<float, 15>{
        0.0F, 0.0F, 0.0F, 0.25F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F,
        0.75F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F});
    append_all(binary, std::array<float, 5>{0.0F, 0.5F, 1.0F, 1.5F, 2.0F});
    append_all(binary, std::array<float, 20>{
        0.0F, 0.0F, -0.19866933F, 0.98006658F,
        0.0F, 0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 0.19866933F, 0.98006658F,
        0.0F, 0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, -0.19866933F, 0.98006658F});

    using Json = nlohmann::json;
    const Json document = {
        {"asset", {{"version", "2.0"}, {"generator", "noemancer.animation-compression-test"}}},
        {"buffers", {{{"byteLength", binary.size()}}}},
        {"bufferViews", {
            {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}},
            {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 12}},
            {{"buffer", 0}, {"byteOffset", 48}, {"byteLength", 48}},
            {{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 6}},
            {{"buffer", 0}, {"byteOffset", 104}, {"byteLength", 128}},
            {{"buffer", 0}, {"byteOffset", 232}, {"byteLength", 20}},
            {{"buffer", 0}, {"byteOffset", 252}, {"byteLength", 60}},
            {{"buffer", 0}, {"byteOffset", 312}, {"byteLength", 20}},
            {{"buffer", 0}, {"byteOffset", 332}, {"byteLength", 80}}}},
        {"accessors", {
            {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
            {{"bufferView", 1}, {"componentType", 5121}, {"count", 3}, {"type", "VEC4"}},
            {{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC4"}},
            {{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
            {{"bufferView", 4}, {"componentType", 5126}, {"count", 2}, {"type", "MAT4"}},
            {{"bufferView", 5}, {"componentType", 5126}, {"count", 5}, {"type", "SCALAR"}},
            {{"bufferView", 6}, {"componentType", 5126}, {"count", 5}, {"type", "VEC3"}},
            {{"bufferView", 7}, {"componentType", 5126}, {"count", 5}, {"type", "SCALAR"}},
            {{"bufferView", 8}, {"componentType", 5126}, {"count", 5}, {"type", "VEC4"}}}},
        {"meshes", {{{"name", "CompressionTriangle"}, {"primitives", {{
            {"attributes", {{"POSITION", 0}, {"JOINTS_0", 1}, {"WEIGHTS_0", 2}}},
            {"indices", 3}}}}}}},
        {"nodes", {
            {{"name", "Root"}, {"children", {1}}},
            {{"name", "Tip"}, {"translation", {0.0F, 0.5F, 0.0F}}},
            {{"name", "CompressionTriangle"}, {"mesh", 0}, {"skin", 0}}}},
        {"skins", {{{"name", "CompressionRig"}, {"joints", {0, 1}},
            {"inverseBindMatrices", 4}, {"skeleton", 0}}}},
        {"animations", {{{"name", "CompressionClip"},
            {"samplers", {
                {{"input", 5}, {"output", 6}, {"interpolation", "LINEAR"}},
                {{"input", 7}, {"output", 8}, {"interpolation", "LINEAR"}}}},
            {"channels", {
                {{"sampler", 0}, {"target", {{"node", 0}, {"path", "translation"}}}},
                {{"sampler", 1}, {"target", {{"node", 1}, {"path", "rotation"}}}}}}}}},
        {"scenes", {{{"nodes", {0, 2}}}}},
        {"scene", 0}};

    std::string json = document.dump();
    while (json.size() % 4U != 0U) json.push_back(' ');
    std::vector<std::byte> glb;
    const auto total_length = static_cast<std::uint32_t>(12U + 8U + json.size() + 8U + binary.size());
    append(glb, std::uint32_t{0x46546c67U});
    append(glb, std::uint32_t{2U});
    append(glb, total_length);
    append(glb, static_cast<std::uint32_t>(json.size()));
    append(glb, std::uint32_t{0x4e4f534aU});
    const auto json_begin = glb.size();
    glb.resize(json_begin + json.size());
    std::memcpy(glb.data() + json_begin, json.data(), json.size());
    append(glb, static_cast<std::uint32_t>(binary.size()));
    append(glb, std::uint32_t{0x004e4942U});
    glb.insert(glb.end(), binary.begin(), binary.end());

    const auto path = std::filesystem::temp_directory_path() /
        "noemancer-animation-compression-fixture.glb";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    return path;
}

float compare_pose_model_translation(const noemancer::SkeletalPose& left,
                                     const noemancer::SkeletalPose& right) {
    float result{};
    if (left.skinning_matrices.size() != right.skinning_matrices.size() ||
        left.joints.size() != right.joints.size()) return INFINITY;
    for (std::size_t joint = 0; joint < left.joints.size(); ++joint) {
        if (left.joints[joint].name != right.joints[joint].name || left.joints[joint].parent != right.joints[joint].parent) {
            return INFINITY;
        }
        const auto dx = left.joints[joint].model_x - right.joints[joint].model_x;
        const auto dy = left.joints[joint].model_y - right.joints[joint].model_y;
        const auto dz = left.joints[joint].model_z - right.joints[joint].model_z;
        result = std::max(result, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return result;
}

float compare_root_motion(const noemancer::RootMotionDelta& left, const noemancer::RootMotionDelta& right) {
    if (left.valid != right.valid) return INFINITY;
    if (!left.valid) return 0.0F;
    return std::max({std::abs(left.x - right.x), std::abs(left.y - right.y), std::abs(left.z - right.z)});
}

const char* compression_mode_name(const noemancer::AnimationCompressionMode mode) {
    return mode == noemancer::AnimationCompressionMode::ozz_hierarchical_key_reduction
        ? "ozz_hierarchical_key_reduction" : "ozz_runtime_baseline";
}

nlohmann::json compression_evidence_json(const noemancer::AnimationCompressionEvidence& evidence) {
    return {
        {"schemaVersion", evidence.schema_version}, {"backend", evidence.backend},
        {"requestedMode", compression_mode_name(evidence.requested_mode)},
        {"optimizerAttempted", evidence.optimizer_attempted}, {"optimizerApplied", evidence.optimizer_applied},
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

bool write_evidence_json(const char* path, const noemancer::GltfBinaryContainer& container,
                         const noemancer::GltfMeshData& decoded, const noemancer::AnimationCompileResult& baseline,
                         const noemancer::AnimationCompileResult& optimized,
                         const noemancer::AnimationClipComparisonResult& comparison,
                         const float max_fixed_pose_model_error, const float max_root_motion_error) {
    if (path == nullptr || *path == '\0') return true;
    const nlohmann::json evidence = {
        {"schemaVersion", "noemancer.animation-compression-test/0.1"},
        {"fixture", {
            {"containerValid", container.valid}, {"containerCode", container.code},
            {"sourceBytes", container.source_bytes}, {"hasBinaryChunk", container.has_binary_chunk},
            {"decodedValid", decoded.valid}, {"decodedCode", decoded.code}, {"decodedDetail", decoded.detail}}},
        {"compile", {
            {"baseline", {{"success", baseline.success}, {"code", baseline.code}, {"detail", baseline.detail},
                {"skeletonAsset", baseline.skeleton_asset}, {"clipAsset", baseline.clip_asset},
                {"jointCount", baseline.joint_count}, {"duration", baseline.duration},
                {"compression", compression_evidence_json(baseline.compression)}}},
            {"optimized", {{"success", optimized.success}, {"code", optimized.code}, {"detail", optimized.detail},
                {"skeletonAsset", optimized.skeleton_asset}, {"clipAsset", optimized.clip_asset},
                {"jointCount", optimized.joint_count}, {"duration", optimized.duration},
                {"compression", compression_evidence_json(optimized.compression)}}}}},
        {"comparison", {
            {"success", comparison.success}, {"code", comparison.code}, {"detail", comparison.detail},
            {"sampleCount", comparison.sample_count}, {"jointCount", comparison.joint_count},
            {"maximumLocalTranslationErrorMeters", comparison.maximum_local_translation_error_meters},
            {"maximumLocalRotationErrorDegrees", comparison.maximum_local_rotation_error_degrees},
            {"maximumLocalScaleError", comparison.maximum_local_scale_error},
            {"maximumModelTranslationErrorMeters", comparison.maximum_model_translation_error_meters},
            {"maximumModelProbeErrorMeters", comparison.maximum_model_probe_error_meters},
            {"maximumSkinningMatrixAbsoluteError", comparison.maximum_skinning_matrix_absolute_error},
            {"maximumRootMotionDeltaErrorMeters", comparison.maximum_root_motion_delta_error_meters}}},
        {"fixedSamples", {
            {"maximumModelTranslationErrorMeters", max_fixed_pose_model_error},
            {"maximumRootMotionErrorMeters", max_root_motion_error}}},
        {"error", {
            {"maximumContractModelErrorMeters", std::max({max_fixed_pose_model_error,
                comparison.maximum_model_translation_error_meters, comparison.maximum_model_probe_error_meters,
                max_root_motion_error, comparison.maximum_root_motion_delta_error_meters})},
            {"maximumSkinningMatrixAbsoluteError", comparison.maximum_skinning_matrix_absolute_error},
            {"maximumLocalRotationErrorDegrees", comparison.maximum_local_rotation_error_degrees}}}
    };
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << evidence.dump(2) << '\n';
    return static_cast<bool>(output);
}

bool check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main() {
    std::error_code ignored;
    const auto path = make_compression_fixture_glb();
    const auto container = noemancer::read_glb_container(path);
    const auto decoded = noemancer::decode_glb_mesh(path);
    std::filesystem::remove(path, ignored);
    if (!check(container.valid && container.version == 2U && container.has_binary_chunk,
               "Deterministic compression GLB did not validate as a GLB 2 container.") ||
        !check(decoded.valid && decoded.skins.size() == 1U && decoded.skins.front().joints.size() == 2U &&
                   decoded.animations.size() == 1U && decoded.animations.front().channels.size() == 2U,
               "Deterministic compression GLB did not decode the expected skin and channels.")) {
        return 1;
    }

    noemancer::AnimationRuntime runtime;
    const auto unoptimized = runtime.compile_gltf_asset(
        "asset.test.animation-compression-baseline", decoded, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_runtime_baseline);
    const auto optimized = runtime.compile_gltf_asset(
        "asset.test.animation-compression-optimized", decoded, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_hierarchical_key_reduction);
    const auto comparison = runtime.compare_compiled_clips(unoptimized.clip_asset, optimized.clip_asset, 257U);

    if (!check(unoptimized.success && optimized.success && unoptimized.code == "ok" && optimized.code == "ok" &&
                   unoptimized.joint_count == 2U && optimized.joint_count == 2U &&
                   std::abs(unoptimized.duration - 2.0F) <= 0.000001F &&
                   std::abs(optimized.duration - unoptimized.duration) <= 0.000001F,
               "Unoptimized and optimized animation compilation did not both succeed.")) {
        std::cerr << "unoptimized=" << unoptimized.code << ": " << unoptimized.detail
                  << " optimized=" << optimized.code << ": " << optimized.detail << '\n';
        return 3;
    }

    const auto& baseline = unoptimized.compression;
    const auto& selected = optimized.compression;
    const bool baseline_evidence =
        baseline.schema_version == "noemancer.animation-compression/0.1" &&
        baseline.backend == "ozz-animation/0.17.0" &&
        baseline.requested_mode == noemancer::AnimationCompressionMode::ozz_runtime_baseline &&
        !baseline.optimizer_attempted && !baseline.optimizer_applied && !baseline.fallback_used &&
        baseline.input_raw_resident_bytes > 0U &&
        baseline.optimized_raw_resident_bytes == baseline.input_raw_resident_bytes &&
        baseline.input_raw_archive_bytes > 0U &&
        baseline.optimized_raw_archive_bytes == baseline.input_raw_archive_bytes &&
        baseline.input_raw_archive_hash.starts_with("fnv1a64:") &&
        baseline.optimized_raw_archive_hash == baseline.input_raw_archive_hash &&
        baseline.baseline_runtime_resident_bytes > 0U &&
        baseline.selected_runtime_resident_bytes == baseline.baseline_runtime_resident_bytes &&
        baseline.baseline_runtime_archive_bytes > 0U &&
        baseline.selected_runtime_archive_bytes == baseline.baseline_runtime_archive_bytes &&
        baseline.baseline_runtime_archive_hash.starts_with("fnv1a64:") &&
        baseline.selected_runtime_archive_hash == baseline.baseline_runtime_archive_hash &&
        baseline.skeleton_archive_bytes > 0U && baseline.skeleton_archive_hash.starts_with("fnv1a64:") &&
        baseline.input_translation_keys == baseline.selected_translation_keys &&
        baseline.input_rotation_keys == baseline.selected_rotation_keys &&
        baseline.input_scale_keys == baseline.selected_scale_keys;
    const bool optimized_evidence =
        selected.schema_version == baseline.schema_version && selected.backend == baseline.backend &&
        selected.requested_mode == noemancer::AnimationCompressionMode::ozz_hierarchical_key_reduction &&
        selected.optimizer_attempted && (selected.optimizer_applied || selected.fallback_used) &&
        !(selected.optimizer_applied && selected.fallback_used) &&
        selected.input_raw_resident_bytes == baseline.input_raw_resident_bytes &&
        selected.input_raw_archive_bytes == baseline.input_raw_archive_bytes &&
        selected.input_raw_archive_hash.starts_with("fnv1a64:") &&
        selected.input_raw_archive_hash == baseline.input_raw_archive_hash &&
        selected.input_translation_keys == baseline.input_translation_keys &&
        selected.input_rotation_keys == baseline.input_rotation_keys &&
        selected.input_scale_keys == baseline.input_scale_keys &&
        selected.optimized_raw_resident_bytes > 0U && selected.optimized_raw_archive_bytes > 0U &&
        selected.optimized_raw_archive_hash.starts_with("fnv1a64:") &&
        selected.baseline_runtime_resident_bytes == baseline.baseline_runtime_resident_bytes &&
        selected.baseline_runtime_archive_bytes == baseline.baseline_runtime_archive_bytes &&
        selected.selected_runtime_resident_bytes > 0U && selected.selected_runtime_archive_bytes > 0U &&
        selected.selected_runtime_archive_hash.starts_with("fnv1a64:") &&
        selected.skeleton_archive_bytes == baseline.skeleton_archive_bytes &&
        selected.skeleton_archive_hash == baseline.skeleton_archive_hash &&
        selected.selected_translation_keys <= selected.input_translation_keys &&
        selected.selected_rotation_keys <= selected.input_rotation_keys &&
        selected.selected_scale_keys <= selected.input_scale_keys &&
        selected.selected_translation_keys + selected.selected_rotation_keys + selected.selected_scale_keys > 0U;
    if (!check(baseline_evidence && optimized_evidence,
               "Animation compression evidence did not preserve optimizer/fallback, key and byte contracts.")) {
        std::cerr << "baseline bytes=" << baseline.input_raw_resident_bytes << "/"
                  << baseline.input_raw_archive_bytes << "/" << baseline.baseline_runtime_resident_bytes << "/"
                  << baseline.baseline_runtime_archive_bytes << " selected bytes="
                  << selected.optimized_raw_resident_bytes << "/" << selected.optimized_raw_archive_bytes << "/"
                  << selected.selected_runtime_resident_bytes << "/" << selected.selected_runtime_archive_bytes
                  << " keys=" << selected.selected_translation_keys << ","
                  << selected.selected_rotation_keys << "," << selected.selected_scale_keys
                  << " applied=" << selected.optimizer_applied << " fallback=" << selected.fallback_used << '\n';
        return 4;
    }
    if (selected.optimizer_applied &&
        !check(selected.optimized_raw_resident_bytes <= selected.input_raw_resident_bytes &&
                   selected.optimized_raw_archive_bytes <= selected.input_raw_archive_bytes &&
                   selected.selected_runtime_resident_bytes <= selected.baseline_runtime_resident_bytes &&
                   selected.selected_runtime_archive_bytes <= selected.baseline_runtime_archive_bytes,
               "Applied animation optimizer increased raw or selected runtime bytes.")) {
        return 5;
    }
    if (selected.fallback_used &&
        !check(selected.optimized_raw_resident_bytes == selected.input_raw_resident_bytes &&
                   selected.optimized_raw_archive_bytes == selected.input_raw_archive_bytes &&
                   selected.optimized_raw_archive_hash == selected.input_raw_archive_hash &&
                   selected.selected_runtime_resident_bytes == selected.baseline_runtime_resident_bytes &&
                   selected.selected_runtime_archive_bytes == selected.baseline_runtime_archive_bytes &&
                   selected.selected_runtime_archive_hash == selected.baseline_runtime_archive_hash &&
                   selected.selected_translation_keys == selected.input_translation_keys &&
                   selected.selected_rotation_keys == selected.input_rotation_keys &&
                   selected.selected_scale_keys == selected.input_scale_keys,
               "Animation optimizer fallback did not select the baseline payload.")) {
        return 6;
    }

    constexpr std::array<float, 5> sample_times{0.0F, 0.5F, 1.0F, 1.5F, 2.0F};
    float max_fixed_pose_model_error{};
    for (const auto time : sample_times) {
        const auto baseline_pose = runtime.sample_skeletal_pose(unoptimized.clip_asset, time);
        const auto selected_pose = runtime.sample_skeletal_pose(optimized.clip_asset, time);
        if (!check(baseline_pose.valid && selected_pose.valid && baseline_pose.skinning_matrices.size() == 2U &&
                       selected_pose.skinning_matrices.size() == baseline_pose.skinning_matrices.size() &&
                       baseline_pose.joints.size() == selected_pose.joints.size(),
                   "Unoptimized or optimized animation did not produce a complete skeletal pose.")) {
            return 7;
        }
        max_fixed_pose_model_error = std::max(max_fixed_pose_model_error,
            compare_pose_model_translation(baseline_pose, selected_pose));
    }

    float max_root_motion_error{};
    for (const auto& interval : std::array<std::array<float, 2>, 4>{
             std::array<float, 2>{0.0F, 0.5F}, std::array<float, 2>{0.5F, 1.0F},
             std::array<float, 2>{1.5F, 2.0F}, std::array<float, 2>{1.5F, 0.5F}}) {
        const auto baseline_delta = runtime.root_motion_delta(
            unoptimized.clip_asset, interval[0], interval[1], true, 1.0F);
        const auto selected_delta = runtime.root_motion_delta(
            optimized.clip_asset, interval[0], interval[1], true, 1.0F);
        if (!check(baseline_delta.valid && selected_delta.valid,
                   "Unoptimized or optimized animation did not produce root motion evidence.")) {
            return 8;
        }
        max_root_motion_error = std::max(max_root_motion_error,
            compare_root_motion(baseline_delta, selected_delta));
    }

    constexpr float allowed_model_error = 0.0011F;
    const auto* evidence_path = std::getenv("NOEMANCER_ANIMATION_COMPRESSION_EVIDENCE_PATH");
    if (!write_evidence_json(evidence_path, container, decoded, unoptimized, optimized, comparison,
                             max_fixed_pose_model_error, max_root_motion_error)) {
        std::cerr << "Could not write NOEMANCER_ANIMATION_COMPRESSION_EVIDENCE_PATH: "
                  << evidence_path << '\n';
        return 10;
    }
    std::cout << "animation_compression_tests: mode=ozz_runtime_baseline+ozz_hierarchical_key_reduction"
              << " optimizerApplied=" << (selected.optimizer_applied ? "true" : "false")
              << " fallbackUsed=" << (selected.fallback_used ? "true" : "false")
              << " baselineRawResidentBytes=" << baseline.input_raw_resident_bytes
              << " selectedRawResidentBytes=" << selected.optimized_raw_resident_bytes
              << " baselineRawArchiveBytes=" << baseline.input_raw_archive_bytes
              << " selectedRawArchiveBytes=" << selected.optimized_raw_archive_bytes
              << " baselineRuntimeResidentBytes=" << baseline.baseline_runtime_resident_bytes
              << " selectedRuntimeResidentBytes=" << selected.selected_runtime_resident_bytes
              << " baselineRuntimeArchiveBytes=" << baseline.baseline_runtime_archive_bytes
              << " selectedRuntimeArchiveBytes=" << selected.selected_runtime_archive_bytes
              << " inputKeys=" << selected.input_translation_keys << "," << selected.input_rotation_keys << ","
              << selected.input_scale_keys << " selectedKeys=" << selected.selected_translation_keys << ","
              << selected.selected_rotation_keys << "," << selected.selected_scale_keys
              << " maxFixedPoseModelError=" << max_fixed_pose_model_error
              << " maxModelTranslationError=" << comparison.maximum_model_translation_error_meters
              << " maxModelProbeError=" << comparison.maximum_model_probe_error_meters
              << " maxSkinningMatrixError=" << comparison.maximum_skinning_matrix_absolute_error
              << " maxLocalRotationErrorDegrees=" << comparison.maximum_local_rotation_error_degrees
              << " maxRootMotionError=" << max_root_motion_error
              << " maxSampledRootMotionError=" << comparison.maximum_root_motion_delta_error_meters << '\n';
    if (!check(comparison.success && comparison.code == "ok" && comparison.sample_count == 257U &&
                   comparison.joint_count == 2U &&
                   comparison.maximum_model_translation_error_meters <= allowed_model_error &&
                   comparison.maximum_model_probe_error_meters <= allowed_model_error &&
                   comparison.maximum_root_motion_delta_error_meters <= allowed_model_error &&
                   max_fixed_pose_model_error <= allowed_model_error && max_root_motion_error <= allowed_model_error,
               "Optimized animation exceeded the model/probe or root-motion error budget.")) {
        return 9;
    }
    return 0;
}
