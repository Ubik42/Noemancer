#include "engine/asset_registry.hpp"
#include "engine/content_hash.hpp"
#include "engine/gltf_mesh.hpp"
#include "engine/simulation_runtime.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// This test intentionally exercises the public cooked-animation boundary only.
// The two methods used below are the production API being introduced with the
// cooked-animation artifact frontier:
//
//   AnimationRuntime::cook_gltf_animation_artifact(...)
//   AnimationRuntime::load_cooked_animation_artifact(...)
//
// Keeping this test in a separate translation unit makes the contract explicit
// while the artifact codec is implemented.  Until those declarations land, the
// test is expected to be a compile-time TODO rather than a source-format test.

namespace {

using Json = nlohmann::json;
using noemancer::AnimationCompressionMode;
using noemancer::AnimationRuntime;
using noemancer::GltfMeshData;
using noemancer::RootMotionDelta;
using noemancer::SkeletalPose;

template <typename T>
void append_scalar(std::vector<std::byte>& bytes, const T value) {
    const auto begin = bytes.size();
    bytes.resize(begin + sizeof(T));
    std::memcpy(bytes.data() + begin, &value, sizeof(T));
}

std::filesystem::path make_deterministic_skinned_glb() {
    // A two-joint triangle with one rotation channel and one root-translation
    // channel.  The payload is deliberately small, little-endian and stable so
    // a Cook recipe can be called twice and compared byte-for-byte.
    std::vector<std::byte> binary;
    for (const float value : std::array<float, 9>{
             -0.5F, 0.0F, 0.0F,
              0.5F, 0.0F, 0.0F,
              0.0F, 1.0F, 0.0F}) {
        append_scalar(binary, value);
    }
    for (const std::uint8_t value : std::array<std::uint8_t, 12>{
             0U, 0U, 0U, 0U,
             1U, 0U, 0U, 0U,
             1U, 0U, 0U, 0U}) {
        append_scalar(binary, value);
    }
    for (const float value : std::array<float, 12>{
             1.0F, 0.0F, 0.0F, 0.0F,
             1.0F, 0.0F, 0.0F, 0.0F,
             1.0F, 0.0F, 0.0F, 0.0F}) {
        append_scalar(binary, value);
    }
    for (const std::uint16_t value : std::array<std::uint16_t, 3>{0U, 1U, 2U}) {
        append_scalar(binary, value);
    }
    while (binary.size() % 4U != 0U) {
        append_scalar(binary, std::uint8_t{});
    }

    // Inverse-bind matrices: two identity matrices.
    for (int matrix = 0; matrix < 2; ++matrix) {
        for (int component = 0; component < 16; ++component) {
            append_scalar(binary, component % 5 == 0 ? 1.0F : 0.0F);
        }
    }

    // Rotation sampler: identity to a 45-degree Z rotation.
    for (const float value : std::array<float, 2>{0.0F, 1.0F}) {
        append_scalar(binary, value);
    }
    for (const float value : std::array<float, 8>{
             0.0F, 0.0F, 0.0F, 1.0F,
             0.0F, 0.0F, 0.3826834F, 0.9238795F}) {
        append_scalar(binary, value);
    }

    // Root translation sampler: zero to +1m on X.  This makes root-motion
    // preservation observable independently from the child rotation.
    for (const float value : std::array<float, 2>{0.0F, 1.0F}) {
        append_scalar(binary, value);
    }
    for (const float value : std::array<float, 6>{
             0.0F, 0.0F, 0.0F,
             1.0F, 0.0F, 0.0F}) {
        append_scalar(binary, value);
    }

    const Json document = {
        {"asset", {{"version", "2.0"}, {"generator", "noemancer.cooked-animation-test"}}},
        {"buffers", {{{"byteLength", binary.size()}}}},
        {"bufferViews", {
            {{"buffer", 0}, {"byteOffset", 0},   {"byteLength", 36}},
            {{"buffer", 0}, {"byteOffset", 36},  {"byteLength", 12}},
            {{"buffer", 0}, {"byteOffset", 48},  {"byteLength", 48}},
            {{"buffer", 0}, {"byteOffset", 96},  {"byteLength", 6}},
            {{"buffer", 0}, {"byteOffset", 104}, {"byteLength", 128}},
            {{"buffer", 0}, {"byteOffset", 232}, {"byteLength", 8}},
            {{"buffer", 0}, {"byteOffset", 240}, {"byteLength", 32}},
            {{"buffer", 0}, {"byteOffset", 272}, {"byteLength", 8}},
            {{"buffer", 0}, {"byteOffset", 280}, {"byteLength", 24}}
        }},
        {"accessors", {
            {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
            {{"bufferView", 1}, {"componentType", 5121}, {"count", 3}, {"type", "VEC4"}},
            {{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC4"}},
            {{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
            {{"bufferView", 4}, {"componentType", 5126}, {"count", 2}, {"type", "MAT4"}},
            {{"bufferView", 5}, {"componentType", 5126}, {"count", 2}, {"type", "SCALAR"}},
            {{"bufferView", 6}, {"componentType", 5126}, {"count", 2}, {"type", "VEC4"}},
            {{"bufferView", 7}, {"componentType", 5126}, {"count", 2}, {"type", "SCALAR"}},
            {{"bufferView", 8}, {"componentType", 5126}, {"count", 2}, {"type", "VEC3"}}
        }},
        {"meshes", {{{"name", "CookedTriangle"}, {"primitives", {{
            {"attributes", {{"POSITION", 0}, {"JOINTS_0", 1}, {"WEIGHTS_0", 2}}},
            {"indices", 3}, {"mode", 4}
        }}}}}},
        {"nodes", {
            {{"name", "Root"}, {"children", {1}}},
            {{"name", "Tip"}, {"translation", {0.0F, 0.5F, 0.0F}}},
            {{"name", "SkinnedTriangle"}, {"mesh", 0}, {"skin", 0}}
        }},
        {"skins", {{{"name", "CookedRig"}, {"joints", {0, 1}}, {"inverseBindMatrices", 4}, {"skeleton", 0}}}},
        {"animations", Json::array({Json{
            {"name", "RootTravelAndTipTurn"},
            {"samplers", Json::array({
                Json{{"input", 5}, {"output", 6}, {"interpolation", "LINEAR"}},
                Json{{"input", 7}, {"output", 8}, {"interpolation", "LINEAR"}}
            })},
            {"channels", Json::array({
                Json{{"sampler", 0}, {"target", {{"node", 1}, {"path", "rotation"}}}},
                Json{{"sampler", 1}, {"target", {{"node", 0}, {"path", "translation"}}}}
            })}
        }})},
        {"scenes", Json::array({Json{{"nodes", Json::array({0, 2})}}})},
        {"scene", 0}
    };

    std::string json = document.dump();
    while (json.size() % 4U != 0U) {
        json.push_back(' ');
    }

    std::vector<std::byte> glb;
    const auto total_length = static_cast<std::uint32_t>(12U + 8U + json.size() + 8U + binary.size());
    append_scalar(glb, std::uint32_t{0x46546c67U});
    append_scalar(glb, std::uint32_t{2U});
    append_scalar(glb, total_length);
    append_scalar(glb, static_cast<std::uint32_t>(json.size()));
    append_scalar(glb, std::uint32_t{0x4e4f534aU});
    const auto json_begin = glb.size();
    glb.resize(json_begin + json.size());
    std::memcpy(glb.data() + json_begin, json.data(), json.size());
    append_scalar(glb, static_cast<std::uint32_t>(binary.size()));
    append_scalar(glb, std::uint32_t{0x004e4942U});
    glb.insert(glb.end(), binary.begin(), binary.end());

    const auto path = std::filesystem::temp_directory_path() /
        "noemancer-cooked-animation-deterministic-skinned.glb";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    return path;
}

bool nearly_equal(const float left, const float right, const float epsilon = 0.0001F) {
    return std::abs(left - right) <= epsilon;
}

bool compare_pose(const SkeletalPose& reference, const SkeletalPose& candidate, const std::string_view label) {
    if (!reference.valid || !candidate.valid || reference.skinning_matrices.size() != candidate.skinning_matrices.size() ||
        reference.joints.size() != candidate.joints.size()) {
        std::cerr << label << ": pose validity or cardinality changed\n";
        return false;
    }
    for (std::size_t joint = 0; joint < reference.skinning_matrices.size(); ++joint) {
        for (std::size_t component = 0; component < reference.skinning_matrices[joint].size(); ++component) {
            if (!nearly_equal(reference.skinning_matrices[joint][component],
                              candidate.skinning_matrices[joint][component])) {
                std::cerr << label << ": skinning matrix changed at joint " << joint << " component " << component << '\n';
                return false;
            }
        }
    }
    for (std::size_t joint = 0; joint < reference.joints.size(); ++joint) {
        const auto& left = reference.joints[joint];
        const auto& right = candidate.joints[joint];
        if (left.name != right.name || left.parent != right.parent ||
            !nearly_equal(left.model_x, right.model_x) || !nearly_equal(left.model_y, right.model_y) ||
            !nearly_equal(left.model_z, right.model_z)) {
            std::cerr << label << ": model-space joint debug changed at joint " << joint << '\n';
            return false;
        }
    }
    return true;
}

bool compare_root_motion(const RootMotionDelta& reference, const RootMotionDelta& candidate,
                         const std::string_view label) {
    if (!reference.valid || !candidate.valid || !nearly_equal(reference.x, candidate.x) ||
        !nearly_equal(reference.y, candidate.y) || !nearly_equal(reference.z, candidate.z)) {
        std::cerr << label << ": root motion changed (reference=" << reference.x << ',' << reference.y << ','
                  << reference.z << ", candidate=" << candidate.x << ',' << candidate.y << ',' << candidate.z << ")\n";
        return false;
    }
    return true;
}

template <typename Payload>
std::span<const std::byte> payload_span(const Payload& payload) {
    return {reinterpret_cast<const std::byte*>(payload.data()), payload.size()};
}

template <typename Payload>
std::vector<std::byte> payload_copy(const Payload& payload) {
    std::vector<std::byte> bytes(payload.size());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), payload.data(), payload.size());
    }
    return bytes;
}

std::vector<std::byte> replace_artifact_manifest_text(const std::span<const std::byte> payload,
                                                       const std::string_view before,
                                                       const std::string_view after) {
    if(payload.size()<48U)return {};
    std::uint32_t manifest_size{};
    for(std::uint32_t index=0;index<4U;++index)
        manifest_size|=static_cast<std::uint32_t>(payload[16U+index])<<(index*8U);
    if(manifest_size>payload.size()-48U)return {};
    std::string manifest(reinterpret_cast<const char*>(payload.data()+48U),manifest_size);
    const auto position=manifest.find(before);
    if(position==std::string::npos)return {};
    manifest.replace(position,before.size(),after);
    std::vector<std::byte> changed(payload.begin(),payload.begin()+48U);
    const auto changed_size=static_cast<std::uint32_t>(manifest.size());
    for(std::uint32_t index=0;index<4U;++index)
        changed[16U+index]=static_cast<std::byte>((changed_size>>(index*8U))&0xffU);
    changed.insert(changed.end(),reinterpret_cast<const std::byte*>(manifest.data()),
        reinterpret_cast<const std::byte*>(manifest.data()+manifest.size()));
    changed.insert(changed.end(),payload.begin()+48U+manifest_size,payload.end());
    return changed;
}

std::optional<std::uint32_t> read_u32_le(const std::span<const std::byte> bytes, const std::size_t offset) {
    if(offset>bytes.size()||bytes.size()-offset<4U)return std::nullopt;
    std::uint32_t value{};
    for(std::uint32_t index=0U;index<4U;++index)
        value|=static_cast<std::uint32_t>(bytes[offset+index])<<(index*8U);
    return value;
}

std::optional<std::uint64_t> read_u64_le(const std::span<const std::byte> bytes, const std::size_t offset) {
    if(offset>bytes.size()||bytes.size()-offset<8U)return std::nullopt;
    std::uint64_t value{};
    for(std::uint32_t index=0U;index<8U;++index)
        value|=static_cast<std::uint64_t>(bytes[offset+index])<<(index*8U);
    return value;
}

template <typename Mutation>
std::vector<std::byte> mutate_animation_archive(const std::span<const std::byte> payload, Mutation mutation) {
    const auto manifest_size=read_u32_le(payload,16U);
    const auto skeleton_size=read_u64_le(payload,32U);
    const auto animation_size=read_u64_le(payload,40U);
    if(!manifest_size||!skeleton_size||!animation_size)return {};
    const auto animation_begin=48U+static_cast<std::size_t>(*manifest_size)+static_cast<std::size_t>(*skeleton_size);
    if(animation_begin>payload.size()||*animation_size>payload.size()-animation_begin)return {};
    auto changed=payload_copy(payload);
    auto animation=std::span<std::byte>(changed).subspan(animation_begin,static_cast<std::size_t>(*animation_size));
    mutation(animation);
    const auto new_hash=noemancer::sha256_bytes(std::as_bytes(animation));
    if(!new_hash.success)return {};
    std::string manifest(reinterpret_cast<const char*>(changed.data()+48U),*manifest_size);
    const auto parsed=nlohmann::json::parse(manifest,nullptr,false);
    if(!parsed.is_object())return {};
    const auto old_hash=parsed.at("sections").at("animation").at("sha256").get<std::string>();
    const auto hash_position=manifest.find(old_hash);
    if(hash_position==std::string::npos||old_hash.size()!=new_hash.value.size())return {};
    manifest.replace(hash_position,old_hash.size(),new_hash.value);
    std::memcpy(changed.data()+48U,manifest.data(),manifest.size());
    return changed;
}

} // namespace

int main() {
    std::error_code ignored;
    const auto fixture_path = make_deterministic_skinned_glb();
    const auto decoded = noemancer::decode_glb_mesh(fixture_path);
    if (!decoded.valid || decoded.skins.size() != 1U || decoded.animations.size() != 1U ||
        decoded.skins.front().joints.size() != 2U || decoded.animations.front().duration != 1.0F ||
        decoded.animations.front().channels.size() != 2U) {
        std::cerr << "Deterministic skinned GLB fixture did not decode as expected: "
                  << decoded.code << ": " << decoded.detail << '\n';
        return 10;
    }

    constexpr std::string_view asset_id = "asset.test.cooked-animation";
    constexpr std::string_view source_hash =
        "sha256:6f34d6501458f6f6509dbf8f68f0b8e558070672170dba62c68f148eda211298";
    constexpr auto recipe = AnimationCompressionMode::ozz_runtime_baseline;

    // The reference remains an ordinary source compile.  It is used only to
    // establish the fixed pose/root-motion oracle for a newly loaded artifact.
    AnimationRuntime reference_runtime;
    const auto reference = reference_runtime.compile_gltf_asset(asset_id, decoded, 0U, 0U, recipe);
    if (!reference.success || reference.clip_asset.empty()) {
        std::cerr << "Reference source compile failed: " << reference.code << ": " << reference.detail << '\n';
        return 11;
    }

    // Same source, identity and recipe in two independent runtimes must produce
    // exactly the same artifact bytes and payload hash.
    AnimationRuntime cooker_a;
    AnimationRuntime cooker_b;
    const auto cooked_a = cooker_a.cook_gltf_animation_artifact(asset_id, source_hash, decoded, 0U, 0U, recipe);
    const auto cooked_b = cooker_b.cook_gltf_animation_artifact(asset_id, source_hash, decoded, 0U, 0U, recipe);
    const auto payloads_equal = cooked_a.payload.size() == cooked_b.payload.size() &&
        (cooked_a.payload.empty() || std::memcmp(cooked_a.payload.data(), cooked_b.payload.data(), cooked_a.payload.size()) == 0);
    if (!cooked_a.success || !cooked_b.success || cooked_a.payload.empty() || cooked_b.payload.empty() ||
        cooked_a.payload_hash.empty() || cooked_a.payload_hash != cooked_b.payload_hash || !payloads_equal ||
        cooked_a.clip_assets.empty() ||
        cooked_a.clip_assets != cooked_b.clip_assets) {
        std::cerr << "Cooked artifact recipe is not deterministic: "
                  << cooked_a.code << ": " << cooked_a.detail << " / "
                  << cooked_b.code << ": " << cooked_b.detail << '\n';
        return 12;
    }

    // A fresh runtime must be able to reconstruct the clip without seeing the
    // GLB.  The returned clip asset list is the only name hand-off from the
    // artifact loader to the sampling runtime.
    AnimationRuntime loaded_runtime;
    const auto loaded = loaded_runtime.load_cooked_animation_artifact(
        payload_span(cooked_a.payload), asset_id, source_hash);
    if (!loaded.success || loaded.clip_assets.empty()) {
        std::cerr << "Fresh runtime rejected cooked artifact: " << loaded.code << ": " << loaded.detail << '\n';
        return 13;
    }
    if (loaded.clip_assets != cooked_a.clip_assets) {
        std::cerr << "Loaded artifact changed its deterministic clip asset list\n";
        return 14;
    }
    const auto loaded_clip = loaded.clip_assets.front();

    constexpr std::array<float, 5> fixed_pose_times{0.0F, 0.25F, 0.5F, 0.75F, 1.0F};
    for (std::size_t index = 0; index < fixed_pose_times.size(); ++index) {
        const auto reference_pose = reference_runtime.sample_skeletal_pose(reference.clip_asset, fixed_pose_times[index]);
        const auto loaded_pose = loaded_runtime.sample_skeletal_pose(loaded_clip, fixed_pose_times[index]);
        if (!compare_pose(reference_pose, loaded_pose, "fixed pose " + std::to_string(index))) {
            return 15;
        }
    }

    struct RootMotionSample final {
        float previous{};
        float current{};
        bool looping{};
        float playback_speed{};
    };
    constexpr std::array<RootMotionSample, 3> root_samples{{
        {0.0F, 0.5F, false, 1.0F},
        {0.75F, 0.25F, true, 1.0F},
        {0.75F, 0.25F, false, -1.0F}
    }};
    for (std::size_t index = 0; index < root_samples.size(); ++index) {
        const auto& sample = root_samples[index];
        const auto reference_root = reference_runtime.root_motion_delta(
            reference.clip_asset, sample.previous, sample.current, sample.looping, sample.playback_speed);
        const auto loaded_root = loaded_runtime.root_motion_delta(
            loaded_clip, sample.previous, sample.current, sample.looping, sample.playback_speed);
        if (!compare_root_motion(reference_root, loaded_root, "root motion " + std::to_string(index))) {
            return 16;
        }
        if (index < 2U && !nearly_equal(reference_root.x, 0.5F, 0.001F)) {
            std::cerr << "Root-motion fixture oracle is not +0.5m on X\n";
            return 17;
        }
    }

    const auto preserved_pose = loaded_runtime.sample_skeletal_pose(loaded_clip, 0.5F);
    const auto reject_without_mutation = [&](const auto& result, const std::string_view operation) {
        if (result.success || result.code.empty() || result.detail.empty()) {
            std::cerr << operation << " unexpectedly accepted malformed artifact: "
                      << result.code << ": " << result.detail << '\n';
            return false;
        }
        const auto after_rejection = loaded_runtime.sample_skeletal_pose(loaded_clip, 0.5F);
        return compare_pose(preserved_pose, after_rejection, std::string(operation) + " preserved old clip");
    };

    if (!reject_without_mutation(
            loaded_runtime.load_cooked_animation_artifact(payload_span(cooked_a.payload), "asset.test.wrong-id", source_hash),
            "wrong asset id")) {
        return 18;
    }
    if (!reject_without_mutation(
            loaded_runtime.load_cooked_animation_artifact(payload_span(cooked_a.payload), asset_id,
                                                          "sha256:wrong-source-hash"),
            "wrong source hash")) {
        return 19;
    }

    std::vector<std::byte> truncated = payload_copy(cooked_a.payload);
    truncated.resize(truncated.size() - 1U);
    if (!reject_without_mutation(
            loaded_runtime.load_cooked_animation_artifact(payload_span(truncated), asset_id, source_hash),
            "truncated payload")) {
        return 20;
    }

    std::vector<std::byte> tampered = payload_copy(cooked_a.payload);
    tampered[tampered.size() / 2U] ^= static_cast<std::byte>(0x01U);
    if (!reject_without_mutation(
            loaded_runtime.load_cooked_animation_artifact(payload_span(tampered), asset_id, source_hash),
            "tampered payload")) {
        return 21;
    }
    const auto wrong_manifest_type=replace_artifact_manifest_text(payload_span(cooked_a.payload),
        R"("jointCount":2)",R"("jointCount":[])"
    );
    const auto wrong_type_result=loaded_runtime.load_cooked_animation_artifact(
        payload_span(wrong_manifest_type),asset_id,source_hash);
    if(wrong_type_result.success||wrong_type_result.code!="animation.artifact-manifest-invalid"||
        !compare_pose(preserved_pose,loaded_runtime.sample_skeletal_pose(loaded_clip,0.5F),
            "wrong manifest type preserved old clip")) {
        std::cerr << "Malformed manifest field was not converted into a transactional structured failure\n";
        return 22;
    }
    const auto unsupported_archive=replace_artifact_manifest_text(payload_span(cooked_a.payload),
        R"("archiveVersion":2)",R"("archiveVersion":9)");
    const auto unsupported_result=loaded_runtime.load_cooked_animation_artifact(
        payload_span(unsupported_archive),asset_id,source_hash);
    if(unsupported_result.success||unsupported_result.code!="animation.artifact-archive-version-unsupported") {
        std::cerr << "Unsupported ozz serialization version was not rejected before archive decode\n";
        return 23;
    }
    const auto wrong_track_count=replace_artifact_manifest_text(payload_span(cooked_a.payload),
        R"("trackCount":2)",R"("trackCount":1)");
    const auto wrong_track_result=loaded_runtime.load_cooked_animation_artifact(
        payload_span(wrong_track_count),asset_id,source_hash);
    if(wrong_track_result.success||wrong_track_result.code!="animation.artifact-archive-invalid") {
        std::cerr << "Animation track/joint cardinality mismatch was not rejected\n";
        return 24;
    }
    const auto oversized_internal_count=mutate_animation_archive(payload_span(cooked_a.payload),
        [](const std::span<std::byte> archive) {
            // Animation v7 name length follows endian, tag, version,
            // duration and track count. A trusted-only IArchive would attempt
            // to allocate from this value before discovering the short read.
            for(std::size_t index=0U;index<4U;++index)archive[27U+index]=std::byte{0xffU};
        });
    if(!reject_without_mutation(loaded_runtime.load_cooked_animation_artifact(
            payload_span(oversized_internal_count),asset_id,source_hash),"oversized internal archive count")) {
        return 25;
    }
    const auto non_finite_duration=mutate_animation_archive(payload_span(cooked_a.payload),
        [](const std::span<std::byte> archive) {
            constexpr std::uint32_t quiet_nan=0x7fc00000U;
            for(std::uint32_t index=0U;index<4U;++index)
                archive[19U+index]=static_cast<std::byte>((quiet_nan>>(index*8U))&0xffU);
        });
    if(!reject_without_mutation(loaded_runtime.load_cooked_animation_artifact(
            payload_span(non_finite_duration),asset_id,source_hash),"non-finite archive duration")) {
        return 26;
    }

    // Close the production-facing descriptor -> Cook cache -> manifest path.
    // The raw GLB is a build input only: it must not become a scheduled output
    // or a Runtime Package dependency.
    const auto project_root=std::filesystem::temp_directory_path()/"noemancer-animation-clip-cook-test";
    const auto asset_root=project_root/"assets";
    std::filesystem::remove_all(project_root,ignored);
    std::filesystem::create_directories(asset_root,ignored);
    std::filesystem::copy_file(fixture_path,asset_root/"source.glb",
        std::filesystem::copy_options::overwrite_existing,ignored);
    {
        std::ofstream descriptor(asset_root/"walk.animation-clip.json",std::ios::binary);
        descriptor << R"({"schemaVersion":"noemancer.animation-clip/0.1","assetId":"clip.walk","sourceAsset":"source.rig","skinIndex":0,"animationIndex":0,"compression":"ozz_runtime_baseline"})";
    }
    {
        std::ofstream registry_file(asset_root/"registry.json",std::ios::binary);
        registry_file << R"({"schema":"noemancer.assets/0.1","assets":[)"
            R"({"id":"source.rig","displayName":"Cook Source","kind":"Model","uri":"asset://source.glb","path":"source.glb","license":"CC0-1.0","redistribution":"public"},)"
            R"({"id":"clip.walk","displayName":"Walk","kind":"AnimationClip","uri":"asset://walk.animation-clip.json","path":"walk.animation-clip.json","license":"CC0-1.0","redistribution":"public"}]})";
    }
    noemancer::AssetRegistry registry(asset_root);
    const auto* source_record=registry.find("source.rig");
    if(!registry.errors().empty()||source_record==nullptr||!source_record->content_hash.starts_with("sha256:")) {
        std::cerr << "Animation Clip Registry fixture failed to establish a verified source identity\n";
        return 30;
    }
    const auto plan=Json::parse(registry.cook_plan_json({"clip.walk"},"windows-x64-release"));
    if(!plan.value("valid",false)||plan.at("inputs").size()!=1U||
        plan.at("inputs").front().at("assetId")!="clip.walk"||
        !plan.at("inputs").front().contains("recipeHash")||
        plan.at("inputs").front().at("buildInputs").size()!=1U||
        plan.at("inputs").front().at("buildInputs").front().at("assetId")!="source.rig"||
        plan.at("inputs").front().at("buildInputs").front().at("license")!="CC0-1.0"||
        plan.at("inputs").front().at("buildInputs").front().at("redistribution")!="public") {
        std::cerr << "Animation Clip Cook plan leaked or lost its Cook-only build input boundary\n";
        return 31;
    }
    const auto applied=Json::parse(registry.apply_cook_plan_json(plan.dump(),false));
    const auto repeated=Json::parse(registry.apply_cook_plan_json(plan.dump(),false));
    const auto recipe_hash=plan.at("inputs").front().at("recipeHash").get<std::string>();
    const auto cache_key=recipe_hash.substr(recipe_hash.find(':')+1U);
    const auto cache_root=project_root/"generated"/"cook-cache"/cache_key;
    const auto payload_path=cache_root/"payload.animbin";
    const auto metadata_path=cache_root/"asset.json";
    if(!applied.value("success",false)||applied.at("cacheMisses")!=1U||
        !repeated.value("success",false)||repeated.at("cacheHits")!=1U||
        !std::filesystem::is_regular_file(payload_path)||!std::filesystem::is_regular_file(metadata_path)) {
        std::cerr << "Animation Clip Cook did not commit and reuse its content-addressed .animbin\n";
        return 32;
    }
    Json metadata;
    { std::ifstream input(metadata_path,std::ios::binary); metadata=Json::parse(input); }
    const auto original_payload_hash=metadata.at("payloadHash").get<std::string>();
    const auto manifest_path=project_root/"generated"/"cook-manifests"/
        (plan.at("planId").get<std::string>()+".json");
    Json manifest;
    { std::ifstream input(manifest_path,std::ios::binary); manifest=Json::parse(input); }
    if(metadata.at("payloadFormat")!="noemancer/animbin"||
        metadata.at("importedMetadata").at("buildInputs").front().at("packaged")!=false||
        metadata.at("importedMetadata").at("runtimeContract").at("sourceDecodeAtRuntime")!=false||
        metadata.at("importedMetadata").at("runtimeContract").at("offlineCompileAtRuntime")!=false||
        manifest.at("outputs").size()!=1U||manifest.at("outputs").front().at("assetId")!="clip.walk"||
        manifest.at("outputs").front().at("payloadFormat")!="noemancer/animbin"||
        manifest.at("outputs").front().at("payloadHash")!=original_payload_hash||
        manifest.at("outputs").front().at("buildInputs").front().at("redistribution")!="public") {
        std::cerr << "Cook metadata/manifest did not preserve the cooked-only Runtime contract\n";
        return 33;
    }
    {
        std::fstream payload(payload_path,std::ios::binary|std::ios::in|std::ios::out);
        payload.seekg(-1,std::ios::end);
        char byte{};payload.read(&byte,1);byte=static_cast<char>(byte^0x01);
        payload.seekp(-1,std::ios::end);payload.write(&byte,1);
    }
    const auto rebuilt=Json::parse(registry.apply_cook_plan_json(plan.dump(),false));
    const auto rebuilt_hash=noemancer::sha256_file(payload_path);
    if(!rebuilt.value("success",false)||rebuilt.at("cacheMisses")!=1U||!rebuilt_hash.success||
        rebuilt_hash.value!=original_payload_hash) {
        std::cerr << "Corrupted animation cache payload was not detected and rebuilt deterministically\n";
        return 34;
    }
    { std::ofstream source(asset_root/"source.glb",std::ios::binary|std::ios::app); source.put('\0'); }
    const auto changed_source=Json::parse(registry.apply_cook_plan_json(plan.dump(),false));
    if(changed_source.value("success",true)||changed_source.value("code",std::string{})!="asset.cook-failed"||
        changed_source.value("detail",std::string{}).find("build input changed")==std::string::npos) {
        std::cerr << "Cook accepted an animation build input that changed after planning\n";
        return 35;
    }

    std::filesystem::remove_all(project_root,ignored);
    if(const auto* requested_fixture=std::getenv("NOEMANCER_ANIMATION_FIXTURE_OUTPUT");
        requested_fixture!=nullptr&&*requested_fixture!='\0') {
        const auto output=std::filesystem::path(requested_fixture);
        std::filesystem::create_directories(output.parent_path(),ignored);
        std::filesystem::copy_file(fixture_path,output,std::filesystem::copy_options::overwrite_existing,ignored);
        if(ignored) {
            std::cerr << "Requested animation fixture evidence output could not be written\n";
            return 36;
        }
    }
    std::filesystem::remove(fixture_path,ignored);

    std::cout << "cooked animation artifact: deterministic bytes/hash, fresh-runtime pose/root equivalence, "
                 "identity/integrity rejection, Registry Cook/cache rebuild and source exclusion passed\n";
    return 0;
}
