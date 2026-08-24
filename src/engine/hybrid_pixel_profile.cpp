#include "engine/hybrid_pixel_profile.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

void add_error(std::vector<HybridPixelProfileError>& errors, std::string code,
               std::string path, std::string message) {
    errors.push_back({std::move(code), std::move(path), std::move(message)});
}

bool fields(const Json& value, const std::initializer_list<std::string_view> allowed,
            const std::string_view path,
            std::vector<HybridPixelProfileError>& errors) {
    if (!value.is_object()) {
        add_error(errors, "hybrid-pixel.invalid-object", std::string(path),
                  "Expected a JSON object.");
        return false;
    }
    bool valid = true;
    for (const auto& [name, unused] : value.items()) {
        static_cast<void>(unused);
        if (std::ranges::none_of(allowed, [&name](const auto candidate) {
                return candidate == name;
            })) {
            add_error(errors, "hybrid-pixel.unknown-field",
                      std::string(path) + "/" + name, "Unknown field.");
            valid = false;
        }
    }
    return valid;
}

bool string_field(const Json& value, const char* name, const std::string_view path,
                  std::string& output,
                  std::vector<HybridPixelProfileError>& errors) {
    const auto field_path = std::string(path) + "/" + name;
    if (!value.contains(name)) {
        add_error(errors, "hybrid-pixel.missing-field", field_path,
                  "Required string field is missing.");
        return false;
    }
    if (!value.at(name).is_string()) {
        add_error(errors, "hybrid-pixel.invalid-string", field_path,
                  "Expected a string.");
        return false;
    }
    output = value.at(name).get<std::string>();
    return true;
}

bool boolean_field(const Json& value, const char* name, const std::string_view path,
                   bool& output,
                   std::vector<HybridPixelProfileError>& errors) {
    const auto field_path = std::string(path) + "/" + name;
    if (!value.contains(name)) {
        add_error(errors, "hybrid-pixel.missing-field", field_path,
                  "Required boolean field is missing.");
        return false;
    }
    if (!value.at(name).is_boolean()) {
        add_error(errors, "hybrid-pixel.invalid-boolean", field_path,
                  "Expected a boolean.");
        return false;
    }
    output = value.at(name).get<bool>();
    return true;
}

bool dimension_field(const Json& value, const char* name, const std::string_view path,
                     std::uint32_t& output,
                     std::vector<HybridPixelProfileError>& errors) {
    const auto field_path = std::string(path) + "/" + name;
    if (!value.contains(name)) {
        add_error(errors, "hybrid-pixel.missing-field", field_path,
                  "Required unsigned integer field is missing.");
        return false;
    }
    if (!value.at(name).is_number_unsigned()) {
        add_error(errors, "hybrid-pixel.invalid-integer", field_path,
                  "Expected an unsigned integer.");
        return false;
    }
    const auto number = value.at(name).get<std::uint64_t>();
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        add_error(errors, "hybrid-pixel.integer-range", field_path,
                  "Value exceeds the supported 32-bit unsigned range.");
        return false;
    }
    output = static_cast<std::uint32_t>(number);
    return true;
}

bool number_field(const Json& value, const char* name, const std::string_view path,
                  float& output,
                  std::vector<HybridPixelProfileError>& errors) {
    const auto field_path = std::string(path) + "/" + name;
    if (!value.contains(name)) {
        add_error(errors, "hybrid-pixel.missing-field", field_path,
                  "Required numeric field is missing.");
        return false;
    }
    if (!value.at(name).is_number()) {
        add_error(errors, "hybrid-pixel.invalid-number", field_path,
                  "Expected a finite number.");
        return false;
    }
    output = value.at(name).get<float>();
    if (!std::isfinite(output)) {
        add_error(errors, "hybrid-pixel.invalid-number", field_path,
                  "Expected a finite number.");
        return false;
    }
    return true;
}

bool has_non_whitespace(const std::string_view text) {
    return std::ranges::any_of(text, [](const unsigned char value) {
        return value > 0x20U;
    });
}

} // namespace

