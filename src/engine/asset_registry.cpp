#include "engine/asset_registry.hpp"

#include "engine/fbx_asset.hpp"
#include "engine/gltf_inspector.hpp"
#include "engine/gltf_mesh.hpp"
#include "engine/asset_cook_pipeline.hpp"
#include "engine/animation_clip_asset.hpp"
#include "engine/animation_graph.hpp"
#include "engine/animation_state_machine.hpp"
#include "engine/content_hash.hpp"
#include "engine/image_decoder.hpp"
#include "engine/ktx2_cook_adapter.hpp"
#include "engine/mesh_runtime_artifact.hpp"
#include "engine/sprite_asset.hpp"
#include "engine/sprite_atlas_artifact.hpp"
#include "engine/simulation_runtime.hpp"
#include "engine/tilemap_asset.hpp"
#include "engine/vfs_document_reader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;

DecodedHdrImage decode_hdr_file(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) { DecodedHdrImage result; result.code="image.open-failed"; result.detail="Unable to open Radiance HDR asset."; return result; }
    std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
    return decode_radiance_hdr(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string normalized_relative_path(const std::filesystem::path& path) {
    return path.generic_string();
}

bool is_asset_source(const std::filesystem::path& path) {
    const auto extension = lower(path.extension().string());
    if (extension == ".glb" || extension == ".gltf" || extension == ".fbx" ||
        extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".tga" || extension == ".exr" || extension == ".hdr" ||
        extension == ".wav" || extension == ".ogg" || extension == ".flac" || extension == ".mp3" || extension == ".slang" ||
        extension == ".hlsl") return true;
    return extension == ".json" && (path.filename().string().ends_with(".scene.json") ||
        path.filename().string().ends_with(".sprite.json") || path.filename().string().ends_with(".tile-palette.json") ||
        path.filename().string().ends_with(".tilemap.json") || path.filename().string().ends_with(".animation-state-machine.json") ||
        path.filename().string().ends_with(".animation-graph.json") ||
        path.filename().string().ends_with(".animation-clip.json"));
}

bool is_sprite_asset(const AssetRecord& asset) {
    return asset.relative_path.ends_with(".sprite.json");
}

bool is_tile_palette_asset(const AssetRecord& asset){return asset.relative_path.ends_with(".tile-palette.json");}
bool is_tilemap_asset(const AssetRecord& asset){return asset.relative_path.ends_with(".tilemap.json");}
bool is_animation_state_machine_asset(const AssetRecord& asset){
    return asset.kind=="AnimationStateMachine"||asset.relative_path.ends_with(".animation-state-machine.json");
}
bool is_animation_graph_asset(const AssetRecord& asset){
    return asset.kind=="AnimationGraph"||asset.relative_path.ends_with(".animation-graph.json");
}
bool is_animation_clip_asset(const AssetRecord& asset){
    return asset.kind=="AnimationClip"||asset.relative_path.ends_with(".animation-clip.json");
}
bool has_animation_format_conflict(const AssetRecord& asset) {
    const bool graph_kind = asset.kind == "AnimationGraph";
    const bool state_machine_kind = asset.kind == "AnimationStateMachine";
    const bool graph_suffix = asset.relative_path.ends_with(".animation-graph.json");
    const bool state_machine_suffix = asset.relative_path.ends_with(".animation-state-machine.json");
    return (graph_kind && state_machine_suffix) || (state_machine_kind && graph_suffix);
}
bool is_texture_cook_source(const AssetRecord& asset){return asset.extension==".png";}

constexpr std::size_t max_animation_graph_source_bytes = 64U * 1024U * 1024U;
constexpr std::size_t max_animation_graph_string_bytes = 4096U;

struct BoundedTextRead final {
    bool opened{};
    bool too_large{};
    std::string text;
};

BoundedTextRead read_text_file_bounded(const std::filesystem::path& path, const std::size_t max_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (!size_error && size > max_bytes) return {true, true, {}};
    BoundedTextRead result{true, false, {}};
    if (!size_error && size <= static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
        result.text.reserve(static_cast<std::size_t>(size));
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        const auto chunk_size = static_cast<std::size_t>(count);
        if (chunk_size > max_bytes || result.text.size() > max_bytes - chunk_size)
            return {true, true, {}};
        result.text.append(buffer.data(), chunk_size);
    }
    if (input.bad()) return {};
    return result;
}

AnimationClipAssetParseResult parse_animation_clip_source(const std::filesystem::path& path) {
    const auto source=read_text_file_bounded(path,animation_clip_asset_max_source_bytes);
    if(!source.opened)return {std::nullopt,"animation.clip-source-unavailable",
        "Animation Clip source could not be read."};
    if(source.too_large)return {std::nullopt,"animation.clip-source-too-large",
        "Animation Clip source exceeds the 64 KiB limit."};
    return AnimationClipAssetCodec::parse_json(source.text);
}

struct AnimationGraphIssue final {
    std::string code;
    std::string detail;
};

std::optional<AnimationGraphIssue> animation_graph_string_budget_issue(
    const AnimationGraphDocument& document) {
    const auto check = [](const std::string_view field, const std::string_view value)
        -> std::optional<AnimationGraphIssue> {
        if (value.size() <= max_animation_graph_string_bytes) return std::nullopt;
        return AnimationGraphIssue{
            "animation.graph-string-too-large",
            "Animation Graph field " + std::string(field) + " exceeds the 4096-byte limit."};
    };
    if (auto issue = check("assetId", document.asset_id)) return issue;
    for (const auto& parameter : document.parameters) {
        if (auto issue = check("parameter.id", parameter.id)) return issue;
        if (auto issue = check("parameter.type", parameter.type)) return issue;
    }
    for (const auto& node : document.nodes) {
        if (auto issue = check("node.id", node.id)) return issue;
        if (auto issue = check("node.kind", node.kind)) return issue;
        if (auto issue = check("node.clipAsset", node.clip_asset)) return issue;
        if (auto issue = check("node.stateMachineAsset", node.state_machine_asset)) return issue;
        if (auto issue = check("node.parameter", node.parameter)) return issue;
        for (const auto& child : node.children)
            if (auto issue = check("node.children.nodeId", child.node_id)) return issue;
    }
    for (const auto& layer : document.layers) {
        if (auto issue = check("layer.id", layer.id)) return issue;
        if (auto issue = check("layer.rootNode", layer.root_node)) return issue;
        if (auto issue = check("layer.mode", layer.mode)) return issue;
        if (auto issue = check("layer.weightParameter", layer.weight_parameter)) return issue;
        if (auto issue = check("layer.maskId", layer.mask_id)) return issue;
        if (auto issue = check("layer.syncGroup", layer.sync_group)) return issue;
    }
    for (const auto& mask : document.masks) {
        if (auto issue = check("mask.id", mask.id)) return issue;
        for (const auto& joint : mask.joints)
            if (auto issue = check("mask.joints.name", joint.name)) return issue;
    }
    for (const auto& group : document.sync_groups) {
        if (auto issue = check("syncGroup.id", group.id)) return issue;
        if (auto issue = check("syncGroup.mode", group.mode)) return issue;
    }
    for (const auto& layout : document.editor.nodes)
        if (auto issue = check("editor.nodes.id", layout.node_id)) return issue;
    return std::nullopt;
}

AnimationGraphParseResult parse_animation_graph_source(const std::filesystem::path& path) {
    const auto source = read_text_file_bounded(path, max_animation_graph_source_bytes);
    if (!source.opened)
        return {std::nullopt, "animation.graph.source-unavailable", "Animation Graph source could not be read."};
    if (source.too_large)
        return {std::nullopt, "animation.graph-source-too-large",
            "Animation Graph source exceeds the 64 MiB limit."};
    auto parsed = AnimationGraphCodec::parse_json(source.text);
    if (!parsed) return parsed;
    if (const auto issue = animation_graph_string_budget_issue(*parsed.document))
        return {std::nullopt, issue->code, issue->detail};
    return parsed;
}

std::optional<AnimationGraphIssue> animation_graph_asset_issue(
    const AssetRecord& asset, const AnimationGraphDocument& document, const AssetRegistry* registry) {
    if (document.asset_id != asset.id)
        return AnimationGraphIssue{"animation.graph-identity-mismatch",
            "Registry asset ID and Animation Graph document assetId must match."};
    if (registry != nullptr) {
        for (const auto& node : document.nodes) {
            if (node.kind != "state-machine") continue;
            const auto* dependency = registry->find(node.state_machine_asset);
            if (dependency == nullptr) continue;
            if (has_animation_format_conflict(*dependency) || !is_animation_state_machine_asset(*dependency))
                return AnimationGraphIssue{"animation.graph-state-machine-dependency-kind-invalid",
                    "stateMachineAsset " + node.state_machine_asset +
                    " must resolve to an AnimationStateMachine asset."};
        }
    }
    return std::nullopt;
}

bool contains_token(const AssetRecord& asset,const std::string_view token) {
    auto material=lower(asset.id+" "+asset.relative_path+" "+std::accumulate(asset.tags.begin(),asset.tags.end(),std::string{},
        [](std::string value,const std::string& tag){return std::move(value)+" "+tag;}));
    std::ranges::replace_if(material,[](const unsigned char value){return !std::isalnum(value);},' ');
    std::istringstream tokens(material);std::string current;
    while(tokens>>current)if(current==token)return true;
    return false;
}

TextureCookSettings texture_settings(const AssetRecord& asset) {
    TextureCookSettings settings;settings.alpha_mode=TextureAlphaMode::blend;
    if(contains_token(asset,"normal")){settings.semantic=TextureSemantic::normal;settings.srgb=false;settings.alpha_mode=TextureAlphaMode::opaque;settings.quality=4U;}
    else if(contains_token(asset,"depth")||contains_token(asset,"mask")||contains_token(asset,"roughness")||contains_token(asset,"metallic")) {
        settings.semantic=TextureSemantic::data;settings.srgb=false;settings.alpha_mode=TextureAlphaMode::opaque;
    } else if(contains_token(asset,"emissive"))settings.semantic=TextureSemantic::emissive;
    else if(contains_token(asset,"ui"))settings.semantic=TextureSemantic::ui;
    return settings;
}

TextureCookCompression texture_compression(const TextureCookSettings& settings) {
    return settings.semantic==TextureSemantic::normal?TextureCookCompression::uastc:TextureCookCompression::basis_lz;
}

std::string cooked_payload_format(const AssetRecord& asset) {
    if(asset.uri.starts_with("builtin://"))return "builtin/json";
    if(is_animation_clip_asset(asset))return "noemancer/animbin";
    if(asset.extension==".glb"||asset.extension==".fbx")return "noemancer/meshbin/0.2";
    if(is_sprite_asset(asset))return "noemancer.sprite-atlas-artifact/0.1";
    if(is_texture_cook_source(asset))return "ktx2";
    return asset.extension;
}

std::string cooked_payload_extension(const AssetRecord& asset) {
    if(asset.uri.starts_with("builtin://"))return ".json";
    if(is_animation_clip_asset(asset))return ".animbin";
    if(asset.extension==".glb"||asset.extension==".fbx")return ".meshbin";
    if(is_texture_cook_source(asset))return ".ktx2";
    return asset.extension;
}

std::vector<std::byte> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);if(!input)return {};
    const std::vector<char> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    return {reinterpret_cast<const std::byte*>(bytes.data()),reinterpret_cast<const std::byte*>(bytes.data()+bytes.size())};
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input)return {};
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}

std::string provisional_id(const std::string_view path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : path) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "asset.unregistered." << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string fnv1a64(const std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

struct AuthoredStreamingPolicy final {
    std::string mode{"stream"};
    std::string importance{"normal"};
    std::uint32_t priority{500U};
};

AuthoredStreamingPolicy parse_authored_streaming_policy(
    const Json& asset_source, const std::string_view asset_id) {
    AuthoredStreamingPolicy result;
    if (!asset_source.contains("streamingPolicy")) return result;

    const auto& policy = asset_source.at("streamingPolicy");
    const auto prefix = std::string("Asset ") + std::string(asset_id) + " streamingPolicy";
    if (!policy.is_object()) {
        throw std::invalid_argument(prefix + " must be an object");
    }
    for (auto field = policy.begin(); field != policy.end(); ++field) {
        if (field.key() != "mode" && field.key() != "importance" && field.key() != "priority") {
            throw std::invalid_argument(prefix + " contains unknown field: " + field.key());
        }
    }
    if (!policy.contains("mode") || !policy.at("mode").is_string()) {
        throw std::invalid_argument(prefix + ".mode must be one of: stream, resident");
    }
    result.mode = policy.at("mode").get<std::string>();
    if (result.mode != "stream" && result.mode != "resident") {
        throw std::invalid_argument(prefix + ".mode must be one of: stream, resident");
    }
    if (!policy.contains("importance") || !policy.at("importance").is_string()) {
        throw std::invalid_argument(prefix + ".importance must be one of: low, normal, high, critical");
    }
    result.importance = policy.at("importance").get<std::string>();
    if (result.importance != "low" && result.importance != "normal" &&
        result.importance != "high" && result.importance != "critical") {
        throw std::invalid_argument(prefix + ".importance must be one of: low, normal, high, critical");
    }
    if (!policy.contains("priority") || !policy.at("priority").is_number_unsigned()) {
        throw std::invalid_argument(prefix + ".priority must be an integer in the range 0..1000");
    }
    const auto priority = policy.at("priority").get<std::uint64_t>();
    if (priority > 1000U) {
        throw std::invalid_argument(prefix + ".priority must be an integer in the range 0..1000");
    }
    result.priority = static_cast<std::uint32_t>(priority);
    return result;
}

std::string next_cook_operation_id() {
    static std::atomic<std::uint64_t> counter{0};
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream output;
    output << "cook_op_" << std::hex << timestamp << '-' << std::dec << ++counter;
    return output.str();
}

bool write_atomic(const std::filesystem::path& destination, const std::string_view content, std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    static std::atomic<std::uint64_t> temporary_sequence{};
    const auto temporary=destination.parent_path()/(destination.filename().string()+".noemancer-"+
        std::to_string(++temporary_sequence)+".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not open temporary output: " + temporary.string();
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            error = "Could not write temporary output: " + temporary.string();
            return false;
        }
    }
#ifdef _WIN32
    if(MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0)return true;
    error="Atomic replacement failed with Win32 error "+std::to_string(GetLastError());
#else
    std::filesystem::rename(temporary,destination,filesystem_error);if(!filesystem_error)return true;
    error=filesystem_error.message();
#endif
    std::filesystem::remove(temporary,filesystem_error);return false;
}

