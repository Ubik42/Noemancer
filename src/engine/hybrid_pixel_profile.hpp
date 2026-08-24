#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// This is an engine-owned, renderer-neutral authoring contract.  It is kept
// deliberately small so the same value can be consumed by the editor, the
// runtime and an Agent without exposing an SDL/RHI or other third-party type.
inline constexpr std::string_view hybrid_pixel_profile_schema =
    "noemancer.hybrid-pixel-profile/0.1";
inline constexpr std::uint32_t hybrid_pixel_profile_min_dimension = 1U;
inline constexpr std::uint32_t hybrid_pixel_profile_max_dimension = 8192U;
inline constexpr float hybrid_pixel_profile_min_pixels_per_unit = 1.0F;
inline constexpr float hybrid_pixel_profile_max_pixels_per_unit = 1024.0F;

struct HybridPixelProfileError final {
    std::string code;
    std::string path;
    std::string message;
};

struct HybridPixelProfile final {
    std::string schema{std::string(hybrid_pixel_profile_schema)};
    // A stable project-local identity.  The default is useful for a new
    // project and avoids an anonymous profile entering persistence.
    std::string profile_id{"default"};
    bool enabled{true};
    std::uint32_t virtual_width{320U};
    std::uint32_t virtual_height{180U};
    float pixels_per_unit{16.0F};
    bool integer_scaling{true};
    bool snap_camera{true};
    bool snap_sprites{true};
    std::string presentation_filter{"nearest"};
};

struct HybridPixelProfileParseResult final {
    std::optional<HybridPixelProfile> document;
    std::vector<HybridPixelProfileError> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return document.has_value();
    }
};

class HybridPixelProfileCodec final {
public:
    [[nodiscard]] static HybridPixelProfileParseResult parse_json(std::string_view json);
    [[nodiscard]] static std::vector<HybridPixelProfileError> validate(
        const HybridPixelProfile& profile);
    [[nodiscard]] static std::string write_canonical_json(
        const HybridPixelProfile& profile);
};

} // namespace noemancer