HybridPixelProfileParseResult HybridPixelProfileCodec::parse_json(
    const std::string_view source) {
    HybridPixelProfileParseResult result;
    const auto input = Json::parse(source, nullptr, false);
    if (input.is_discarded() || !input.is_object()) {
        add_error(result.errors, "hybrid-pixel.invalid-json", "/",
                  "Hybrid Pixel Profile must be a JSON object.");
        return result;
    }

    fields(input, {"schema", "profileId", "enabled", "virtualWidth",
                   "virtualHeight", "pixelsPerUnit", "integerScaling",
                   "snapCamera", "snapSprites", "presentationFilter"},
           "", result.errors);

    HybridPixelProfile profile;
    string_field(input, "schema", "", profile.schema, result.errors);
    string_field(input, "profileId", "", profile.profile_id, result.errors);
    boolean_field(input, "enabled", "", profile.enabled, result.errors);
    dimension_field(input, "virtualWidth", "", profile.virtual_width,
                    result.errors);
    dimension_field(input, "virtualHeight", "", profile.virtual_height,
                    result.errors);
    number_field(input, "pixelsPerUnit", "", profile.pixels_per_unit,
                 result.errors);
    boolean_field(input, "integerScaling", "", profile.integer_scaling,
                  result.errors);
    boolean_field(input, "snapCamera", "", profile.snap_camera, result.errors);
    boolean_field(input, "snapSprites", "", profile.snap_sprites, result.errors);
    string_field(input, "presentationFilter", "", profile.presentation_filter,
                 result.errors);

    const auto semantic_errors = validate(profile);
    result.errors.insert(result.errors.end(), semantic_errors.begin(),
                         semantic_errors.end());
    if (result.errors.empty()) {
        result.document = std::move(profile);
    }
    return result;
}

std::vector<HybridPixelProfileError> HybridPixelProfileCodec::validate(
    const HybridPixelProfile& profile) {
    std::vector<HybridPixelProfileError> errors;
    if (profile.schema != hybrid_pixel_profile_schema) {
        add_error(errors, "hybrid-pixel.unsupported-schema", "/schema",
                  "Expected noemancer.hybrid-pixel-profile/0.1.");
    }
    if (!has_non_whitespace(profile.profile_id)) {
        add_error(errors, "hybrid-pixel.empty-profile-id", "/profileId",
                  "profileId must contain a non-whitespace identity.");
    } else if (profile.profile_id.size() > 128U) {
        add_error(errors, "hybrid-pixel.profile-id-range", "/profileId",
                  "profileId must be at most 128 UTF-8 bytes.");
    }
    if (profile.virtual_width < hybrid_pixel_profile_min_dimension ||
        profile.virtual_width > hybrid_pixel_profile_max_dimension) {
        add_error(errors, "hybrid-pixel.dimension-range", "/virtualWidth",
                  "virtualWidth must be in [1,8192].");
    }
    if (profile.virtual_height < hybrid_pixel_profile_min_dimension ||
        profile.virtual_height > hybrid_pixel_profile_max_dimension) {
        add_error(errors, "hybrid-pixel.dimension-range", "/virtualHeight",
                  "virtualHeight must be in [1,8192].");
    }
    if (!std::isfinite(profile.pixels_per_unit) ||
        profile.pixels_per_unit < hybrid_pixel_profile_min_pixels_per_unit ||
        profile.pixels_per_unit > hybrid_pixel_profile_max_pixels_per_unit) {
        add_error(errors, "hybrid-pixel.pixels-per-unit-range",
                  "/pixelsPerUnit", "pixelsPerUnit must be finite in [1,1024].");
    }
    if (profile.presentation_filter != "nearest") {
        add_error(errors, "hybrid-pixel.invalid-filter", "/presentationFilter",
                  "presentationFilter must be nearest for a Hybrid Pixel profile.");
    }
    if (profile.enabled && !profile.integer_scaling) {
        add_error(errors, "hybrid-pixel.integer-scaling-required", "/integerScaling",
                  "An enabled Hybrid Pixel 0.1 profile requires integer scaling.");
    }
    return errors;
}

std::string HybridPixelProfileCodec::write_canonical_json(
    const HybridPixelProfile& profile) {
    const Json output = {
        {"schema", profile.schema},
        {"profileId", profile.profile_id},
        {"enabled", profile.enabled},
        {"virtualWidth", profile.virtual_width},
        {"virtualHeight", profile.virtual_height},
        {"pixelsPerUnit", profile.pixels_per_unit},
        {"integerScaling", profile.integer_scaling},
        {"snapCamera", profile.snap_camera},
        {"snapSprites", profile.snap_sprites},
        {"presentationFilter", profile.presentation_filter},
    };
    return output.dump(2) + "\n";
}

} // namespace noemancer
