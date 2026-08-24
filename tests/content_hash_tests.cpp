#include "engine/content_hash.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>

namespace {

bool check(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main() {
    bool valid = true;

    const auto empty = noemancer::sha256_bytes({});
    valid = check(empty.success && empty.code == "ok" && empty.bytes == 0U &&
                      empty.value == "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                  "SHA-256 empty input vector failed.") && valid;

    constexpr std::array<std::uint8_t, 3> abc{{'a', 'b', 'c'}};
    const auto abc_hash = noemancer::sha256_bytes(std::as_bytes(std::span(abc)));
    valid = check(abc_hash.success && abc_hash.code == "ok" && abc_hash.bytes == abc.size() &&
                      abc_hash.value == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                  "SHA-256 abc input vector failed.") && valid;

    std::error_code temporary_error;
    const auto temporary_root = std::filesystem::temp_directory_path(temporary_error);
    if (temporary_error) {
        std::cerr << "Could not locate a temporary directory for the file hash test.\n";
        return 3;
    }
    const auto fixture_path = temporary_root / "noemancer-content-hash-test.bin";
    const auto missing_path = temporary_root / "noemancer-content-hash-missing.bin";
    std::filesystem::remove(fixture_path, temporary_error);
    std::filesystem::remove(missing_path, temporary_error);

    constexpr std::array<std::uint8_t, 9> fixture{{0U, 1U, 2U, 0xffU, 0x80U, 13U, 10U, 42U, 255U}};
    {
        std::ofstream output(fixture_path, std::ios::binary);
        valid = check(static_cast<bool>(output), "Could not create the deterministic file fixture.") && valid;
        if (output) {
            output.write(reinterpret_cast<const char*>(fixture.data()), static_cast<std::streamsize>(fixture.size()));
            valid = check(static_cast<bool>(output), "Could not write the deterministic file fixture.") && valid;
        }
    }

    const auto expected_file_hash = noemancer::sha256_bytes(std::as_bytes(std::span(fixture)));
    const auto file_hash = noemancer::sha256_file(fixture_path);
    const auto repeated_file_hash = noemancer::sha256_file(fixture_path);
    valid = check(expected_file_hash.success && file_hash.success && repeated_file_hash.success &&
                      file_hash.value == expected_file_hash.value && file_hash.value == repeated_file_hash.value &&
                      file_hash.bytes == fixture.size(),
                  "Deterministic file SHA-256 did not match the byte API.") && valid;

    const auto missing_hash = noemancer::sha256_file(missing_path);
    valid = check(!missing_hash.success && !missing_hash.code.empty() && !missing_hash.detail.empty() &&
                      missing_hash.value.empty() && missing_hash.bytes == 0U,
                  "Missing file hashing did not return an explicit failure result.") && valid;

    std::filesystem::remove(fixture_path, temporary_error);
    std::filesystem::remove(missing_path, temporary_error);
    return valid ? 0 : 1;
}
