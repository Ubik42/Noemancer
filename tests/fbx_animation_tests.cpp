#include "engine/fbx_asset.hpp"
#include "engine/process_diagnostics.hpp"
#include "engine/simulation_runtime.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>

int main() {
    noemancer::configure_process_diagnostics("test.fbx-animation");
    const auto path = std::filesystem::path(NOEMANCER_SOURCE_DIR) /
        "assets/local-test/mixamo/rumba-dancing-02.fbx";
    if (!std::filesystem::is_regular_file(path)) {
        std::cout << "Optional local Mixamo FBX fixture is unavailable; skipping.\n";
        return 77;
    }
    const auto decoded = noemancer::decode_fbx_asset(path);
    if (!decoded.valid || decoded.vertices.empty() || decoded.indices.empty() || decoded.primitives.empty() ||
        decoded.skins.size() != 1U || decoded.skins.front().joints.empty() || decoded.animations.empty()) {
        std::cerr << "FBX importer did not produce mesh, skin and animation payload: "
                  << decoded.code << " - " << decoded.detail << '\n';
        return 1;
    }
    for (const auto& vertex : decoded.vertices) {
        const float sum = vertex.weights[0] + vertex.weights[1] + vertex.weights[2] + vertex.weights[3];
        if (std::abs(sum - 1.0F) > 0.001F) {
            std::cerr << "FBX skin weights are not normalized to the runtime contract\n";
            return 2;
        }
    }
    for (const auto& primitive:decoded.primitives) if (primitive.bounds_radius<=0.0F) {
        std::cerr<<"FBX primitive bounds were not produced by the importer\n"; return 5;
    }
    noemancer::AnimationRuntime runtime;
    const auto compiled = runtime.compile_gltf_asset("asset.test.mixamo-rumba", decoded, 0U, 0U);
    const auto baseline = runtime.compile_gltf_asset("asset.test.mixamo-rumba-baseline", decoded, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_runtime_baseline);
    const auto optimized = runtime.compile_gltf_asset("asset.test.mixamo-rumba-optimized", decoded, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_hierarchical_key_reduction);
    if (!compiled.success || compiled.joint_count != decoded.skins.front().joints.size() || compiled.duration <= 0.0F) {
        std::cerr << "Normalized FBX animation was rejected by ozz: " << compiled.code
                  << " - " << compiled.detail << '\n';
        return 3;
    }
    const auto pose = runtime.sample_skeletal_pose(compiled.clip_asset, compiled.duration * 0.5F);
    if (!pose.valid || pose.skinning_matrices.size() != compiled.joint_count) {
        std::cerr << "Compiled FBX clip did not produce a complete GPU skinning palette\n";
        return 4;
    }
    const auto comparison = runtime.compare_compiled_clips(baseline.clip_asset, optimized.clip_asset, 257U);
    if (!baseline.success || !optimized.success || !optimized.compression.optimizer_applied ||
        optimized.compression.fallback_used ||
        optimized.compression.selected_runtime_archive_bytes > optimized.compression.baseline_runtime_archive_bytes ||
        optimized.compression.selected_translation_keys > optimized.compression.input_translation_keys ||
        optimized.compression.selected_rotation_keys > optimized.compression.input_rotation_keys ||
        optimized.compression.selected_scale_keys > optimized.compression.input_scale_keys ||
        !comparison.success || comparison.maximum_model_translation_error_meters > 0.0011F ||
        comparison.maximum_model_probe_error_meters > 0.0011F ||
        comparison.maximum_root_motion_delta_error_meters > 0.001F) {
        std::cerr << "FBX ozz optimization exceeded the compression/error contract: baselineArchive="
                  << optimized.compression.baseline_runtime_archive_bytes
                  << " selectedArchive=" << optimized.compression.selected_runtime_archive_bytes
                  << " keys=" << optimized.compression.input_translation_keys << '/'
                  << optimized.compression.input_rotation_keys << '/' << optimized.compression.input_scale_keys
                  << " -> " << optimized.compression.selected_translation_keys << '/'
                  << optimized.compression.selected_rotation_keys << '/' << optimized.compression.selected_scale_keys
                  << " localT=" << comparison.maximum_local_translation_error_meters
                  << " localR=" << comparison.maximum_local_rotation_error_degrees
                  << " localS=" << comparison.maximum_local_scale_error
                  << " modelT=" << comparison.maximum_model_translation_error_meters
                  << " modelProbe=" << comparison.maximum_model_probe_error_meters
                  << " skin=" << comparison.maximum_skinning_matrix_absolute_error
                  << " root=" << comparison.maximum_root_motion_delta_error_meters << '\n';
        return 6;
    }
    return 0;
}
