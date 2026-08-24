#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace noemancer {

struct ContentHashResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string value;
    std::uintmax_t bytes{};
};

[[nodiscard]] ContentHashResult sha256_bytes(std::span<const std::byte> bytes);

[[nodiscard]] ContentHashResult sha256_file(
    const std::filesystem::path& path,
    std::size_t max_bytes = 512U * 1024U * 1024U);

} // namespace noemancer
