#include "engine/hybrid_pixel_profile.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

using noemancer::HybridPixelProfileError;

bool has_error(const std::vector<HybridPixelProfileError>& errors,
               const std::string_view code, const std::string_view path) {
    for (const auto& error : errors) {
        if (error.code == code && error.path == path && !error.message.empty()) {
            return true;
        }
    }
    return false;
}

int fail(const char* message) {
    std::cerr << "hybrid_pixel_profile_tests: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    using noemancer::HybridPixelProfile;
    using noemancer::HybridPixelProfileCodec;

    const HybridPixelProfile defaults;
    if (!HybridPixelProfileCodec::validate(defaults).empty()) {
        return fail("default profile must satisfy the contract");
    }
    const auto default_json = HybridPixelProfileCodec::write_canonical_json(defaults);
    const auto default_roundtrip = HybridPixelProfileCodec::parse_json(default_json);
    if (!default_roundtrip ||
        HybridPixelProfileCodec::write_canonical_json(*default_roundtrip.document) !=
            default_json ||
        default_roundtrip.document->profile_id != "default" ||
        default_roundtrip.document->presentation_filter != "nearest") {
        return fail("default profile canonical roundtrip drifted");
    }

    // Deliberately reorder keys and use a fractional PPU.  The parsed value
    // must be normalized into the one deterministic canonical representation.
    const auto complete = HybridPixelProfileCodec::parse_json(R"({
      "snapSprites":false,
      "schema":"noemancer.hybrid-pixel-profile/0.1",
      "virtualHeight":360,
      "presentationFilter":"nearest",
      "profileId":"hd2d.main",
      "pixelsPerUnit":24.5,
      "enabled":false,
      "snapCamera":false,
      "integerScaling":false,
      "virtualWidth":640
    })");
    if (!complete || complete.document->profile_id != "hd2d.main" ||
        complete.document->virtual_width != 640U ||
        complete.document->virtual_height != 360U ||
        complete.document->pixels_per_unit != 24.5F ||
        complete.document->enabled || complete.document->integer_scaling ||
        complete.document->snap_camera || complete.document->snap_sprites) {
        return fail("complete profile did not parse into plain data");
    }
    const auto complete_json =
        HybridPixelProfileCodec::write_canonical_json(*complete.document);
    const auto complete_again = HybridPixelProfileCodec::parse_json(complete_json);
    if (!complete_again ||
        HybridPixelProfileCodec::write_canonical_json(*complete_again.document) !=
            complete_json) {
        return fail("complete profile canonical roundtrip drifted");
    }

    const auto unknown_field = HybridPixelProfileCodec::parse_json(
        R"({"schema":"noemancer.hybrid-pixel-profile/0.1","profileId":"x","enabled":true,"virtualWidth":320,"virtualHeight":180,"pixelsPerUnit":16,"integerScaling":true,"snapCamera":true,"snapSprites":true,"presentationFilter":"nearest","futureField":1})");
    if (unknown_field ||
        !has_error(unknown_field.errors, "hybrid-pixel.unknown-field",
                   "/futureField")) {
        return fail("unknown fields must be rejected with a path");
    }

    const auto wrong_schema = HybridPixelProfileCodec::parse_json(
        R"({"schema":"noemancer.hybrid-pixel-profile/0.2","profileId":"x","enabled":true,"virtualWidth":320,"virtualHeight":180,"pixelsPerUnit":16,"integerScaling":true,"snapCamera":true,"snapSprites":true,"presentationFilter":"nearest"})");
    if (wrong_schema ||
        !has_error(wrong_schema.errors, "hybrid-pixel.unsupported-schema",
                   "/schema")) {
        return fail("unknown schema must be rejected");
    }

    const auto missing = HybridPixelProfileCodec::parse_json(
        R"({"schema":"noemancer.hybrid-pixel-profile/0.1","profileId":"x"})");
    if (missing ||
        !has_error(missing.errors, "hybrid-pixel.missing-field", "/enabled") ||
        !has_error(missing.errors, "hybrid-pixel.missing-field", "/virtualWidth")) {
        return fail("missing required fields must be diagnosed");
    }

    const auto wrong_types = HybridPixelProfileCodec::parse_json(
        R"({"schema":"noemancer.hybrid-pixel-profile/0.1","profileId":7,"enabled":"yes","virtualWidth":320.0,"virtualHeight":180,"pixelsPerUnit":"16","integerScaling":true,"snapCamera":true,"snapSprites":true,"presentationFilter":true})");
    if (wrong_types ||
        !has_error(wrong_types.errors, "hybrid-pixel.invalid-string", "/profileId") ||
        !has_error(wrong_types.errors, "hybrid-pixel.invalid-boolean", "/enabled") ||
        !has_error(wrong_types.errors, "hybrid-pixel.invalid-integer",
                   "/virtualWidth") ||
        !has_error(wrong_types.errors, "hybrid-pixel.invalid-number",
                   "/pixelsPerUnit") ||
        !has_error(wrong_types.errors, "hybrid-pixel.invalid-string",
                   "/presentationFilter")) {
        return fail("wrong JSON types must be rejected with diagnostics");
    }

    const auto invalid_values = HybridPixelProfileCodec::parse_json(
        R"({"schema":"noemancer.hybrid-pixel-profile/0.1","profileId":"   ","enabled":true,"virtualWidth":0,"virtualHeight":9000,"pixelsPerUnit":0,"integerScaling":true,"snapCamera":true,"snapSprites":true,"presentationFilter":"linear"})");
    if (invalid_values ||
        !has_error(invalid_values.errors, "hybrid-pixel.empty-profile-id",
                   "/profileId") ||
        !has_error(invalid_values.errors, "hybrid-pixel.dimension-range",
                   "/virtualWidth") ||
        !has_error(invalid_values.errors, "hybrid-pixel.dimension-range",
                   "/virtualHeight") ||
        !has_error(invalid_values.errors,
                   "hybrid-pixel.pixels-per-unit-range", "/pixelsPerUnit") ||
        !has_error(invalid_values.errors, "hybrid-pixel.invalid-filter",
                   "/presentationFilter")) {
        return fail("out-of-range values must be rejected");
    }

    const auto invalid_json = HybridPixelProfileCodec::parse_json("not-json");
    if (invalid_json ||
        !has_error(invalid_json.errors, "hybrid-pixel.invalid-json", "/")) {
        return fail("invalid JSON must be diagnosed");
    }

    auto non_integer = defaults;
    non_integer.integer_scaling = false;
    if (!has_error(HybridPixelProfileCodec::validate(non_integer),
                   "hybrid-pixel.integer-scaling-required", "/integerScaling")) {
        return fail("enabled Hybrid Pixel profiles must retain integer scaling");
    }

    std::cout << "hybrid_pixel_profile_tests: ok\n";
    return 0;
}
