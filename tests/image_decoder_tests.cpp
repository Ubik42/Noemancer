#include "engine/image_decoder.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::string header="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    std::vector<std::byte> payload;
    payload.reserve(header.size()+8U);
    for (const char value:header) payload.push_back(static_cast<std::byte>(value));
    for (const std::uint8_t value:{128U,64U,32U,129U,32U,64U,128U,129U}) payload.push_back(static_cast<std::byte>(value));
    const auto decoded=noemancer::decode_radiance_hdr(payload);
    if (!decoded.valid || decoded.width!=2U || decoded.height!=1U || decoded.rgba32f.size()!=8U ||
        !(decoded.rgba32f[0]>decoded.rgba32f[1] && decoded.rgba32f[1]>decoded.rgba32f[2]) ||
        !(decoded.rgba32f[6]>decoded.rgba32f[5] && decoded.rgba32f[5]>decoded.rgba32f[4])) {
        std::cerr << "Flat Radiance RGBE fixture did not decode to linear RGBA32F\n";
        return 1;
    }
    const std::string truncated="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 8\n";
    std::vector<std::byte> invalid;
    for (const char value:truncated) invalid.push_back(static_cast<std::byte>(value));
    if (noemancer::decode_radiance_hdr(invalid).valid) {
        std::cerr << "Truncated HDR payload was accepted\n";
        return 2;
    }
    return 0;
}
