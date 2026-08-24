#include "engine/animation_clip_asset.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;
using Errors = std::vector<AnimationClipAssetError>;

constexpr std::size_t max_string_bytes = 4U * 1024U;
constexpr std::size_t max_json_depth = 8U;
constexpr std::uint32_t max_index = 4096U;

void error(Errors& errors, std::string code, std::string path, std::string message) {
    errors.push_back({std::move(code), std::move(path), std::move(message)});
}

void finalize_failure(AnimationClipAssetParseResult& result) {
    if (result.errors.empty()) {
        result.code = "animation.clip.invalid";
        result.detail = "Animation Clip descriptor is invalid.";
        return;
    }
    result.code = result.errors.front().code;
    result.detail = result.errors.front().message;
}

bool depth_is_bounded(const std::string_view source) {
    std::size_t depth = 0U;
    bool in_string = false;
    bool escaped = false;
    for (const char value : source) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '{' || value == '[') {
            if (depth >= max_json_depth) return false;
            ++depth;
        } else if (value == '}' || value == ']') {
            if (depth > 0U) --depth;
        }
    }
    return true;
}

bool known_field(const std::string_view name) {
    return name == "schemaVersion" || name == "assetId" || name == "sourceAsset" ||
           name == "skinIndex" || name == "animationIndex" || name == "compression";
}

bool fields(const Json& value, Errors& errors) {
    bool valid = true;
    for (const auto& [name, unused] : value.items()) {
        static_cast<void>(unused);
        if (!known_field(name)) {
            error(errors, "animation.clip.unknown-field", std::string("/") + name, "Unknown field.");
            valid = false;
        }
    }
    return valid;
}

std::string field_path(const std::string_view name) {
    return "/" + std::string(name);
}

bool has_control_character(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](const char character) {
        return std::iscntrl(static_cast<unsigned char>(character)) != 0;
    });
}

bool text_field(const Json& value, const char* name, std::string& output, Errors& errors) {
    const auto path = field_path(name);
    if (!value.contains(name)) {
        error(errors, "animation.clip.missing-field", path, "Required string field is missing.");
        return false;
    }
    if (!value.at(name).is_string()) {
        error(errors, "animation.clip.invalid-string", path, "Expected a string.");
        return false;
    }
    output = value.at(name).get<std::string>();
    if (output.empty()) {
        error(errors, "animation.clip.empty-string", path, "Value cannot be empty.");
        return false;
    }
    if (output.size() > max_string_bytes) {
        error(errors, "animation.clip.string-too-large", path, "String exceeds the 4096-byte limit.");
        return false;
    }
    if (has_control_character(output)) {
        error(errors, "animation.clip.string-invalid", path, "String cannot contain control characters.");
        return false;
    }
    return true;
}

bool index_field(const Json& value, const char* name, std::uint32_t& output, Errors& errors) {
    const auto path = field_path(name);
    if (!value.contains(name)) {
        error(errors, "animation.clip.missing-field", path, "Required unsigned integer field is missing.");
        return false;
    }
    if (!value.at(name).is_number_unsigned()) {
        error(errors, "animation.clip.invalid-integer", path, "Expected an unsigned integer.");
        return false;
    }
    const auto number = value.at(name).get<std::uint64_t>();
    if (number > max_index) {
        error(errors, "animation.clip.index-range", path, "Index must be in [0,4096].");
        return false;
    }
    output = static_cast<std::uint32_t>(number);
    return true;
}

bool valid_text(const std::string& value, const std::string_view path, Errors& errors) {
    if (value.empty()) {
        error(errors, "animation.clip.empty-string", std::string(path), "Value cannot be empty.");
        return false;
    }
    if (value.size() > max_string_bytes) {
        error(errors, "animation.clip.string-too-large", std::string(path), "String exceeds the 4096-byte limit.");
        return false;
    }
    if (has_control_character(value)) {
        error(errors, "animation.clip.string-invalid", std::string(path),
              "String cannot contain control characters.");
        return false;
    }
    return true;
}

} // namespace

