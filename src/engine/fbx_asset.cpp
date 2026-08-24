#include "engine/fbx_asset.hpp"

#include <ufbx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noemancer {
namespace {

using ScenePtr = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
using BakedPtr = std::unique_ptr<ufbx_baked_anim, decltype(&ufbx_free_baked_anim)>;

std::string text(const ufbx_string value) {
    return std::string(value.data ? value.data : "", value.length);
}

std::array<float, 16> matrix(const ufbx_matrix& source) {
    return {
        static_cast<float>(source.cols[0].x), static_cast<float>(source.cols[0].y), static_cast<float>(source.cols[0].z), 0.0F,
        static_cast<float>(source.cols[1].x), static_cast<float>(source.cols[1].y), static_cast<float>(source.cols[1].z), 0.0F,
        static_cast<float>(source.cols[2].x), static_cast<float>(source.cols[2].y), static_cast<float>(source.cols[2].z), 0.0F,
        static_cast<float>(source.cols[3].x), static_cast<float>(source.cols[3].y), static_cast<float>(source.cols[3].z), 1.0F
    };
}

std::array<float, 4> material_color(const ufbx_material* material) {
    if (!material) return {0.72F, 0.74F, 0.78F, 1.0F};
    const auto& map = material->pbr.base_color;
    return {static_cast<float>(map.value_vec4.x), static_cast<float>(map.value_vec4.y),
        static_cast<float>(map.value_vec4.z), map.value_components >= 4 ? static_cast<float>(map.value_vec4.w) : 1.0F};
}

struct SkinMap final {
    int skin{-1};
    std::unordered_map<const ufbx_node*, std::uint16_t> joint_by_node;
    std::vector<std::uint16_t> cluster_to_joint;
};

SkinMap append_skin(DecodedSceneAsset& output, const ufbx_skin_deformer* deformer) {
    SkinMap result{};
    if (!deformer || deformer->clusters.count == 0) return result;

    std::unordered_set<const ufbx_node*> required;
    for (const auto* cluster : deformer->clusters) {
        for (auto* node = cluster->bone_node; node && !node->is_root; node = node->parent) required.insert(node);
    }
    std::vector<const ufbx_node*> ordered(required.begin(), required.end());
    const auto depth = [](const ufbx_node* node) {
        std::size_t value = 0;
        for (; node; node = node->parent) ++value;
        return value;
    };
    std::ranges::sort(ordered, [&](const ufbx_node* left, const ufbx_node* right) {
        const auto left_depth = depth(left), right_depth = depth(right);
        return left_depth == right_depth ? left->typed_id < right->typed_id : left_depth < right_depth;
    });
    if (ordered.size() > std::numeric_limits<std::uint16_t>::max()) return result;

    GltfDecodedSkin skin;
    skin.name = text(deformer->name);
    skin.joints.reserve(ordered.size());
    for (const auto* node : ordered) {
        const auto index = static_cast<std::uint16_t>(skin.joints.size());
        result.joint_by_node.emplace(node, index);
        int parent = -1;
        for (auto* ancestor = node->parent; ancestor; ancestor = ancestor->parent) {
            if (const auto found = result.joint_by_node.find(ancestor); found != result.joint_by_node.end()) {
                parent = static_cast<int>(found->second);
                break;
            }
        }
        GltfDecodedJoint joint;
        joint.name = text(node->name);
        joint.node_index = node->typed_id;
        joint.parent_joint = parent;
        joint.local_transform = matrix(node->node_to_parent);
        joint.inverse_bind_matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        skin.joints.push_back(std::move(joint));
    }
    result.cluster_to_joint.reserve(deformer->clusters.count);
    for (const auto* cluster : deformer->clusters) {
        const auto found = result.joint_by_node.find(cluster->bone_node);
        const auto joint = found == result.joint_by_node.end() ? std::uint16_t{} : found->second;
        result.cluster_to_joint.push_back(joint);
        skin.joints[joint].inverse_bind_matrix = matrix(cluster->geometry_to_bone);
    }
    result.skin = static_cast<int>(output.skins.size());
    output.skins.push_back(std::move(skin));
    return result;
}

GltfDecodedVertex decode_vertex(const ufbx_mesh* mesh, const ufbx_skin_deformer* skin,
    const SkinMap& mapping, const std::uint32_t corner) {
    GltfDecodedVertex vertex;
    const auto position = ufbx_get_vertex_vec3(&mesh->vertex_position, corner);
    const auto normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, corner);
    vertex.position = {static_cast<float>(position.x), static_cast<float>(position.y), static_cast<float>(position.z)};
    vertex.normal = {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)};
    if (mesh->vertex_uv.exists) {
        const auto uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
        vertex.texcoord = {static_cast<float>(uv.x), 1.0F - static_cast<float>(uv.y)};
    }
    if (mesh->vertex_tangent.exists) {
        const auto tangent = ufbx_get_vertex_vec3(&mesh->vertex_tangent, corner);
        vertex.tangent = {static_cast<float>(tangent.x), static_cast<float>(tangent.y), static_cast<float>(tangent.z), 1.0F};
        if (mesh->vertex_bitangent.exists) {
            const auto bitangent = ufbx_get_vertex_vec3(&mesh->vertex_bitangent, corner);
            const auto cross_x = normal.y * tangent.z - normal.z * tangent.y;
            const auto cross_y = normal.z * tangent.x - normal.x * tangent.z;
            const auto cross_z = normal.x * tangent.y - normal.y * tangent.x;
            vertex.tangent[3] = cross_x * bitangent.x + cross_y * bitangent.y + cross_z * bitangent.z < 0.0 ? -1.0F : 1.0F;
        }
    }
    if (!skin || corner >= mesh->vertex_indices.count) return vertex;
    const auto logical = mesh->vertex_indices.data[corner];
    if (logical >= skin->vertices.count) return vertex;
    const auto& influences = skin->vertices.data[logical];
    const auto count = std::min<std::uint32_t>(influences.num_weights, 4U);
    float total = 0.0F;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& weight = skin->weights.data[influences.weight_begin + index];
        if (weight.cluster_index >= mapping.cluster_to_joint.size()) continue;
        vertex.joints[index] = mapping.cluster_to_joint[weight.cluster_index];
        vertex.weights[index] = static_cast<float>(weight.weight);
        total += vertex.weights[index];
    }
    if (total > std::numeric_limits<float>::epsilon()) {
        for (auto& weight : vertex.weights) weight /= total;
    }
    return vertex;
}