bool write_atomic_bytes(const std::filesystem::path& destination,
    const std::span<const std::byte> content, std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    static std::atomic<std::uint64_t> temporary_sequence{};
    const auto temporary = destination.parent_path() / (destination.filename().string() + ".noemancer-" +
        std::to_string(++temporary_sequence) + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not open temporary output: " + temporary.string();
            return false;
        }
        output.write(reinterpret_cast<const char*>(content.data()),
            static_cast<std::streamsize>(content.size()));
        if (!output) {
            error = "Could not write temporary output: " + temporary.string();
            return false;
        }
    }
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) return true;
    error = "Atomic replacement failed with Win32 error " + std::to_string(GetLastError());
#else
    std::filesystem::rename(temporary, destination, filesystem_error);
    if (!filesystem_error) return true;
    error = filesystem_error.message();
#endif
    std::filesystem::remove(temporary, filesystem_error);
    return false;
}

class ScopedCookInputSnapshot final {
public:
    explicit ScopedCookInputSnapshot(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedCookInputSnapshot() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.parent_path(), error);
    }
    ScopedCookInputSnapshot(const ScopedCookInputSnapshot&) = delete;
    ScopedCookInputSnapshot& operator=(const ScopedCookInputSnapshot&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::uint32_t rotate_right(const std::uint32_t value, const std::uint32_t count) {
    return (value >> count) | (value << (32U - count));
}

std::string sha256_file_value(const std::filesystem::path& path) {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::vector<char> source_bytes(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>{});
    std::vector<std::uint8_t> bytes(source_bytes.begin(), source_bytes.end());
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8ULL;
    bytes.push_back(0x80U);
    while ((bytes.size() % 64U) != 56U) bytes.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }

    std::array<std::uint32_t, 8> hash{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto base = offset + index * 4U;
            words[index] = (static_cast<std::uint32_t>(bytes[base]) << 24U) |
                (static_cast<std::uint32_t>(bytes[base + 1]) << 16U) |
                (static_cast<std::uint32_t>(bytes[base + 2]) << 8U) |
                static_cast<std::uint32_t>(bytes[base + 3]);
        }
        for (std::size_t index = 16U; index < 64U; ++index) {
            const auto a = words[index - 15U];
            const auto b = words[index - 2U];
            const auto s0 = rotate_right(a, 7U) ^ rotate_right(a, 18U) ^ (a >> 3U);
            const auto s1 = rotate_right(b, 17U) ^ rotate_right(b, 19U) ^ (b >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        auto a = hash[0]; auto b = hash[1]; auto c = hash[2]; auto d = hash[3];
        auto e = hash[4]; auto f = hash[5]; auto g = hash[6]; auto h = hash[7];
        for (std::size_t index = 0; index < 64U; ++index) {
            const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temporary1 = h + sum1 + choose + constants[index] + words[index];
            const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temporary1;
            d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    }
    std::ostringstream output;
    output << "sha256:" << std::hex << std::setfill('0');
    for (const auto value : hash) output << std::setw(8) << value;
    return output.str();
}

Json asset_json(const AssetRecord& asset) {
    return {
        {"id", asset.id},
        {"displayName", asset.display_name},
        {"kind", asset.kind},
        {"uri", asset.uri},
        {"sourceRoot", asset.source_root},
        {"relativePath", asset.relative_path},
        {"extension", asset.extension},
        {"contentHash", asset.content_hash},
        {"hashProvenance", asset.hash_provenance},
        {"license", asset.license},
        {"redistribution", asset.redistribution},
        {"importState", asset.import_state},
        {"sourceBytes", asset.source_bytes},
        {"optional", asset.optional},
        {"available", asset.available},
        {"streamingPolicy", {
            {"mode", asset.streaming_mode},
            {"importance", asset.streaming_importance},
            {"priority", asset.streaming_priority}
        }},
        {"tags", asset.tags},
        {"dependencies", asset.dependencies}
    };
}

} // namespace

AssetRegistry::AssetRegistry(std::filesystem::path asset_root)
    : asset_root_(std::filesystem::absolute(std::move(asset_root)).lexically_normal()),
      asset_roots_{asset_root_}, registry_sources_(1U) {
    static_cast<void>(refresh());
}

AssetRegistry::AssetRegistry(std::filesystem::path asset_root, std::string registry_json)
    : asset_root_(std::filesystem::absolute(std::move(asset_root)).lexically_normal()),
      asset_roots_{asset_root_}, registry_sources_(1U) {
    registry_sources_.front().json = std::move(registry_json);
    static_cast<void>(refresh());
}

AssetRegistry::AssetRegistry(std::filesystem::path asset_root,
    std::shared_ptr<const VirtualFileSystem> vfs,
    std::string registry_uri, const std::size_t byte_budget)
    : asset_root_(std::filesystem::absolute(std::move(asset_root)).lexically_normal()),
      asset_roots_{asset_root_}, registry_sources_(1U) {
    registry_sources_.front().vfs = std::move(vfs);
    registry_sources_.front().uri = std::move(registry_uri);
    registry_sources_.front().byte_budget = byte_budget;
    if (!registry_sources_.front().vfs)
        registry_sources_.front().error = "asset.registry-read-failed: asset.registry-vfs-missing: VFS authority is required";
    static_cast<void>(refresh());
}

bool AssetRegistry::refresh() {
    records_.clear();
    errors_.clear();
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> paths;
    for(std::size_t root_index = 0; root_index < asset_roots_.size(); ++root_index) {
        const auto& root = asset_roots_[root_index];
        std::error_code root_error;
        if(!std::filesystem::is_directory(root,root_error)) {errors_.push_back("Asset root is missing: "+root.string());continue;}
        try {
          std::optional<Json> manifest;
          const auto& registry_source = registry_sources_.at(root_index);
          if (registry_source.vfs) {
            const auto read = read_vfs_document(*registry_source.vfs, VfsDocumentReadRequest{
                .uri = registry_source.uri,
                .kind = VfsDocumentKind::json,
                .byte_budget = registry_source.byte_budget,
                .expected_schema = std::string("noemancer.assets/0.1")});
            if (!read.success) {
                errors_.push_back(root.generic_string() + ": asset.registry-read-failed: " +
                    read.code + ": " + read.detail);
            } else {
                auto document = Json::parse(read.canonical_json, nullptr, false);
                if (document.is_discarded())
                    throw std::invalid_argument("asset.registry-json-invalid: Registry manifest is not valid JSON");
                manifest = std::move(document);
            }
          } else if (registry_source.error) {
            errors_.push_back(root.generic_string() + ": " + *registry_source.error);
          } else if (registry_source.json) {
            auto document = Json::parse(*registry_source.json, nullptr, false);
            if (document.is_discarded())
                throw std::invalid_argument("asset.registry-json-invalid: Registry manifest is not valid JSON");
            manifest = std::move(document);
          } else {
            std::ifstream input(root/"registry.json",std::ios::binary);
            if (input) {
                auto document = Json::parse(input, nullptr, false);
                if (document.is_discarded())
                    throw std::invalid_argument("asset.registry-json-invalid: Registry manifest is not valid JSON");
                manifest = std::move(document);
            }
          }
          if(manifest) {
            const auto& document = *manifest;
            if(document.value("schema",std::string{})!="noemancer.assets/0.1"||!document.contains("assets")||!document.at("assets").is_array())
                throw std::invalid_argument("asset.registry-schema-invalid: Registry manifest must use noemancer.assets/0.1 and contain an assets array");
            for (const auto& source : document.at("assets")) {
            const auto asset_id = source.at("id").get<std::string>();
            const auto streaming_policy = parse_authored_streaming_policy(source, asset_id);
            AssetRecord asset{
                .id = asset_id,
                .display_name = source.at("displayName").get<std::string>(),
                .kind = source.at("kind").get<std::string>(),
                .uri = source.at("uri").get<std::string>(),
                .source_root = root.generic_string(),
                .relative_path = source.value("path", std::string{}),
                .extension = lower(std::filesystem::path(source.value("path", std::string{})).extension().string()),
                .content_hash = source.value("contentHash", std::string{}),
                .hash_provenance = source.value("contentHash", std::string{}).empty() ? "computed" : "manifest",
                .license = source.value("license", "unknown"),
                .redistribution = source.value("redistribution", "unknown"),
                .optional = source.value("optional", false),
                .streaming_mode = streaming_policy.mode,
                .streaming_importance = streaming_policy.importance,
                .streaming_priority = streaming_policy.priority,
                .tags = source.value("tags", std::vector<std::string>{}),
                .dependencies = source.value("dependencies", std::vector<std::string>{})
            };
            if (!ids.insert(asset.id).second) throw std::invalid_argument("Duplicate asset ID: " + asset.id);
            const auto rooted_path=root.generic_string()+"|"+asset.relative_path;
            if (!asset.relative_path.empty() && !paths.insert(rooted_path).second) {
                throw std::invalid_argument("Duplicate asset path: " + asset.relative_path);
            }
            if (asset.uri.starts_with("builtin://")) {
                asset.available = true;
                asset.import_state = "ready";
                asset.hash_provenance = "builtin-version";
                if (asset.content_hash.empty()) asset.content_hash = fnv1a64("builtin:noemancer-0.2:"+asset.uri);
            } else {
                const auto source_path = root / std::filesystem::path(asset.relative_path);
                std::error_code error;
                asset.available = std::filesystem::is_regular_file(source_path, error);
                asset.source_bytes = asset.available ? std::filesystem::file_size(source_path, error) : 0;
                if (!asset.available) {
                    asset.import_state = asset.optional ? "missing-optional" : "missing";
                    if (!asset.optional) errors_.push_back("Required asset is missing: " + asset.relative_path);
                } else {
                    asset.import_state = "ready";
                }
                if (asset.content_hash.empty() && asset.available) {
                    asset.content_hash = sha256_file_value(source_path);
                }
                if (asset.available && is_animation_graph_asset(asset)) {
                    asset.content_hash = sha256_file_value(source_path);
                    asset.hash_provenance = "computed";
                }
            }
            records_.push_back(std::move(asset));
            }
          }
        std::error_code scan_error;
        for (std::filesystem::recursive_directory_iterator iterator(
                 root,
                 std::filesystem::directory_options::skip_permission_denied,
                 scan_error), end;
             iterator != end;
             iterator.increment(scan_error)) {
            if (scan_error) {
                errors_.push_back("Asset scan error: " + scan_error.message());
                scan_error.clear();
                continue;
            }
            if (!iterator->is_regular_file() || !is_asset_source(iterator->path())) continue;
            const auto relative = normalized_relative_path(
                std::filesystem::relative(iterator->path(), root, scan_error));
            if (scan_error) {
                scan_error.clear();
                continue;
            }
            const auto rooted_path=root.generic_string()+"|"+relative;
            if (paths.contains(rooted_path)) continue;
            const auto extension = lower(iterator->path().extension().string());
            auto id=provisional_id(relative);
            if(ids.contains(id)) id=provisional_id(root.filename().generic_string()+"/"+relative);
            for(std::size_t suffix=2;ids.contains(id);++suffix) id=provisional_id(root.filename().generic_string()+"/"+relative+"/"+std::to_string(suffix));
            ids.insert(id);paths.insert(rooted_path);
            records_.push_back({
                .id = std::move(id),
                .display_name = iterator->path().stem().string(),
                .kind = "Unregistered",
                .uri = "asset://" + relative,
                .source_root = root.generic_string(),
                .relative_path = relative,
                .extension = extension,
                .content_hash = sha256_file_value(iterator->path()),
                .hash_provenance = "computed",
                .license = "unknown",
                .redistribution = "unknown",
                .import_state = "unregistered",
                .source_bytes = iterator->file_size(scan_error),
                .available = true,
                .tags = {"unregistered"}
            });
        }
        } catch(const std::exception& error) {errors_.push_back(root.generic_string()+": "+error.what());}
    }
    for (auto& asset : records_) {
        if (has_animation_format_conflict(asset)) {
            asset.import_state = "invalid";
            errors_.push_back(asset.source_root + ": asset.animation-format-conflict: kind and filename suffix disagree for " + asset.id);
            asset.dependencies.clear();
            continue;
        }
        if (!asset.available) continue;
        if(is_animation_graph_asset(asset)) {
            const auto parsed=parse_animation_graph_source(source_path(asset));
            if (!parsed) {
                asset.import_state = "invalid";
                asset.dependencies.clear();
                errors_.push_back(asset.source_root + ": " + parsed.code + ": " + parsed.detail);
            } else if (const auto issue = animation_graph_asset_issue(asset, *parsed.document, this)) {
                asset.import_state = "invalid";
                asset.dependencies.clear();
                errors_.push_back(asset.source_root + ": " + issue->code + ": " + issue->detail);
            } else {
                asset.dependencies=AnimationGraphCodec::asset_dependencies(*parsed.document);
            }
        } else if(is_animation_state_machine_asset(asset)) {
            const auto parsed=AnimationStateMachineCodec::parse_json(read_text_file(source_path(asset)));
            asset.dependencies=parsed?AnimationStateMachineCodec::asset_dependencies(*parsed.document):std::vector<std::string>{};
        } else if(is_sprite_asset(asset)) {
            const auto parsed=SpriteAssetCodec::parse_json(read_text_file(source_path(asset)));
            if(!parsed) {
                asset.import_state="invalid";
                asset.dependencies.clear();
                errors_.push_back(asset.source_root+": sprite.invalid-document: "+asset.id);
            } else {
                asset.dependencies=SpriteAssetCodec::asset_dependencies(*parsed.document);
            }
        }
    }
    std::ranges::sort(records_, {}, &AssetRecord::id);
    ++revision_;
    return errors_.empty();
}

bool AssetRegistry::add_root(std::filesystem::path asset_root) {
    asset_root=std::filesystem::absolute(std::move(asset_root)).lexically_normal();
    if(std::ranges::find(asset_roots_,asset_root)!=asset_roots_.end()) return true;
    asset_roots_.push_back(std::move(asset_root));
    registry_sources_.emplace_back();
    return refresh();
}

bool AssetRegistry::add_root_from_vfs(std::filesystem::path asset_root,
    std::shared_ptr<const VirtualFileSystem> vfs, std::string registry_uri,
    const std::size_t byte_budget) {
    asset_root = std::filesystem::absolute(std::move(asset_root)).lexically_normal();
    const auto existing = std::ranges::find(asset_roots_, asset_root);
    RegistrySource source;
    source.vfs = std::move(vfs);
    source.uri = std::move(registry_uri);
    source.byte_budget = byte_budget;
    if (!source.vfs)
        source.error = "asset.registry-read-failed: asset.registry-vfs-missing: VFS authority is required";
    if (existing == asset_roots_.end()) {
        asset_roots_.push_back(std::move(asset_root));
        registry_sources_.push_back(std::move(source));
    } else {
        registry_sources_.at(static_cast<std::size_t>(std::distance(asset_roots_.begin(), existing))) = std::move(source);
    }
    return refresh();
}

const std::vector<AssetRecord>& AssetRegistry::records() const noexcept { return records_; }
const std::vector<std::string>& AssetRegistry::errors() const noexcept { return errors_; }
std::uint64_t AssetRegistry::revision() const noexcept { return revision_; }
const std::filesystem::path& AssetRegistry::asset_root() const noexcept { return asset_root_; }
const std::vector<std::filesystem::path>& AssetRegistry::asset_roots() const noexcept { return asset_roots_; }
std::filesystem::path AssetRegistry::source_path(const AssetRecord& asset) const {
    const auto root=asset.source_root.empty()?asset_root_:std::filesystem::path(asset.source_root);
    return (root/asset.relative_path).lexically_normal();
}

AnimationGraphSourceValidation AssetRegistry::validate_animation_graph_source(
    const std::string_view asset_id,const std::string_view source) const {
    const auto* asset=find(asset_id);
    if(asset==nullptr||!asset->available||!is_animation_graph_asset(*asset))
        return {false,"animation.graph-source-unavailable","The Animation Graph asset source is unavailable."};
    if(source.size()>max_animation_graph_source_bytes)
        return {false,"animation.graph-source-too-large","Animation Graph source exceeds the 64 MiB limit."};
    const auto parsed=AnimationGraphCodec::parse_json(source);
    if(!parsed)return {false,parsed.code,parsed.detail};
    if(const auto issue=animation_graph_string_budget_issue(*parsed.document))
        return {false,issue->code,issue->detail};
    if(const auto issue=animation_graph_asset_issue(*asset,*parsed.document,this))
        return {false,issue->code,issue->detail};
    return {true,"ok","Animation Graph source passed Registry identity, dependency-kind, and budget validation."};
}

AnimationGraphSourceResult AssetRegistry::read_animation_graph_source(const std::string_view asset_id) const {
    const auto* asset=find(asset_id);
    if(asset==nullptr||!asset->available||!is_animation_graph_asset(*asset))
        return {false,"animation.graph-source-unavailable","The Animation Graph asset source is unavailable.",{}};
    const auto bounded=read_text_file_bounded(source_path(*asset),max_animation_graph_source_bytes);
    if(!bounded.opened)return {false,"animation.graph.source-unavailable","Animation Graph source could not be read.",{}};
    if(bounded.too_large)return {false,"animation.graph-source-too-large","Animation Graph source exceeds the 64 MiB limit.",{}};
    const auto validation=validate_animation_graph_source(asset_id,bounded.text);
    if(!validation.valid)return {false,validation.code,validation.detail,{}};
    return {true,"ok","Animation Graph source passed bounded Registry validation.",bounded.text};
}

AssetSourceEditReceipt AssetRegistry::commit_text_source(const std::string_view asset_id,
    const std::string_view replacement,const std::string_view manager,const std::string_view expected_source) {
    const auto* asset=find(asset_id);
    if(asset==nullptr||!asset->available)return {false,"asset.source-unavailable","The asset source is unavailable.",std::string(asset_id),{},std::string(manager),revision_};
    if(replacement.size()>64U*1024U*1024U)return {false,"asset.source-too-large","Text asset edits are limited to 64 MiB.",
        std::string(asset_id),source_path(*asset).generic_string(),std::string(manager),revision_};
    const auto source=source_path(*asset);
    std::string before;
    bool before_opened = true;
    bool before_too_large = false;
    if (is_animation_graph_asset(*asset)) {
        const auto bounded = read_text_file_bounded(source, max_animation_graph_source_bytes);
        before = bounded.text;
        before_opened = bounded.opened;
        before_too_large = bounded.too_large;
    } else {
        before = read_text_file(source);
    }
    if (before_too_large) return {false,"animation.graph-source-too-large","Animation Graph source exceeds the 64 MiB limit.",
        std::string(asset_id),source.generic_string(),std::string(manager),revision_};
    if(!before_opened || (before.empty()&&asset->source_bytes!=0))return {false,"asset.source-read-failed","The current source could not be read.",
        std::string(asset_id),source.generic_string(),std::string(manager),revision_};
    if(!expected_source.empty()&&before!=expected_source)return {false,"asset.source-conflict",
        "The source changed after the authoring plan was created; refusing to overwrite it.",
        std::string(asset_id),source.generic_string(),std::string(manager),revision_};
    if(before==replacement)return {true,"asset.source-unchanged","The canonical source already matches the requested content.",
        std::string(asset_id),source.generic_string(),std::string(manager),revision_};
    std::string error;if(!write_atomic(source,replacement,error))return {false,"asset.source-write-failed",error,
        std::string(asset_id),source.generic_string(),std::string(manager),revision_};
    const auto transaction_id=next_source_transaction_id_++;
    source_undo_stack_.push_back({std::string(asset_id),source,std::move(before),std::string(replacement),std::string(manager),transaction_id});
    source_redo_stack_.clear();
    constexpr std::size_t history_budget=64U*1024U*1024U;
    const auto history_bytes=[&] {std::size_t bytes{};for(const auto& edit:source_undo_stack_)bytes+=edit.before.size()+edit.after.size();return bytes;};
    while(source_undo_stack_.size()>128||(source_undo_stack_.size()>1&&history_bytes()>history_budget))source_undo_stack_.erase(source_undo_stack_.begin());
    static_cast<void>(refresh());
    return {true,"ok","Text asset source committed with reversible history.",std::string(asset_id),source.generic_string(),std::string(manager),revision_,transaction_id};
}

AssetSourceEditReceipt AssetRegistry::apply_source_history(SourceEdit edit,const bool undo,const std::string_view manager) {
    std::error_code source_error;if(!std::filesystem::is_regular_file(edit.source,source_error))
        return {false,"asset.history-conflict","The source was removed or replaced outside this history; refusing to recreate it.",
            edit.asset_id,edit.source.generic_string(),std::string(manager),revision_,edit.transaction_id};
    std::string current;
    bool current_opened = true;
    bool current_too_large = false;
    if (const auto* asset = find(edit.asset_id); asset != nullptr && is_animation_graph_asset(*asset)) {
        const auto bounded = read_text_file_bounded(edit.source, max_animation_graph_source_bytes);
        current = bounded.text;
        current_opened = bounded.opened;
        current_too_large = bounded.too_large;
    } else {
        current = read_text_file(edit.source);
    }
    if (current_too_large || !current_opened)
        return {false,current_too_large?"animation.graph-source-too-large":"asset.history-conflict",
            current_too_large?"Animation Graph source exceeds the 64 MiB limit.":"The source could not be read.",
            edit.asset_id,edit.source.generic_string(),std::string(manager),revision_,edit.transaction_id};
    const auto& expected=undo?edit.after:edit.before;const auto& replacement=undo?edit.before:edit.after;
    if(current!=expected)return {false,"asset.history-conflict","The source changed outside this history; refusing to overwrite it.",
        edit.asset_id,edit.source.generic_string(),std::string(manager),revision_,edit.transaction_id};
    std::string error;if(!write_atomic(edit.source,replacement,error))return {false,"asset.source-write-failed",error,
        edit.asset_id,edit.source.generic_string(),std::string(manager),revision_,edit.transaction_id};
    static_cast<void>(refresh());
    return {true,"ok",undo?"Text asset edit undone.":"Text asset edit redone.",edit.asset_id,edit.source.generic_string(),std::string(manager),revision_,edit.transaction_id};
}

AssetSourceEditReceipt AssetRegistry::undo_text_source(const std::string_view manager) {
    if(source_undo_stack_.empty())return {false,"asset.undo-empty","No text asset edit is available to undo.",{},{},std::string(manager),revision_};
    auto edit=source_undo_stack_.back();const auto receipt=apply_source_history(edit,true,manager);
    if(receipt.success){source_undo_stack_.pop_back();source_redo_stack_.push_back(std::move(edit));}return receipt;
}

AssetSourceEditReceipt AssetRegistry::redo_text_source(const std::string_view manager) {
    if(source_redo_stack_.empty())return {false,"asset.redo-empty","No text asset edit is available to redo.",{},{},std::string(manager),revision_};
    auto edit=source_redo_stack_.back();const auto receipt=apply_source_history(edit,false,manager);
    if(receipt.success){source_redo_stack_.pop_back();source_undo_stack_.push_back(std::move(edit));}return receipt;
}

AssetSourceEditReceipt AssetRegistry::rollback_text_source(const std::uint64_t transaction_id,
    const std::string_view manager) {
    if(transaction_id==0U)return {false,"asset.rollback-conflict","A valid source transaction ID is required.",
        {},{},std::string(manager),revision_,transaction_id};
    if(source_undo_stack_.empty())return {false,"asset.rollback-conflict","The requested source transaction is not the current undo entry.",
        {},{},std::string(manager),revision_,transaction_id};
    const auto& edit=source_undo_stack_.back();
    if(edit.transaction_id!=transaction_id)return {false,"asset.rollback-conflict",
        "Only the current undo-stack transaction may be rolled back.",edit.asset_id,edit.source.generic_string(),
        std::string(manager),revision_,transaction_id};
    auto receipt=apply_source_history(edit,true,manager);
    if(!receipt.success) {
        if(receipt.code=="asset.history-conflict")receipt.code="asset.rollback-conflict";
        return receipt;
    }
    receipt.detail="Text asset edit rolled back.";
    source_undo_stack_.pop_back();
    return receipt;
}

bool AssetRegistry::can_undo_text_source() const noexcept{return !source_undo_stack_.empty();}
bool AssetRegistry::can_redo_text_source() const noexcept{return !source_redo_stack_.empty();}

const AssetRecord* AssetRegistry::find(const std::string_view asset_id) const noexcept {
    const auto found = std::ranges::find(records_, asset_id, &AssetRecord::id);
    return found == records_.end() ? nullptr : &*found;
}

std::string AssetRegistry::registry_json() const {
    Json assets = Json::array();
    for (const auto& asset : records_) assets.push_back(asset_json(asset));
    return Json{
        {"schemaVersion", "0.1"},
        {"revision", revision_},
        {"rootUri", "asset://"},
        {"sourceRoots",asset_roots_},
        {"assetCount", records_.size()},
        {"sourceHistory",{{"canUndo",can_undo_text_source()},{"canRedo",can_redo_text_source()},
            {"undoDepth",source_undo_stack_.size()},{"redoDepth",source_redo_stack_.size()},{"conflictPolicy","exact-source-match"}}},
        {"errorCount", errors_.size()},
        {"errors", errors_},
        {"assets", std::move(assets)}
    }.dump();
}

std::string AssetRegistry::query_json(const AssetQuery& query) const {
    const auto search = lower(query.text);
    std::vector<const AssetRecord*> matches;
    for (const auto& asset : records_) {
        if (!query.kind.empty() && asset.kind != query.kind) continue;
        if (!query.import_state.empty() && asset.import_state != query.import_state) continue;
        if (!search.empty() && lower(asset.display_name).find(search) == std::string::npos &&
            lower(asset.relative_path).find(search) == std::string::npos) continue;
        bool has_tags = true;
        for (const auto& tag : query.tags) {
            if (std::ranges::find(asset.tags, tag) == asset.tags.end()) {
                has_tags = false;
                break;
            }
        }
        if (has_tags) matches.push_back(&asset);
    }
    const auto cursor = std::min(query.cursor, matches.size());
    const auto end = std::min(matches.size(), cursor + std::clamp<std::size_t>(query.limit, 1, 256));
    Json assets = Json::array();
    for (auto index = cursor; index < end; ++index) assets.push_back(asset_json(*matches[index]));
    return Json{
        {"schemaVersion", "0.1"},
        {"revision", revision_},
        {"total", matches.size()},
        {"cursor", cursor},
        {"nextCursor", end < matches.size() ? Json(end) : Json(nullptr)},
        {"assets", std::move(assets)}
    }.dump();
}

std::string AssetRegistry::inspect_json(const std::string_view asset_id) const {
    const auto* asset = find(asset_id);
    if (asset == nullptr) {
        return Json{
            {"schemaVersion", "0.1"},
            {"valid", false},
            {"code", "asset.not-found"},
            {"assetId", asset_id},
            {"asset", nullptr},
            {"importedMetadata", nullptr}
        }.dump();
    }
    Json inspection = {
        {"schemaVersion", "0.1"},
        {"valid", asset->available},
        {"code", asset->available ? "ok" : "asset.source-unavailable"},
        {"asset", asset_json(*asset)}
    };
    if (has_animation_format_conflict(*asset)) {
        inspection["valid"] = false;
        inspection["code"] = "asset.animation-format-conflict";
        inspection["importedMetadata"] = {{"detail", "Animation asset kind and filename suffix disagree."}};
        inspection["renderPayload"] = nullptr;
    } else if (asset->available && asset->extension == ".glb") {
        inspection["importedMetadata"] = Json::parse(
            gltf_summary_json(inspect_glb(source_path(*asset))));
        inspection["valid"] = inspection["importedMetadata"].value("valid", false);
        inspection["code"] = inspection["importedMetadata"].value("code", "gltf.invalid");
        const auto mesh = decode_glb_mesh(source_path(*asset));
        Json primitive_bounds=Json::array();
        for (const auto& primitive:mesh.primitives) primitive_bounds.push_back(
            {{"center",primitive.bounds_center},{"radius",primitive.bounds_radius},{"skinnedInflation",primitive.skin>=0?1.5:1.0}});
        inspection["renderPayload"] = {
            {"valid", mesh.valid}, {"code", mesh.code}, {"detail", mesh.detail},
            {"vertexCount", mesh.vertices.size()}, {"indexCount", mesh.indices.size()},
            {"primitiveCount", mesh.primitives.size()},
            {"primitiveBounds",primitive_bounds},
            {"skinCount", mesh.skins.size()},
            {"jointCount", std::accumulate(mesh.skins.begin(), mesh.skins.end(), std::size_t{},
                [](const std::size_t total, const GltfDecodedSkin& skin) { return total + skin.joints.size(); })},
            {"animationCount", mesh.animations.size()},
            {"animationChannelCount", std::accumulate(mesh.animations.begin(), mesh.animations.end(), std::size_t{},
                [](const std::size_t total, const GltfAnimationClip& clip) { return total + clip.channels.size(); })},
            {"decodedImageCount", std::ranges::count_if(mesh.images, [](const GltfDecodedImage& image) { return image.valid; })},
            {"texturedPrimitiveCount", std::ranges::count_if(mesh.primitives, [](const GltfDecodedPrimitive& primitive) { return primitive.base_color_image >= 0; })},
            {"normalMappedPrimitiveCount", std::ranges::count_if(mesh.primitives, [](const GltfDecodedPrimitive& primitive) { return primitive.normal_image >= 0; })},
            {"metallicRoughnessMappedPrimitiveCount", std::ranges::count_if(mesh.primitives, [](const GltfDecodedPrimitive& primitive) { return primitive.metallic_roughness_image >= 0; })},
            {"occlusionMappedPrimitiveCount", std::ranges::count_if(mesh.primitives, [](const GltfDecodedPrimitive& primitive) { return primitive.occlusion_image >= 0; })},
            {"emissiveMappedPrimitiveCount", std::ranges::count_if(mesh.primitives, [](const GltfDecodedPrimitive& primitive) { return primitive.emissive_image >= 0; })},
            {"features", {"sceneNodeTransforms", "normals", "generatedNormals", "texcoord0", "materialFactors", "unlit", "embeddedPng", "srgbBaseColor",
                "tangentSpace", "normalMap", "metallicRoughnessMap", "occlusionMap", "emissiveMap", "alphaMask", "doubleSided",
                "primitiveBounds", "joints0", "weights0", "skins", "inverseBindMatrices", "animationChannels"}}
        };
        inspection["valid"] = inspection["valid"].get<bool>() && mesh.valid;
        if (!mesh.valid) inspection["code"] = mesh.code;
    } else if (asset->available && asset->extension == ".fbx") {
        const auto mesh = decode_fbx_asset(source_path(*asset));
        Json primitive_bounds=Json::array();
        for (const auto& primitive:mesh.primitives) primitive_bounds.push_back(
            {{"center",primitive.bounds_center},{"radius",primitive.bounds_radius},{"skinnedInflation",primitive.skin>=0?1.5:1.0}});
        Json skin_summaries = Json::array();
        for (const auto& skin : mesh.skins) skin_summaries.push_back({{"name", skin.name}, {"jointCount", skin.joints.size()}});
        Json animation_summaries = Json::array();
        for (const auto& clip : mesh.animations) animation_summaries.push_back(
            {{"name", clip.name}, {"durationSeconds", clip.duration}, {"channelCount", clip.channels.size()}});
        inspection["importedMetadata"] = {
            {"format", "fbx"}, {"importer", "ufbx/0.23.0"}, {"valid", mesh.valid},
            {"code", mesh.code}, {"detail", mesh.detail}
        };
        inspection["renderPayload"] = {
            {"valid", mesh.valid}, {"code", mesh.code}, {"detail", mesh.detail},
            {"vertexCount", mesh.vertices.size()}, {"indexCount", mesh.indices.size()},
            {"primitiveCount", mesh.primitives.size()}, {"skinCount", mesh.skins.size()},
            {"primitiveBounds",primitive_bounds},
            {"skins", std::move(skin_summaries)}, {"animations", std::move(animation_summaries)},
            {"jointCount", std::accumulate(mesh.skins.begin(), mesh.skins.end(), std::size_t{},
                [](const std::size_t total, const GltfDecodedSkin& skin) { return total + skin.joints.size(); })},
            {"animationCount", mesh.animations.size()},
            {"animationChannelCount", std::accumulate(mesh.animations.begin(), mesh.animations.end(), std::size_t{},
                [](const std::size_t total, const GltfAnimationClip& clip) { return total + clip.channels.size(); })},
            {"features", {"triangulation", "generatedNormals", "texcoord0", "materialFactors", "top4NormalizedWeights",
                "primitiveBounds", "skins", "inverseBindMatrices", "bakedLinearAnimation"}}
        };
        inspection["valid"] = mesh.valid;
        inspection["code"] = mesh.code;
    } else if (asset->available && asset->extension == ".hdr") {
        const auto image=decode_hdr_file(source_path(*asset));
        inspection["importedMetadata"]={{"format","radiance-rgbe"},{"importer","radiance.hdr/1.0"},
            {"valid",image.valid},{"code",image.code},{"detail",image.detail},{"width",image.width},{"height",image.height},
            {"colorSpace","linear-rec709"},{"channels","RGBA32F"}};
        inspection["renderPayload"]={{"valid",image.valid},{"projection","equirectangular"},
            {"derivedArtifacts",Json::array({"irradiance-cube-rgba16f","prefiltered-specular-cube-rgba16f","brdf-lut-rg16f"})}};
        inspection["valid"]=image.valid; inspection["code"]=image.code;
    } else if(asset->available&&is_sprite_asset(*asset)) {
        const auto parsed=SpriteAssetCodec::parse_json(read_text_file(source_path(*asset)));
        Json errors=Json::array();for(const auto& value:parsed.errors)errors.push_back(
            {{"code",value.code},{"path",value.path},{"message",value.message}});
        inspection["valid"]=static_cast<bool>(parsed);
        inspection["code"]=parsed?"ok":"sprite.invalid-document";
        inspection["importedMetadata"]={{"format",parsed?parsed.document->schema:"noemancer.sprite-asset/unknown"},{"errors",std::move(errors)},
            {"dependencies",parsed?SpriteAssetCodec::asset_dependencies(*parsed.document):std::vector<std::string>{}}};
        inspection["renderPayload"]=nullptr;
        if(parsed) {
            const auto document=Json::parse(SpriteAssetCodec::write_canonical_json(*parsed.document));
            const auto dependencies=SpriteAssetCodec::asset_dependencies(*parsed.document);
            const auto production=SpriteAssetCodec::production_report(*parsed.document);
            inspection["importedMetadata"]["document"]=document;
            inspection["renderPayload"]={{"textureAsset",parsed.document->texture_asset},{"frameCount",parsed.document->frames.size()},
                {"clipCount",parsed.document->clips.size()},{"sampling",parsed.document->sampling},{"alphaMode",parsed.document->alpha_mode},
                {"pixelsPerUnit",parsed.document->pixels_per_unit},{"dependencies",dependencies},
                {"production",{{"valid",production.valid},{"code",production.code},
                    {"totalClipFrameReferences",production.total_clip_frame_references},
                    {"uniqueReferencedFrames",production.unique_referenced_frame_count},
                    {"unreferencedFrames",production.unreferenced_frame_count},{"maximumFramesPerClip",production.max_clip_frame_count},
                    {"atlas",{{"pageCount",production.atlas_page_count},{"area",production.atlas_area},
                        {"frameArea",production.frame_area_sum},{"occupiedArea",production.occupied_area},
                        {"freeArea",production.free_area},{"overlapArea",production.overlap_area},
                        {"layoutFingerprint",production.layout_fingerprint}}}}},
                {"material",document.value("material",Json(nullptr))}};
        }
    } else if(asset->available&&is_animation_clip_asset(*asset)) {
        const auto parsed=parse_animation_clip_source(source_path(*asset));
        inspection["valid"]=static_cast<bool>(parsed);inspection["code"]=parsed?"ok":parsed.code;
        inspection["importedMetadata"]={{"format",std::string(animation_clip_asset_schema)},
            {"detail",parsed.detail}};inspection["renderPayload"]=nullptr;
        if(parsed) {
            const auto* source=find(parsed.document->source_asset);
            inspection["importedMetadata"]["document"]=Json::parse(
                AnimationClipAssetCodec::write_canonical_json(*parsed.document));
            inspection["importedMetadata"]["buildInputs"]=AnimationClipAssetCodec::build_inputs(*parsed.document);
            inspection["renderPayload"]={{"sourceAsset",parsed.document->source_asset},
                {"skinIndex",parsed.document->skin_index},{"animationIndex",parsed.document->animation_index},
                {"compression",parsed.document->compression},{"sourceAvailable",source!=nullptr&&source->available}};
            if(source==nullptr||!source->available||(source->extension!=".fbx"&&source->extension!=".glb")) {
                inspection["valid"]=false;inspection["code"]="animation.clip-source-invalid";
            }
        }
    } else if(asset->available&&is_animation_state_machine_asset(*asset)) {
        const auto parsed=AnimationStateMachineCodec::parse_json(read_text_file(source_path(*asset)));
        inspection["valid"]=static_cast<bool>(parsed);inspection["code"]=parsed?"ok":parsed.code;
        inspection["importedMetadata"]={{"format","noemancer.animation-state-machine/0.2"},{"detail",parsed.detail}};
        inspection["renderPayload"]=nullptr;
        if(parsed)inspection["importedMetadata"]["document"]=Json::parse(AnimationStateMachineCodec::write_canonical_json(*parsed.document));
    } else if(asset->available&&is_animation_graph_asset(*asset)) {
        const auto parsed=parse_animation_graph_source(source_path(*asset));
        inspection["valid"]=static_cast<bool>(parsed);inspection["code"]=parsed?"ok":parsed.code;
        inspection["importedMetadata"]={{"format",std::string(animation_graph_schema)}, {"detail",parsed.detail}};
        inspection["renderPayload"]=nullptr;
        if(parsed) {
            if (const auto issue = animation_graph_asset_issue(*asset, *parsed.document, this)) {
                inspection["valid"] = false;
                inspection["code"] = issue->code;
                inspection["importedMetadata"]["detail"] = issue->detail;
            } else {
                const auto dependencies=AnimationGraphCodec::asset_dependencies(*parsed.document);
                inspection["importedMetadata"]["document"]=Json::parse(AnimationGraphCodec::write_canonical_json(*parsed.document));
                inspection["importedMetadata"]["dependencies"]=dependencies;
                inspection["renderPayload"]={{"parameterCount",parsed.document->parameters.size()},
                    {"nodeCount",parsed.document->nodes.size()},{"layerCount",parsed.document->layers.size()},
                    {"maskCount",parsed.document->masks.size()},{"syncGroupCount",parsed.document->sync_groups.size()},
                    {"dependencyCount",dependencies.size()}};
            }
        }
    } else if(asset->available&&is_tile_palette_asset(*asset)) {
        const auto parsed=TilemapAssetCodec::parse_palette_json(read_text_file(source_path(*asset)));
        Json errors=Json::array();for(const auto& value:parsed.errors)errors.push_back({{"code",value.code},{"path",value.path},{"message",value.message}});
        inspection["valid"]=static_cast<bool>(parsed);inspection["code"]=parsed?"ok":"tilemap.invalid-palette";
        inspection["importedMetadata"]={{"format",parsed?parsed.document->schema:"noemancer.tile-palette/unknown"},{"errors",std::move(errors)}};
        inspection["renderPayload"]=nullptr;
        if(parsed){inspection["importedMetadata"]["document"]=Json::parse(TilemapAssetCodec::write_palette_canonical_json(*parsed.document));
            const auto autotile_count=std::ranges::count_if(parsed.document->tiles,[](const TileDefinition& tile){return !tile.autotile_group.empty();});
            inspection["renderPayload"]={{"spriteAsset",parsed.document->sprite_asset},{"tileCount",parsed.document->tiles.size()},
                {"autotileCount",autotile_count},{"autotileNeighborBits",{{"north",1},{"east",2},{"south",4},{"west",8}}}};}
    } else if(asset->available&&is_tilemap_asset(*asset)) {
        const auto parsed=TilemapAssetCodec::parse_tilemap_json(read_text_file(source_path(*asset)));
        Json errors=Json::array();for(const auto& value:parsed.errors)errors.push_back({{"code",value.code},{"path",value.path},{"message",value.message}});
        inspection["valid"]=static_cast<bool>(parsed);inspection["code"]=parsed?"ok":"tilemap.invalid-document";
        inspection["importedMetadata"]={{"format","noemancer.tilemap/0.1"},{"errors",std::move(errors)}};inspection["renderPayload"]=nullptr;
        if(parsed){const auto production=TilemapAssetCodec::production_stats(*parsed.document);
            inspection["importedMetadata"]["document"]=Json::parse(TilemapAssetCodec::write_tilemap_canonical_json(*parsed.document));
            inspection["renderPayload"]={{"paletteAsset",parsed.document->palette_asset},{"layerCount",production.layer_count},
                {"chunkCount",production.chunk_count},{"cellCount",production.occupied_cell_count},
                {"production",Json::parse(TilemapAssetCodec::production_stats_json(production))}};}
    } else {
        inspection["importedMetadata"] = nullptr;
        inspection["renderPayload"] = nullptr;
    }
    return inspection.dump();
}

std::string AssetRegistry::cook_plan_json(
    const std::vector<std::string>& asset_ids,
    const std::string_view target_profile) const {
    Json inputs = Json::array();
    Json errors = Json::array();
    std::ostringstream integrity_source;
    integrity_source << target_profile << '\n' << revision_ << '\n';
    std::vector<std::string> scheduled_ids=asset_ids;
    std::unordered_set<std::string> scheduled(scheduled_ids.begin(),scheduled_ids.end());
    for (std::size_t scheduled_index=0;scheduled_index<scheduled_ids.size();++scheduled_index) {
        const auto id=scheduled_ids[scheduled_index];
        const auto* asset = find(id);
        if (asset == nullptr) {
            errors.push_back({{"code", "asset.not-found"}, {"assetId", id}});
            continue;
        }
        if (!asset->available) {
            errors.push_back({{"code", "asset.source-unavailable"}, {"assetId", id}});
            continue;
        }
        std::string importer = "passthrough.binary/0.1";
        Json build_inputs=Json::array();
        std::string recipe_hash;
        if (has_animation_format_conflict(*asset)) {
            importer = "invalid.animation/0.1";
            errors.push_back({{"code", "asset.animation-format-conflict"}, {"assetId", id},
                {"detail", "Animation asset kind and filename suffix disagree."}});
        } else if (asset->uri.starts_with("builtin://")) importer = "builtin.geometry/0.1";
        else if (asset->kind == "Scene") importer = "noemancer.scene/0.1";
        else if (is_animation_clip_asset(*asset)) {
            importer="noemancer.animation-clip/0.1+ozz-animation/0.17.0";
            const auto parsed=parse_animation_clip_source(source_path(*asset));
            if(!parsed)errors.push_back({{"code",parsed.code},{"assetId",id},{"detail",parsed.detail}});
            else if(parsed.document->asset_id!=asset->id)errors.push_back({{"code","animation.clip-asset-id-mismatch"},
                {"assetId",id},{"documentAssetId",parsed.document->asset_id}});
            else {
                const auto* source=find(parsed.document->source_asset);
                if(source==nullptr||!source->available||(source->extension!=".fbx"&&source->extension!=".glb"))
                    errors.push_back({{"code","animation.clip-source-invalid"},{"assetId",id},
                        {"sourceAsset",parsed.document->source_asset}});
                else {
                    build_inputs.push_back({{"assetId",source->id},{"sourceUri",source->uri},
                        {"sourceHash",source->content_hash},{"sourceBytes",source->source_bytes},
                        {"role","offline-animation-source"},{"license",source->license},
                        {"redistribution",source->redistribution}});
                    const auto canonical=AnimationClipAssetCodec::write_canonical_json(*parsed.document);
                    const auto recipe_material=std::string(animation_clip_asset_schema)+"\n"+canonical+"\n"+
                        source->content_hash+"\n"+importer+"\n"+std::string(target_profile)+"\n";
                    const auto hash=sha256_bytes(std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(recipe_material.data()),recipe_material.size()));
                    if(!hash.success)errors.push_back({{"code",hash.code},{"assetId",id},{"detail",hash.detail}});
                    else recipe_hash=hash.value;
                }
            }
        }
        else if (asset->extension == ".glb" || asset->extension == ".fbx") {
            importer = (asset->extension == ".glb" ? "gltf.binary/0.1" : "ufbx.scene/0.23.0") +
                std::string("+mesh-runtime-artifact/0.2");
            const auto recipe_material = std::string(mesh_runtime_artifact_schema) + "\n" +
                asset->content_hash + "\n" + importer + "\n" + std::string(target_profile) +
                "\nmeshoptimizer-per-primitive\nktx2-embedded\n";
            const auto hash = sha256_bytes(std::as_bytes(std::span(recipe_material)));
            if (!hash.success) errors.push_back({{"code", hash.code}, {"assetId", id}, {"detail", hash.detail}});
            else recipe_hash = hash.value;
        }
        else if (is_texture_cook_source(*asset)) importer = "image.png-rgba8/1.0+ktx2-basisu/0.1";
        else if (asset->extension == ".hdr") importer = "radiance.hdr/1.0+split-sum-ggx/1.0";
        else if (is_sprite_asset(*asset)) {
            importer="noemancer.sprite-asset/0.1";
            const auto parsed=SpriteAssetCodec::parse_json(read_text_file(source_path(*asset)));
            if(!parsed) errors.push_back({{"code","sprite.invalid-document"},{"assetId",id},{"errorCount",parsed.errors.size()}});
            else {
                importer=parsed.document->schema;
                const auto dependencies=SpriteAssetCodec::asset_dependencies(*parsed.document);
                std::string recipe_material = parsed.document->schema + "\n" +
                    SpriteAssetCodec::write_canonical_json(*parsed.document) + "\n" +
                    std::string(target_profile) + "\n";
                for(const auto& texture_asset:dependencies) {
                    const auto* dependency = find(texture_asset);
                    if(dependency==nullptr) {
                        errors.push_back({{"code","sprite.texture-asset-not-found"},{"assetId",id},{"textureAsset",texture_asset}});
                    } else {
                        if (!dependency->available)
                            errors.push_back({{"code","sprite.texture-asset-unavailable"},{"assetId",id},{"textureAsset",texture_asset}});
                        recipe_material += dependency->id + "\n" + dependency->content_hash + "\n";
                        if(scheduled.insert(texture_asset).second)scheduled_ids.push_back(texture_asset);
                    }
                }
                const auto hash = sha256_bytes(std::as_bytes(std::span(recipe_material)));
                if (!hash.success)
                    errors.push_back({{"code",hash.code},{"assetId",id},{"detail",hash.detail}});
                else
                    recipe_hash = hash.value;
            }
        } else if (is_animation_graph_asset(*asset)) {
            importer=std::string(animation_graph_schema);
            const auto parsed=parse_animation_graph_source(source_path(*asset));
            if(!parsed)errors.push_back({{"code",parsed.code},{"assetId",id},{"detail",parsed.detail}});
            else if (const auto issue = animation_graph_asset_issue(*asset, *parsed.document, this))
                errors.push_back({{"code",issue->code},{"assetId",id},{"detail",issue->detail}});
            else {
                const auto dependencies=AnimationGraphCodec::asset_dependencies(*parsed.document);
                for(const auto& dependency:dependencies) {
                    if(find(dependency)==nullptr)errors.push_back({{"code","animation.graph-dependency-not-found"},
                        {"assetId",id},{"dependency",dependency}});
                    else if(scheduled.insert(dependency).second)scheduled_ids.push_back(dependency);
                }
            }
        } else if (is_animation_state_machine_asset(*asset)) {
            importer="noemancer.animation-state-machine/0.2";
            const auto parsed=AnimationStateMachineCodec::parse_json(read_text_file(source_path(*asset)));
            if(!parsed)errors.push_back({{"code",parsed.code},{"assetId",id},{"detail",parsed.detail}});
            else for(const auto& dependency:AnimationStateMachineCodec::asset_dependencies(*parsed.document)) {
                if(find(dependency)==nullptr)errors.push_back({{"code","animation.machine-dependency-not-found"},
                    {"assetId",id},{"dependency",dependency}});
                else if(scheduled.insert(dependency).second)scheduled_ids.push_back(dependency);
            }
        } else if(is_tile_palette_asset(*asset)) {
            const auto parsed=TilemapAssetCodec::parse_palette_json(read_text_file(source_path(*asset)));importer=parsed?parsed.document->schema:"noemancer.tile-palette/invalid";
            if(!parsed)errors.push_back({{"code","tilemap.invalid-palette"},{"assetId",id},{"errorCount",parsed.errors.size()}});
            else if(find(parsed.document->sprite_asset)==nullptr)errors.push_back({{"code","tilemap.sprite-asset-not-found"},{"assetId",id},{"spriteAsset",parsed.document->sprite_asset}});
            else {
                const auto* sprite_asset=find(parsed.document->sprite_asset);const auto sprite=SpriteAssetCodec::parse_json(read_text_file(source_path(*sprite_asset)));
                if(!sprite)errors.push_back({{"code","tilemap.sprite-asset-invalid"},{"assetId",id},{"spriteAsset",parsed.document->sprite_asset}});
                else {std::unordered_set<std::string> frame_ids;for(const auto& frame:sprite.document->frames)frame_ids.insert(frame.id);
                    for(const auto& tile:parsed.document->tiles) {if(!frame_ids.contains(tile.frame_id))errors.push_back({{"code","tilemap.frame-not-found"},
                        {"assetId",id},{"tileId",tile.id},{"frameId",tile.frame_id}});for(const auto& variant:tile.autotile_variants)
                            if(!frame_ids.contains(variant.frame_id))errors.push_back({{"code","tilemap.autotile-frame-not-found"},{"assetId",id},
                                {"tileId",tile.id},{"neighborMask",variant.neighbor_mask},{"frameId",variant.frame_id}});}
                }
                if(scheduled.insert(parsed.document->sprite_asset).second)scheduled_ids.push_back(parsed.document->sprite_asset);
            }
        } else if(is_tilemap_asset(*asset)) {
            importer="noemancer.tilemap/0.1";const auto parsed=TilemapAssetCodec::parse_tilemap_json(read_text_file(source_path(*asset)));
            if(!parsed)errors.push_back({{"code","tilemap.invalid-document"},{"assetId",id},{"errorCount",parsed.errors.size()}});
            else if(find(parsed.document->palette_asset)==nullptr)errors.push_back({{"code","tilemap.palette-asset-not-found"},{"assetId",id},{"paletteAsset",parsed.document->palette_asset}});
            else if(scheduled.insert(parsed.document->palette_asset).second)scheduled_ids.push_back(parsed.document->palette_asset);
        }
        const bool importable=asset->import_state=="ready"||asset->import_state=="unregistered";
        if (!importable) {
            errors.push_back({
                {"code", "asset.importer-unavailable"},
                {"assetId", id},
                {"importState", asset->import_state}
            });
        }
        integrity_source << asset->id << '\n' << asset->content_hash << '\n' << importer << '\n'
                         << recipe_hash << '\n' << build_inputs.dump() << '\n';
        const auto cache_identity=recipe_hash.empty()?asset->content_hash:recipe_hash;
        Json input = {
            {"assetId", asset->id},
            {"sourceUri", asset->uri},
            {"sourceHash", asset->content_hash},
            {"importer", importer},
            {"cacheUri", "cache://sha256/" + cache_identity.substr(cache_identity.find(':') + 1)},
            {"cacheEligible", importable}
        };
        if(!recipe_hash.empty())input["recipeHash"]=recipe_hash;
        if(!build_inputs.empty())input["buildInputs"]=std::move(build_inputs);
        if (is_animation_graph_asset(*asset)||is_animation_state_machine_asset(*asset)||is_sprite_asset(*asset)) {
            if(is_sprite_asset(*asset)) {
                const auto parsed=SpriteAssetCodec::parse_json(read_text_file(source_path(*asset)));
                input["dependencies"]=parsed?SpriteAssetCodec::asset_dependencies(*parsed.document):asset->dependencies;
            } else input["dependencies"] = asset->dependencies;
        }
        inputs.push_back(std::move(input));
    }
    const bool valid = errors.empty() && !inputs.empty();
    const auto content_hash = fnv1a64(integrity_source.str());
    return Json{
        {"schemaVersion", "0.1"},
        {"valid", valid},
        {"code", valid ? "ok" : "asset.cook-plan-invalid"},
        {"planId", "cook-plan-" + content_hash.substr(content_hash.find(':') + 1)},
        {"contentHash", content_hash},
        {"targetProfile", target_profile},
        {"registryRevision", revision_},
        {"inputs", std::move(inputs)},
        {"errors", std::move(errors)},
        {"sideEffects", Json::array({"generated/cache", "generated/cook-manifest.json"})}
    }.dump();
}

