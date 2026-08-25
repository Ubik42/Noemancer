#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct DecodedImage final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8;
};

struct EncodedPng final {
    bool valid{};
    std::string code;
    std::string detail;
    std::vector<std::uint8_t> bytes;
};

struct DecodedHdrImage final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<float> rgba32f;
};

[[nodiscard]] DecodedImage decode_png_rgba8(std::span<const std::byte> encoded);
[[nodiscard]] DecodedImage decode_jpeg_rgba8(std::span<const std::byte> encoded);
[[nodiscard]] DecodedImage decode_image_rgba8(
    std::span<const std::byte> encoded, std::string_view format_hint);
[[nodiscard]] EncodedPng encode_png_rgba8(std::uint32_t width, std::uint32_t height,
                                          std::span<const std::uint8_t> rgba8);
[[nodiscard]] DecodedHdrImage decode_radiance_hdr(std::span<const std::byte> encoded);

} // namespace noemancer