void append_mesh(DecodedSceneAsset& output, const ufbx_node* node) {
    const auto* mesh = node->mesh;
    const auto* deformer = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
    const auto skin_map = append_skin(output, deformer);
    const auto material_count = std::max<std::size_t>(node->materials.count, 1U);
    std::vector<GltfDecodedPrimitive> primitives(material_count);
    for (std::size_t index = 0; index < primitives.size(); ++index) {
        auto& primitive = primitives[index];
        primitive.first_index = static_cast<std::uint32_t>(output.indices.size());
        primitive.base_color = material_color(index < node->materials.count ? node->materials.data[index] : nullptr);
        if (index < node->materials.count && node->materials.data[index]) {
            const auto* material = node->materials.data[index];
            primitive.roughness = static_cast<float>(material->pbr.roughness.value_real);
            primitive.metallic = static_cast<float>(material->pbr.metalness.value_real);
        }
        primitive.node_name = text(node->name);
        primitive.mesh_name = text(mesh->name);
        primitive.skin = skin_map.skin;
    }
    std::vector<std::uint32_t> triangle_indices(std::max<std::size_t>(mesh->max_face_triangles * 3U, 3U));
    for (std::size_t face_index = 0; face_index < mesh->faces.count; ++face_index) {
        const auto face = mesh->faces.data[face_index];
        const auto triangle_count = ufbx_triangulate_face(triangle_indices.data(), triangle_indices.size(), mesh, face);
        auto material_index = mesh->face_material.count > face_index ? mesh->face_material.data[face_index] : 0U;
        if (material_index >= primitives.size()) material_index = 0;
        auto& primitive = primitives[material_index];
        for (std::size_t index = 0; index < static_cast<std::size_t>(triangle_count) * 3U; ++index) {
            const auto corner = triangle_indices[index];
            output.vertices.push_back(decode_vertex(mesh, deformer, skin_map, corner));
            output.indices.push_back(static_cast<std::uint32_t>(output.vertices.size() - 1U));
            ++primitive.index_count;
        }
    }
    for (auto& primitive : primitives) if (primitive.index_count) output.primitives.push_back(std::move(primitive));
}