std::string AssetRegistry::apply_cook_plan_json(
    const std::string_view plan_json,
    const bool dry_run) const {
    Json receipt = {
        {"schemaVersion", "0.1"},
        {"success", false},
        {"dryRun", dry_run},
        {"code", "asset.invalid-cook-plan"},
        {"detail", "Cook plan could not be validated."},
        {"operationId", next_cook_operation_id()},
        {"planId", ""},
        {"registryRevision", revision_},
        {"cacheHits", 0},
        {"cacheMisses", 0},
        {"artifacts", Json::array()},
        {"errors", Json::array()}
    };
    std::vector<std::filesystem::path> sprite_created_page_paths;
    try {
        const auto plan = Json::parse(plan_json);
        receipt["planId"] = plan.value("planId", std::string{});
        if (!plan.value("valid", false)) {
            receipt["code"] = "asset.cook-plan-invalid";
            receipt["detail"] = "Only a valid Cook plan can be applied.";
            receipt["errors"] = plan.value("errors", Json::array());
            return receipt.dump();
        }
        if (plan.at("registryRevision").get<std::uint64_t>() != revision_) {
            receipt["code"] = "asset.registry-revision-conflict";
            receipt["detail"] = "Asset Registry changed after the Cook plan was created.";
            return receipt.dump();
        }
        std::vector<std::string> ids;
        for (const auto& input : plan.at("inputs")) ids.push_back(input.at("assetId").get<std::string>());
        const auto regenerated = Json::parse(cook_plan_json(ids, plan.at("targetProfile").get<std::string>()));
        if (!regenerated.value("valid", false) ||
            regenerated.at("contentHash") != plan.at("contentHash") || regenerated.at("planId") != plan.at("planId")) {
            receipt["code"] = "asset.cook-plan-integrity-error";
            receipt["detail"] = "Cook plan hash does not match current deterministic inputs.";
            return receipt.dump();
        }
        receipt["success"] = true;
        receipt["code"] = dry_run ? "asset.cook-plan-validated" : "ok";
        receipt["detail"] = dry_run ? "Cook dry run passed without writing artifacts." : "Cook artifacts committed to the content-addressed cache.";
        if (dry_run) return receipt.dump();

        const auto generated_root = asset_root_.parent_path() / "generated";
        const auto resolve_generated_uri = [&](const std::string_view uri)
            -> std::optional<std::filesystem::path> {
            constexpr std::string_view prefix = "generated://";
            if (!uri.starts_with(prefix)) return std::nullopt;
            const auto relative_text = std::string(uri.substr(prefix.size()));
            if (relative_text.empty()) return std::nullopt;
            const std::filesystem::path relative(relative_text);
            if (relative.is_absolute()) return std::nullopt;
            for (const auto& component : relative) {
                if (component == ".." || component == "." || component.empty()) return std::nullopt;
            }
            return (generated_root / relative).lexically_normal();
        };
        Json outputs = Json::array();
        for (const auto& input : plan.at("inputs")) {
            const auto id = input.at("assetId").get<std::string>();
            const auto* asset = find(id);
            if (asset == nullptr) throw std::runtime_error("Planned asset disappeared: " + id);
            if (has_animation_format_conflict(*asset))
                throw std::runtime_error("Animation asset kind and filename suffix disagree for " + asset->id);
            const auto source_file = source_path(*asset);
            if ((asset->hash_provenance == "computed" || is_animation_clip_asset(*asset)) &&
                sha256_file_value(source_file) != asset->content_hash) {
                throw std::runtime_error("Source hash changed after Cook planning: " + asset->id);
            }
            if(input.contains("buildInputs"))for(const auto& build_input:input.at("buildInputs")) {
                const auto build_asset_id=build_input.at("assetId").get<std::string>();
                const auto* build_asset=find(build_asset_id);
                if(build_asset==nullptr||!build_asset->available)
                    throw std::runtime_error("Cook build input is unavailable: "+build_asset_id);
                const auto current_hash=noemancer::sha256_file(source_path(*build_asset));
                if(!current_hash.success||current_hash.value!=build_input.at("sourceHash").get<std::string>()||
                    current_hash.value!=build_asset->content_hash)
                    throw std::runtime_error("Animation Clip build input changed after Cook planning: "+build_asset_id);
            }
            const auto profile=cook_platform_profile(plan.at("targetProfile").get<std::string>());
            const bool texture_cook=is_texture_cook_source(*asset);
            const bool sprite_atlas_cook=is_sprite_asset(*asset);
            const auto texture_cook_settings=texture_settings(*asset);
            const auto texture_plan=texture_cook?plan_texture_cook(CookSource{.asset_id=asset->id,.source_uri=asset->uri,
                .source_hash=asset->content_hash,.source_bytes=asset->source_bytes,.importer=input.at("importer").get<std::string>()},
                profile,texture_cook_settings):CookArtifactContract{};
            if(texture_cook&&!texture_plan.valid)throw std::runtime_error("Texture Cook plan failed for "+asset->id+": "+texture_plan.detail);
            const auto planned_recipe_hash=input.value("recipeHash",std::string{});
            const auto hash=texture_cook?texture_plan.cache_key.substr(texture_plan.cache_key.find(':')+1):
                !planned_recipe_hash.empty()?planned_recipe_hash.substr(planned_recipe_hash.find(':')+1):
                asset->content_hash.substr(asset->content_hash.find(':')+1);
            const auto cache_directory = generated_root / "cook-cache" / hash;
            const auto metadata_path = cache_directory / "asset.json";
            const bool mesh_cook = asset->extension == ".glb" || asset->extension == ".fbx";
            const auto payload_extension = cooked_payload_extension(*asset);
            const auto payload_format = cooked_payload_format(*asset);
            const auto payload_path = cache_directory / ("payload" + payload_extension);
            const auto payload_uri = "generated://cook-cache/" + hash + "/payload" + payload_extension;
            bool cache_hit = std::filesystem::is_regular_file(metadata_path) &&
                std::filesystem::is_regular_file(payload_path);
            if(cache_hit&&is_animation_clip_asset(*asset)) {
                const auto cached_metadata=Json::parse(read_text_file(metadata_path),nullptr,false);
                const auto cached_payload_hash=noemancer::sha256_file(payload_path);
                cache_hit=cached_metadata.is_object()&&cached_metadata.value("schema",std::string{})=="noemancer.cooked-asset/0.1"&&
                    cached_metadata.value("recipeHash",std::string{})==planned_recipe_hash&&cached_payload_hash.success&&
                    cached_metadata.value("payloadHash",std::string{})==cached_payload_hash.value;
            }
            if (cache_hit && mesh_cook) {
                const auto cached_metadata = Json::parse(read_text_file(metadata_path), nullptr, false);
                const auto cached_payload_hash = noemancer::sha256_file(payload_path);
                const auto* imported = cached_metadata.is_object() && cached_metadata.contains("importedMetadata")
                    ? &cached_metadata.at("importedMetadata") : nullptr;
                cache_hit = imported != nullptr && imported->is_object() &&
                    cached_metadata.value("schema", std::string{}) == "noemancer.cooked-asset/0.1" &&
                    cached_metadata.value("recipeHash", std::string{}) == planned_recipe_hash &&
                    cached_payload_hash.success &&
                    cached_metadata.value("payloadHash", std::string{}) == cached_payload_hash.value &&
                    imported->value("format", std::string{}) == mesh_runtime_artifact_schema &&
                    imported->contains("runtimeContract") && imported->at("runtimeContract").is_object() &&
                    imported->at("runtimeContract").value("sourceDecodeAtRuntime", true) == false;
            }
            if (cache_hit && sprite_atlas_cook) {
                const auto cached_metadata = Json::parse(read_text_file(metadata_path), nullptr, false);
                const auto cached_payload_hash = noemancer::sha256_file(payload_path);
                const auto* imported = cached_metadata.is_object() && cached_metadata.contains("importedMetadata")
                    ? &cached_metadata.at("importedMetadata") : nullptr;
                cache_hit = imported != nullptr && imported->is_object() &&
                    cached_metadata.value("schema", std::string{}) == "noemancer.cooked-asset/0.1" &&
                    cached_metadata.value("recipeHash", std::string{}) == planned_recipe_hash &&
                    cached_payload_hash.success &&
                    cached_metadata.value("payloadHash", std::string{}) == cached_payload_hash.value &&
                    cached_metadata.value("payloadFormat", std::string{}) ==
                        "noemancer.sprite-atlas-artifact/0.1" &&
                    imported->value("format", std::string{}) == "noemancer.sprite-atlas-artifact/0.1" &&
                    imported->contains("authoringDocument") && imported->at("authoringDocument").is_object() &&
                    imported->contains("atlasArtifact") && imported->at("atlasArtifact").is_object() &&
                    imported->at("atlasArtifact").value("kind", std::string{}) == "SpriteAtlas";
                if (cache_hit && imported->contains("atlasArtifact") &&
                    imported->at("atlasArtifact").is_object() &&
                    imported->at("atlasArtifact").contains("pageArtifacts") &&
                    imported->at("atlasArtifact").at("pageArtifacts").is_array()) {
                    const auto& cached_sprite_page_outputs = imported->at("atlasArtifact").at("pageArtifacts");
                    for (const auto& page : cached_sprite_page_outputs) {
                        const auto page_uri = page.value("payloadUri", std::string{});
                        const auto page_path = resolve_generated_uri(page_uri);
                        const auto expected_hash = page.value("payloadHash", std::string{});
                        if (!page_path || expected_hash.empty()) {
                            cache_hit = false;
                            break;
                        }
                        const auto page_identity = noemancer::sha256_file(*page_path);
                        if (!page_identity.success || page_identity.value != expected_hash) {
                            cache_hit = false;
                            break;
                        }
                    }
                } else {
                    cache_hit = false;
                }
            }
            if (cache_hit) {
                receipt["cacheHits"] = receipt.at("cacheHits").get<std::size_t>() + 1;
            } else {
                receipt["cacheMisses"] = receipt.at("cacheMisses").get<std::size_t>() + 1;
                Json imported_metadata = nullptr;
                Json cooked_dependencies = input.value("dependencies", asset->dependencies);
                std::vector<std::byte> cooked_binary_payload;
                std::string cooked_payload_hash;
                const bool animation_clip=is_animation_clip_asset(*asset);
                const bool animation_document = !asset->uri.starts_with("builtin://") &&
                    (is_animation_graph_asset(*asset) || is_animation_state_machine_asset(*asset));
                if (asset->uri.starts_with("builtin://") && !animation_document) {
                    imported_metadata = {{"procedural", true}, {"sourceUri", asset->uri}};
                } else if(animation_clip) {
                    const auto descriptor_bytes=read_binary_file(source_file);
                    if(descriptor_bytes.empty()||descriptor_bytes.size()>animation_clip_asset_max_source_bytes)
                        throw std::runtime_error("Animation Clip descriptor snapshot is unavailable or too large: "+asset->id);
                    const auto descriptor_identity=sha256_bytes(descriptor_bytes);
                    if(!descriptor_identity.success||descriptor_identity.value!=asset->content_hash)
                        throw std::runtime_error("Animation Clip descriptor changed while Cook was starting: "+asset->id);
                    const auto descriptor_text=std::string_view(
                        reinterpret_cast<const char*>(descriptor_bytes.data()),descriptor_bytes.size());
                    const auto parsed=AnimationClipAssetCodec::parse_json(descriptor_text);
                    if(!parsed)throw std::runtime_error("Animation Clip validation failed for "+asset->id+": "+parsed.code);
                    const auto* source_asset=find(parsed.document->source_asset);
                    if(source_asset==nullptr||!source_asset->available)
                        throw std::runtime_error("Animation Clip source is unavailable for "+asset->id);
                    const auto source_asset_path=source_path(*source_asset);
                    constexpr std::uintmax_t maximum_snapshot_bytes=512U*1024U*1024U;
                    if(source_asset->source_bytes==0U||source_asset->source_bytes>maximum_snapshot_bytes)
                        throw std::runtime_error("Animation Clip source exceeds the Cook snapshot budget: "+asset->id);
                    const auto source_bytes=read_binary_file(source_asset_path);
                    const auto source_identity=sha256_bytes(source_bytes);
                    if(source_bytes.size()!=source_asset->source_bytes||!source_identity.success||
                        source_identity.value!=source_asset->content_hash)
                        throw std::runtime_error("Animation Clip source changed while Cook was starting: "+asset->id);
                    const auto snapshot_directory=generated_root/"cook-staging"/next_cook_operation_id();
                    ScopedCookInputSnapshot snapshot(snapshot_directory/("source"+source_asset->extension));
                    std::string snapshot_error;
                    if(!write_atomic_bytes(snapshot.path(),source_bytes,snapshot_error))
                        throw std::runtime_error("Animation Clip source snapshot failed: "+snapshot_error);
                    const auto decoded=source_asset->extension==".fbx"?decode_fbx_asset(snapshot.path()):
                        decode_glb_mesh(snapshot.path());
                    if(!decoded.valid)throw std::runtime_error("Animation Clip source decode failed for "+asset->id+
                        ": "+decoded.code+" - "+decoded.detail);
                    const auto compression=parsed.document->compression=="ozz_hierarchical_key_reduction"?
                        AnimationCompressionMode::ozz_hierarchical_key_reduction:
                        AnimationCompressionMode::ozz_runtime_baseline;
                    AnimationRuntime cooker;
                    const auto product=cooker.cook_gltf_animation_artifact(asset->id,source_asset->content_hash,
                        decoded,parsed.document->skin_index,parsed.document->animation_index,compression);
                    if(!product.success)throw std::runtime_error("Animation Cook failed for "+asset->id+": "+
                        product.code+" - "+product.detail);
                    cooked_binary_payload=product.payload;cooked_payload_hash=product.payload_hash;
                    imported_metadata={{"format",std::string(animation_clip_asset_schema)},
                        {"document",Json::parse(AnimationClipAssetCodec::write_canonical_json(*parsed.document))},
                        {"buildInputs",Json::array({{{"assetId",source_asset->id},{"sourceHash",source_asset->content_hash},
                            {"sourceBytes",source_asset->source_bytes},{"license",source_asset->license},
                            {"redistribution",source_asset->redistribution},{"packaged",false}}})},
                        {"runtimeContract",{{"payloadFormat","noemancer/animbin"},{"payloadHash",product.payload_hash},
                            {"jointCount",product.joint_count},{"clipAssets",product.clip_assets},
                            {"sourceDecodeAtRuntime",false},{"offlineCompileAtRuntime",false}}}};
                } else if (!animation_document && mesh_cook) {
                    constexpr std::uintmax_t maximum_snapshot_bytes = 512U * 1024U * 1024U;
                    if (asset->source_bytes == 0U || asset->source_bytes > maximum_snapshot_bytes)
                        throw std::runtime_error("GLB source exceeds the Cook snapshot budget: " + asset->id);
                    const auto source_bytes = read_binary_file(source_file);
                    const auto source_identity = sha256_bytes(source_bytes);
                    if (source_bytes.size() != asset->source_bytes || !source_identity.success ||
                        source_identity.value != asset->content_hash)
                        throw std::runtime_error("GLB source changed while Cook was starting: " + asset->id);
                    const auto snapshot_directory = generated_root / "cook-staging" / next_cook_operation_id();
                    ScopedCookInputSnapshot snapshot(snapshot_directory / ("source" + asset->extension));
                    std::string snapshot_error;
                    if (!write_atomic_bytes(snapshot.path(), source_bytes, snapshot_error))
                        throw std::runtime_error("GLB source snapshot failed: " + snapshot_error);
                    Json summary;
                    GltfMeshData decoded;
                    if (asset->extension == ".glb") {
                        summary = Json::parse(gltf_summary_json(inspect_glb(snapshot.path())));
                        if (!summary.value("valid", false))
                            throw std::runtime_error("GLB inspection failed for " + asset->id);
                        decoded = decode_glb_mesh(snapshot.path());
                    } else {
                        decoded = decode_fbx_asset(snapshot.path());
                        summary = {{"valid", decoded.valid}, {"format", "fbx"},
                            {"vertexCount", decoded.vertices.size()}, {"indexCount", decoded.indices.size()},
                            {"primitiveCount", decoded.primitives.size()}, {"imageCount", decoded.images.size()}};
                    }
                    if (!decoded.valid) {
                        throw std::runtime_error("GLB mesh decode failed for " + asset->id + ": " +
                            decoded.code + " - " + decoded.detail);
                    }
                    const auto mesh_profile = cook_platform_profile(plan.at("targetProfile").get<std::string>());
                    const auto mesh_product = cook_mesh_runtime_artifact(CookSource{
                        .asset_id = asset->id, .source_uri = asset->uri,
                        .source_hash = asset->content_hash, .source_bytes = asset->source_bytes,
                        .importer = input.at("importer").get<std::string>()}, decoded, mesh_profile);
                    if (!mesh_product.success) {
                        throw std::runtime_error("GLB mesh Cook failed for " + asset->id + ": " +
                            mesh_product.code + " - " + mesh_product.detail);
                    }
                    cooked_binary_payload = mesh_product.payload;
                    cooked_payload_hash = mesh_product.payload_hash;
                    imported_metadata = {{"format", std::string(mesh_runtime_artifact_schema)},
                        {"sourceSummary", std::move(summary)},
                        {"runtimeContract", {{"payloadFormat", "noemancer/meshbin/0.2"},
                            {"payloadHash", mesh_product.payload_hash}, {"lodCount", mesh_product.lod_count},
                            {"primitiveCount", mesh_product.primitive_count}, {"imageCount", mesh_product.image_count},
                            {"geometryPartitioning", "per-primitive"}, {"sourceDecodeAtRuntime", false},
                            {"embeddedTextures", "ktx2"}}}};
                } else if(!animation_document && texture_cook) {
                    const auto encoded=read_binary_file(source_file);
                    const auto decoded=decode_png_rgba8(std::span<const std::byte>(encoded.data(),encoded.size()));
                    if(!decoded.valid)throw std::runtime_error("PNG decode failed for "+asset->id+": "+decoded.code+" - "+decoded.detail);
                    TextureCookInput texture_input{.width=decoded.width,.height=decoded.height};
                    texture_input.rgba8.assign(reinterpret_cast<const std::byte*>(decoded.rgba8.data()),
                        reinterpret_cast<const std::byte*>(decoded.rgba8.data()+decoded.rgba8.size()));
                    const auto product=execute_texture_cook(CookSource{.asset_id=asset->id,.source_uri=asset->uri,
                        .source_hash=asset->content_hash,.source_bytes=asset->source_bytes,.importer=input.at("importer").get<std::string>()},
                        texture_input,profile,texture_cook_settings,texture_compression(texture_cook_settings));
                    if(!product.valid)throw std::runtime_error("KTX2 Cook failed for "+asset->id+": "+product.code+" - "+product.detail);
                    cooked_binary_payload=product.payload;imported_metadata={{"textureCook",Json::parse(texture_cook_product_json(product))}};
                    imported_metadata["textureCook"]["payloadUri"]=payload_uri;
                } else if (!animation_document && asset->extension == ".hdr") {
                    const auto image=decode_hdr_file(source_file);
                    if (!image.valid) throw std::runtime_error("HDR inspection failed for " + asset->id + ": " + image.code);
                    imported_metadata={{"format","radiance-rgbe"},{"width",image.width},{"height",image.height},
                        {"colorSpace","linear-rec709"},{"projection","equirectangular"},
                        {"iblCookContract",{{"version","split-sum-ggx/1.0"},{"irradiance",{{"resolution",16},{"format","RGBA16F"}}},
                            {"prefilteredSpecular",{{"resolution",64},{"mipLevels",7},{"format","RGBA16F"}}},
                            {"brdfLut",{{"resolution",128},{"format","RG16F"}}}}}};
                } else if(!animation_document && sprite_atlas_cook) {
                    const auto parsed=SpriteAssetCodec::parse_json(read_text_file(source_file));
                    if(!parsed)throw std::runtime_error("sprite.invalid-document: Sprite document validation failed for "+asset->id);
                    const auto document=Json::parse(SpriteAssetCodec::write_canonical_json(*parsed.document));
                    const auto dependencies=SpriteAssetCodec::asset_dependencies(*parsed.document);
                    std::vector<std::string> material_dependencies;
                    for(const auto& dependency:dependencies)
                        if(dependency!=parsed.document->texture_asset)material_dependencies.push_back(dependency);
                    const auto* texture_asset=find(parsed.document->texture_asset);
                    if(texture_asset==nullptr||!texture_asset->available)
                        throw std::runtime_error("sprite.texture-asset-unavailable: " +
                            parsed.document->texture_asset + " for " + asset->id);
                    const auto texture_source=source_path(*texture_asset);
                    const auto texture_identity=noemancer::sha256_file(texture_source);
                    if(!texture_identity.success||texture_identity.value!=texture_asset->content_hash)
                        throw std::runtime_error("sprite.texture-source-changed: " + texture_asset->id);
                    const auto encoded=read_binary_file(texture_source);
                    const auto decoded=decode_png_rgba8(std::span<const std::byte>(encoded.data(),encoded.size()));
                    if(!decoded.valid)
                        throw std::runtime_error("sprite.atlas-source-decode-failed: " + texture_asset->id +
                            ": " + decoded.code + " - " + decoded.detail);
                    if(decoded.width!=parsed.document->texture_width||decoded.height!=parsed.document->texture_height)
                        throw std::runtime_error("sprite.atlas-source-size-mismatch: " + texture_asset->id);
                    const auto atlas_rgba8=std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(decoded.rgba8.data()),decoded.rgba8.size());
                    const auto sprite_texture_settings=texture_settings(*texture_asset);
                    SpriteAtlasArtifactExecutionOptions atlas_execution;
                    // The cache adapter adds its own versioned namespace. Do
                    // not repeat that name here: real Windows projects often
                    // Cook from long staging paths and must stay below legacy
                    // path limits while retaining atomic temporary names.
                    atlas_execution.page_cache_root=generated_root/"cook-cache";
                    atlas_execution.compression=texture_compression(sprite_texture_settings);
                    const auto artifact=execute_sprite_atlas_artifact(
                        *parsed.document,atlas_rgba8,SpriteAtlasPlanningOptions{},
                        CookSource{.asset_id=asset->id,.source_uri=asset->uri,.source_hash=asset->content_hash,
                            .source_bytes=asset->source_bytes,.importer=input.at("importer").get<std::string>()},
                        profile,sprite_texture_settings,{},atlas_execution);
                    if(!artifact.valid)
                        throw std::runtime_error("sprite.atlas-artifact-failed: " + artifact.code +
                            (artifact.detail.empty()?std::string{}:": "+artifact.detail));

                    Json atlas_manifest=Json::parse(sprite_atlas_artifact_json(artifact));
                    const auto page_metadata_source=*texture_asset;
                    const auto streaming_policy=Json{
                        {"mode",page_metadata_source.streaming_mode},
                        {"importance",page_metadata_source.streaming_importance},
                        {"priority",page_metadata_source.streaming_priority}};
                    Json page_artifacts=Json::array();
                    std::vector<std::string> page_ids;
                    page_ids.reserve(artifact.pages.size());
                    const auto page_items=atlas_manifest.value("pages",Json::object()).value("items",Json::array());
                    for(std::size_t page_index=0U;page_index<artifact.pages.size();++page_index) {
                        const auto& page=artifact.pages[page_index];
                        page_ids.push_back(page.asset_id);
                        const auto page_path=cache_directory/"pages"/("page-"+std::to_string(page.page_index)+".ktx2");
                        const auto page_uri="generated://cook-cache/"+hash+"/pages/page-"+
                            std::to_string(page.page_index)+".ktx2";
                        const auto existing_identity=noemancer::sha256_file(page_path);
                        if(existing_identity.success) {
                            if(existing_identity.value!=page.payload_fingerprint)
                                throw std::runtime_error("sprite.page-artifact-conflict: "+page_path.string());
                        } else {
                            std::string page_error;
                            if(!write_atomic_bytes(page_path,std::span<const std::byte>(page.payload.data(),page.payload.size()),page_error))
                                throw std::runtime_error("sprite.page-artifact-write-failed: "+page_error);
                            sprite_created_page_paths.push_back(page_path);
                        }
                        Json page_record={
                            {"assetId",page.asset_id},
                            {"displayName",asset->display_name+" / Atlas Page "+std::to_string(page.page_index)},
                            {"kind","SpriteAtlasPage"},
                            {"license",page_metadata_source.license},
                            {"redistribution",page_metadata_source.redistribution},
                            {"streamingPolicy",streaming_policy},
                            {"tags",page_metadata_source.tags},
                            {"dependencies",material_dependencies},
                            {"required",!asset->optional&&!page_metadata_source.optional},
                            {"sourceAssetId",asset->id},
                            {"sourceTextureAssetId",texture_asset->id},
                            {"pageIndex",page.page_index},
                            {"payloadUri",page_uri},
                            {"payloadFormat","ktx2"},
                            {"payloadHash",page.payload_fingerprint},
                            {"payloadBytes",page.payload_bytes},
                            {"cacheKey",page.cache_key},
                            {"frameCount",page.frame_count},
                            {"size",Json::array({page.width,page.height})}};
                        page_artifacts.push_back(std::move(page_record));
                        if(page_index<page_items.size()&&page_items.at(page_index).is_object()) {
                            atlas_manifest["pages"]["items"][page_index]["payloadUri"]=page_uri;
                            atlas_manifest["pages"]["items"][page_index]["sourceAssetId"]=asset->id;
                        }
                    }
                    std::vector<std::string> manifest_dependencies=page_ids;
                    for(const auto& dependency:material_dependencies)
                        if(std::ranges::find(manifest_dependencies,dependency)==manifest_dependencies.end())
                            manifest_dependencies.push_back(dependency);
                    cooked_dependencies=manifest_dependencies;
                    atlas_manifest["displayName"]=asset->display_name;
                    atlas_manifest["kind"]="SpriteAtlas";
                    atlas_manifest["license"]=asset->license;
                    atlas_manifest["redistribution"]=asset->redistribution;
                    atlas_manifest["streamingPolicy"]={{"mode",asset->streaming_mode},
                        {"importance",asset->streaming_importance},{"priority",asset->streaming_priority}};
                    atlas_manifest["tags"]=asset->tags;
                    atlas_manifest["dependencies"]=manifest_dependencies;
                    atlas_manifest["authoringDependencies"]=dependencies;
                    atlas_manifest["authoringDocument"]=document;
                    atlas_manifest["required"]=!asset->optional;
                    atlas_manifest["sourceAssetId"]=asset->id;
                    atlas_manifest["pageArtifacts"]=page_artifacts;
                    atlas_manifest["payloadFormat"]="noemancer.sprite-atlas-artifact/0.1";
                    const auto atlas_manifest_text=atlas_manifest.dump(2)+"\n";
                    cooked_binary_payload.assign(reinterpret_cast<const std::byte*>(atlas_manifest_text.data()),
                        reinterpret_cast<const std::byte*>(atlas_manifest_text.data()+atlas_manifest_text.size()));
                    const auto atlas_identity=sha256_bytes(std::span<const std::byte>(
                        cooked_binary_payload.data(),cooked_binary_payload.size()));
                    if(!atlas_identity.success)throw std::runtime_error("sprite.atlas-manifest-hash-failed: "+atlas_identity.detail);
                    cooked_payload_hash=atlas_identity.value;
                    imported_metadata={{"format","noemancer.sprite-atlas-artifact/0.1"},
                        {"document",document},{"authoringDocument",document},{"authoringDependencies",dependencies},
                        {"dependencies",manifest_dependencies},{"atlasArtifact",atlas_manifest},
                        {"displayName",asset->display_name},{"kind","SpriteAtlas"},{"license",asset->license},
                        {"redistribution",asset->redistribution},{"streamingPolicy",streaming_policy},
                        {"tags",asset->tags},{"required",!asset->optional},{"sourceAssetId",asset->id},
                        {"runtimeContract",{{"textureAsset",parsed.document->texture_asset},
                            {"frameCount",parsed.document->frames.size()},{"clipCount",parsed.document->clips.size()},
                            {"sampling",parsed.document->sampling},{"alphaMode",parsed.document->alpha_mode},
                            {"pixelsPerUnit",parsed.document->pixels_per_unit},{"dependencies",manifest_dependencies},
                            {"material",document.value("material",Json(nullptr))},{"atlasManifest",atlas_manifest}}}};
                } else if(is_animation_state_machine_asset(*asset)) {
                    const auto parsed=AnimationStateMachineCodec::parse_json(read_text_file(source_file));
                    if(!parsed)throw std::runtime_error("Animation State Machine validation failed for "+asset->id+": "+parsed.code);
                    imported_metadata={{"format","noemancer.animation-state-machine/0.2"},
                        {"document",Json::parse(AnimationStateMachineCodec::write_canonical_json(*parsed.document))},
                        {"dependencies",AnimationStateMachineCodec::asset_dependencies(*parsed.document)},
                        {"runtimeContract",{{"stateCount",parsed.document->states.size()},{"transitionCount",parsed.document->transitions.size()},
                            {"parameterCount",parsed.document->parameters.size()},{"initialState",parsed.document->initial_state}}}};
                } else if(is_animation_graph_asset(*asset)) {
                    const auto parsed=parse_animation_graph_source(source_file);
                    if(!parsed)throw std::runtime_error("Animation Graph validation failed for "+asset->id+": "+parsed.code);
                    if (const auto issue = animation_graph_asset_issue(*asset, *parsed.document, this))
                        throw std::runtime_error("Animation Graph validation failed for "+asset->id+": "+issue->code);
                    const auto dependencies=AnimationGraphCodec::asset_dependencies(*parsed.document);
                    imported_metadata={{"format",std::string(animation_graph_schema)},
                        {"document",Json::parse(AnimationGraphCodec::write_canonical_json(*parsed.document))},
                        {"dependencies",dependencies},
                        {"runtimeContract",{{"parameterCount",parsed.document->parameters.size()},
                            {"nodeCount",parsed.document->nodes.size()},{"layerCount",parsed.document->layers.size()},
                            {"maskCount",parsed.document->masks.size()},{"syncGroupCount",parsed.document->sync_groups.size()},
                            {"dependencies",dependencies}}}};
                } else if(is_tile_palette_asset(*asset)) {
                    const auto parsed=TilemapAssetCodec::parse_palette_json(read_text_file(source_file));
                    if(!parsed)throw std::runtime_error("Tile palette validation failed for "+asset->id);
                    imported_metadata={{"format",parsed.document->schema},{"document",Json::parse(TilemapAssetCodec::write_palette_canonical_json(*parsed.document))},
                        {"runtimeContract",{{"spriteAsset",parsed.document->sprite_asset},{"tileCount",parsed.document->tiles.size()}}}};
                } else if(is_tilemap_asset(*asset)) {
                    const auto parsed=TilemapAssetCodec::parse_tilemap_json(read_text_file(source_file));
                    if(!parsed)throw std::runtime_error("Tilemap validation failed for "+asset->id);
                    std::size_t chunks=0,cells=0;for(const auto& layer:parsed.document->layers){chunks+=layer.chunks.size();for(const auto& chunk:layer.chunks)cells+=chunk.cells.size();}
                    imported_metadata={{"format",parsed.document->schema},{"document",Json::parse(TilemapAssetCodec::write_tilemap_canonical_json(*parsed.document))},
                        {"runtimeContract",{{"paletteAsset",parsed.document->palette_asset},{"layerCount",parsed.document->layers.size()},{"chunkCount",chunks},{"cellCount",cells}}}};
                }
                const Json metadata = {
                    {"schema", "noemancer.cooked-asset/0.1"},
                    {"generatedBy", "asset.cook.apply"},
                    {"targetProfile", plan.at("targetProfile")},
                    {"asset", asset_json(*asset)},
                    {"importer", input.at("importer")},
                    {"payloadUri", payload_uri},
                    {"payloadFormat", payload_format},
                    {"payloadHash",cooked_payload_hash},
                    {"recipeHash",planned_recipe_hash},
                    {"dependencies", cooked_dependencies},
                    {"importedMetadata", imported_metadata}
                };
                std::string write_error;
                if (!write_atomic(metadata_path, metadata.dump(2) + "\n", write_error)) {
                    throw std::runtime_error("Cook metadata write failed: " + write_error);
                }
                if (asset->uri.starts_with("builtin://")) {
                    if (!write_atomic(payload_path, imported_metadata.dump(2) + "\n", write_error)) {
                        throw std::runtime_error("Built-in payload write failed: " + write_error);
                    }
                } else if (mesh_cook||texture_cook||animation_clip||sprite_atlas_cook) {
                    if (cooked_binary_payload.empty() || !write_atomic_bytes(payload_path,
                        std::span<const std::byte>(cooked_binary_payload.data(), cooked_binary_payload.size()),
                        write_error)) {
                        const auto product_kind=mesh_cook?"Mesh":animation_clip?"Animation":
                            sprite_atlas_cook?"Sprite Atlas":"Texture";
                        throw std::runtime_error(std::string(product_kind)+" Cook payload write failed: " + write_error);
                    }
                } else {
                    std::error_code copy_error;
                    std::filesystem::copy_file(
                        source_file,
                        payload_path,
                        std::filesystem::copy_options::overwrite_existing,
                        copy_error);
                    if (copy_error) throw std::runtime_error("Cook payload copy failed: " + copy_error.message());
                }
            }
            const auto metadata_uri = "generated://cook-cache/" + hash + "/asset.json";
            const auto committed_payload_hash=noemancer::sha256_file(payload_path);
            if(!committed_payload_hash.success)
                throw std::runtime_error("Cook payload identity failed for "+asset->id+": "+committed_payload_hash.detail);
            Json output={{"assetId", id}, {"metadataUri", metadata_uri}, {"payloadUri", payload_uri},
                {"payloadFormat", payload_format}, {"payloadHash",committed_payload_hash.value},
                {"buildInputs",input.value("buildInputs",Json::array())},{"cacheHit", cache_hit}};
            Json derived_outputs=Json::array();
            if (sprite_atlas_cook) {
                const auto cached_metadata=Json::parse(read_text_file(metadata_path),nullptr,false);
                const auto* imported=cached_metadata.is_object()&&cached_metadata.contains("importedMetadata")
                    ? &cached_metadata.at("importedMetadata"):nullptr;
                if(imported!=nullptr&&imported->is_object()) {
                    output["displayName"]=asset->display_name;
                    output["kind"]="SpriteAtlas";
                    output["license"]=asset->license;
                    output["redistribution"]=asset->redistribution;
                    output["streamingPolicy"]={{"mode",asset->streaming_mode},
                        {"importance",asset->streaming_importance},{"priority",asset->streaming_priority}};
                    output["tags"]=asset->tags;
                    const auto atlas_manifest=imported->value("atlasArtifact",Json::object());
                    output["dependencies"]=atlas_manifest.value("dependencies",Json::array());
                    output["required"]=!asset->optional;
                    output["sourceAssetId"]=asset->id;
                    derived_outputs=atlas_manifest.value("pageArtifacts",Json::array());
                    output["derivedOutputs"]=derived_outputs;
                    for(const auto& page:derived_outputs)
                        if(page.contains("payloadUri"))receipt["artifacts"].push_back(page.at("payloadUri"));
                }
            }
            outputs.push_back(std::move(output));
            if (sprite_atlas_cook)
                for (const auto& page : derived_outputs) outputs.push_back(page);
            receipt["artifacts"].push_back(metadata_uri);
            receipt["artifacts"].push_back(payload_uri);
        }
        const Json manifest = {
            {"schema", "noemancer.cook-manifest/0.1"},
            {"generatedBy", "asset.cook.apply"},
            {"planId", plan.at("planId")},
            {"contentHash", plan.at("contentHash")},
            {"targetProfile", plan.at("targetProfile")},
            {"registryRevision", revision_},
            {"outputs", std::move(outputs)}
        };
        const auto manifest_path = generated_root / "cook-manifests" /
            (plan.at("planId").get<std::string>() + ".json");
        std::string manifest_error;
        if (!write_atomic(manifest_path, manifest.dump(2) + "\n", manifest_error)) {
            throw std::runtime_error("Cook manifest write failed: " + manifest_error);
        }
        receipt["artifacts"].push_back(
            "generated://cook-manifests/" + manifest_path.filename().string());
    } catch (const std::exception& error) {
        std::error_code cleanup_error;
        for (const auto& page_path : sprite_created_page_paths)
            std::filesystem::remove(page_path, cleanup_error);
        receipt["success"] = false;
        receipt["code"] = "asset.cook-failed";
        receipt["detail"] = error.what();
        receipt["errors"].push_back({{"code", "asset.cook-failed"}, {"message", error.what()}});
    }
    return receipt.dump();
}

std::filesystem::path AssetRegistry::default_asset_root() {
#ifdef NOEMANCER_SOURCE_DIR
    return std::filesystem::path(NOEMANCER_SOURCE_DIR) / "assets";
#else
    return std::filesystem::current_path() / "assets";
#endif
}

} // namespace noemancer