AnimationClipAssetParseResult AnimationClipAssetCodec::parse_json(const std::string_view source) {
    AnimationClipAssetParseResult result;
    if (source.size() > animation_clip_asset_max_source_bytes) {
        error(result.errors, "animation.clip.source-too-large", "/",
              "Animation Clip source exceeds the 65536-byte limit.");
        finalize_failure(result);
        return result;
    }
    if (!depth_is_bounded(source)) {
        error(result.errors, "animation.clip.depth-exceeded", "/",
              "Animation Clip JSON nesting exceeds the supported depth.");
        finalize_failure(result);
        return result;
    }

    Json input;
    try {
        input = Json::parse(source, nullptr, false);
    } catch (const Json::exception&) {
        error(result.errors, "animation.clip.invalid-json", "/", "Animation Clip must be valid JSON.");
        finalize_failure(result);
        return result;
    } catch (const std::exception&) {
        error(result.errors, "animation.clip.parse-failed", "/", "Animation Clip JSON parsing failed.");
        finalize_failure(result);
        return result;
    }
    if (input.is_discarded() || !input.is_object()) {
        error(result.errors, "animation.clip.invalid-json", "/", "Animation Clip must be a JSON object.");
        finalize_failure(result);
        return result;
    }

    bool structurally_valid = fields(input, result.errors);
    AnimationClipAssetDocument document;
    structurally_valid = text_field(input, "schemaVersion", document.schema_version, result.errors) &&
                         structurally_valid;
    structurally_valid = text_field(input, "assetId", document.asset_id, result.errors) && structurally_valid;
    structurally_valid = text_field(input, "sourceAsset", document.source_asset, result.errors) &&
                         structurally_valid;
    structurally_valid = index_field(input, "skinIndex", document.skin_index, result.errors) && structurally_valid;
    structurally_valid = index_field(input, "animationIndex", document.animation_index, result.errors) &&
                         structurally_valid;
    structurally_valid = text_field(input, "compression", document.compression, result.errors) &&
                         structurally_valid;
    if (!structurally_valid) {
        finalize_failure(result);
        return result;
    }

    result.errors = validate(document);
    if (!result.errors.empty()) {
        finalize_failure(result);
        return result;
    }
    result.document = std::move(document);
    result.code = "ok";
    result.detail = "Animation Clip descriptor parsed and normalized.";
    return result;
}

std::vector<AnimationClipAssetError> AnimationClipAssetCodec::validate(
    const AnimationClipAssetDocument& document) {
    Errors errors;
    if (document.schema_version != animation_clip_asset_schema) {
        error(errors, "animation.clip.unsupported-schema", "/schemaVersion",
              "Expected noemancer.animation-clip/0.1.");
    }
    valid_text(document.asset_id, "/assetId", errors);
    valid_text(document.source_asset, "/sourceAsset", errors);
    if (document.skin_index > max_index) {
        error(errors, "animation.clip.index-range", "/skinIndex", "Index must be in [0,4096].");
    }
    if (document.animation_index > max_index) {
        error(errors, "animation.clip.index-range", "/animationIndex", "Index must be in [0,4096].");
    }
    const bool compression_text_valid = valid_text(document.compression, "/compression", errors);
    if (compression_text_valid && document.compression != "ozz_runtime_baseline" &&
        document.compression != "ozz_hierarchical_key_reduction") {
        error(errors, "animation.clip.compression-invalid", "/compression",
              "compression must be ozz_runtime_baseline or ozz_hierarchical_key_reduction.");
    }
    return errors;
}

std::string AnimationClipAssetCodec::write_canonical_json(const AnimationClipAssetDocument& document) {
    if (document.schema_version.size() > max_string_bytes || document.asset_id.size() > max_string_bytes ||
        document.source_asset.size() > max_string_bytes || document.compression.size() > max_string_bytes) {
        return {};
    }
    const Json output{{"schemaVersion", document.schema_version},
                      {"assetId", document.asset_id},
                      {"sourceAsset", document.source_asset},
                      {"skinIndex", document.skin_index},
                      {"animationIndex", document.animation_index},
                      {"compression", document.compression}};
    auto canonical = output.dump(2);
    return canonical.size() <= animation_clip_asset_max_source_bytes ? canonical : std::string{};
}

std::vector<std::string> AnimationClipAssetCodec::build_inputs(
    const AnimationClipAssetDocument& document) {
    std::vector<std::string> result;
    if (!document.source_asset.empty() && document.source_asset.size() <= max_string_bytes &&
        !has_control_character(document.source_asset)) {
        result.push_back(document.source_asset);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace noemancer