void append_channel(GltfAnimationClip& clip, const std::uint32_t node, const char* path,
    const ufbx_baked_vec3_list keys) {
    if (!keys.count) return;
    GltfAnimationChannel channel;
    channel.node_index = node; channel.path = path; channel.interpolation = "LINEAR";
    channel.times.reserve(keys.count); channel.values.reserve(keys.count);
    for (const auto& key : keys) {
        channel.times.push_back(static_cast<float>(key.time));
        channel.values.push_back({static_cast<float>(key.value.x), static_cast<float>(key.value.y), static_cast<float>(key.value.z), 0.0F});
    }
    clip.channels.push_back(std::move(channel));
}

void append_animations(DecodedSceneAsset& output, const ufbx_scene* scene) {
    for (const auto* stack : scene->anim_stacks) {
        ufbx_bake_opts options{};
        options.trim_start_time = true;
        options.resample_rate = 30.0;
        options.key_reduction_enabled = true;
        options.key_reduction_rotation = true;
        ufbx_error error{};
        BakedPtr baked(ufbx_bake_anim(scene, stack->anim, &options, &error), &ufbx_free_baked_anim);
        if (!baked) continue;
        GltfAnimationClip clip;
        clip.name = text(stack->name);
        clip.duration = static_cast<float>(baked->playback_duration);
        for (const auto& node : baked->nodes) {
            append_channel(clip, node.typed_id, "translation", node.translation_keys);
            if (node.rotation_keys.count) {
                GltfAnimationChannel channel;
                channel.node_index = node.typed_id; channel.path = "rotation"; channel.interpolation = "LINEAR";
                channel.times.reserve(node.rotation_keys.count); channel.values.reserve(node.rotation_keys.count);
                for (const auto& key : node.rotation_keys) {
                    channel.times.push_back(static_cast<float>(key.time));
                    channel.values.push_back({static_cast<float>(key.value.x), static_cast<float>(key.value.y),
                        static_cast<float>(key.value.z), static_cast<float>(key.value.w)});
                }
                clip.channels.push_back(std::move(channel));
            }
            append_channel(clip, node.typed_id, "scale", node.scale_keys);
        }
        if (!clip.channels.empty()) output.animations.push_back(std::move(clip));
    }
}

} // namespace

DecodedSceneAsset decode_fbx_asset(const std::filesystem::path& path) {
    DecodedSceneAsset output;
    ufbx_load_opts options{};
    options.strict = true;
    options.clean_skin_weights = true;
    options.generate_missing_normals = true;
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    ufbx_error error{};
    ScenePtr scene(ufbx_load_file(path.string().c_str(), &options, &error), &ufbx_free_scene);
    if (!scene) {
        output.code = "fbx.load-failed";
        output.detail = error.description.data ? text(error.description) : "ufbx could not load source";
        return output;
    }
    for (const auto* node : scene->nodes) if (node->mesh && node->visible) append_mesh(output, node);
    append_animations(output, scene.get());
    compute_decoded_scene_bounds(output);
    output.valid = !output.vertices.empty() && !output.indices.empty() && !output.primitives.empty();
    output.code = output.valid ? "ok" : "fbx.no-renderable-mesh";
    output.detail = output.valid ? "FBX normalized to runtime scene payload" : "FBX contains no renderable mesh";
    return output;
}

} // namespace noemancer
