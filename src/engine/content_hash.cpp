#include "engine/content_hash.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <utility>

namespace noemancer {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

constexpr std::array<std::uint32_t, 8> initial_hash{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

constexpr std::uint32_t rotate_right(const std::uint32_t value, const std::uint32_t count) noexcept {
    return (value >> count) | (value << (32U - count));
}

class Sha256State final {
public:
    void update(const std::span<const std::byte> bytes) noexcept {
        bit_length_ += static_cast<std::uint64_t>(bytes.size()) * 8ULL;
        auto input = bytes;
        while (!input.empty()) {
            const auto copy_size = std::min(input.size(), block_.size() - block_size_);
            std::memcpy(block_.data() + block_size_, input.data(), copy_size);
            block_size_ += copy_size;
            input = input.subspan(copy_size);
            if (block_size_ == block_.size()) {
                compress();
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] std::array<std::uint32_t, 8> finish() noexcept {
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), std::uint8_t{0U});
            compress();
            block_size_ = 0U;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56, std::uint8_t{0U});
        for (std::size_t index = 0U; index < sizeof(bit_length_); ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(bit_length_ >> (index * 8U));
        }
        compress();
        return hash_;
    }

private:
    void compress() noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const auto base = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block_[base]) << 24U) |
                (static_cast<std::uint32_t>(block_[base + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block_[base + 2U]) << 8U) |
                static_cast<std::uint32_t>(block_[base + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const auto first = words[index - 15U];
            const auto second = words[index - 2U];
            const auto small_zero = rotate_right(first, 7U) ^ rotate_right(first, 18U) ^ (first >> 3U);
            const auto small_one = rotate_right(second, 17U) ^ rotate_right(second, 19U) ^ (second >> 10U);
            words[index] = words[index - 16U] + small_zero + words[index - 7U] + small_one;
        }

        auto a = hash_[0];
        auto b = hash_[1];
        auto c = hash_[2];
        auto d = hash_[3];
        auto e = hash_[4];
        auto f = hash_[5];
        auto g = hash_[6];
        auto h = hash_[7];
        for (std::size_t index = 0U; index < words.size(); ++index) {
            const auto large_one = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temporary_one = h + large_one + choose + round_constants[index] + words[index];
            const auto large_zero = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary_two = large_zero + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary_one;
            d = c;
            c = b;
            b = a;
            a = temporary_one + temporary_two;
        }
        hash_[0] += a;
        hash_[1] += b;
        hash_[2] += c;
        hash_[3] += d;
        hash_[4] += e;
        hash_[5] += f;
        hash_[6] += g;
        hash_[7] += h;
    }

    std::array<std::uint32_t, 8> hash_{initial_hash};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_{};
    std::uint64_t bit_length_{};
};

std::string digest_string(const std::array<std::uint32_t, 8>& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"sha256:"};
    result.reserve(71U);
    for (const auto word : digest) {
        for (int shift = 28; shift >= 0; shift -= 4)
            result.push_back(hex[(word >> static_cast<unsigned int>(shift)) & 0x0fU]);
    }
    return result;
}

ContentHashResult failure(std::string code, std::string detail, const std::uintmax_t bytes = 0U) {
    return ContentHashResult{false, std::move(code), std::move(detail), {}, bytes};
}

} // namespace

ContentHashResult sha256_bytes(const std::span<const std::byte> bytes) {
    Sha256State state;
    state.update(bytes);
    return ContentHashResult{
        true,
        "ok",
        "SHA-256 computed over the provided bytes.",
        digest_string(state.finish()),
        static_cast<std::uintmax_t>(bytes.size())
    };
}

ContentHashResult sha256_file(const std::filesystem::path& path, const std::size_t max_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return failure("content-hash.file-open-failed", "Could not open the file for hashing.");

    std::error_code size_error;
    const auto declared_size = std::filesystem::file_size(path, size_error);
    if (size_error) return failure("content-hash.file-stat-failed", "Could not determine the file size.");
    const auto maximum = static_cast<std::uintmax_t>(max_bytes);
    if (declared_size > maximum)
        return failure("content-hash.file-too-large", "The file exceeds the configured hashing limit.");

    constexpr std::size_t chunk_size = 64U * 1024U;
    std::array<char, chunk_size> buffer{};
    Sha256State state;
    std::uintmax_t total{};
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read_count = input.gcount();
        if (read_count < 0) return failure("content-hash.file-read-failed", "The file read returned an invalid size.", total);
        if (read_count > 0) {
            const auto count = static_cast<std::uintmax_t>(read_count);
            if (count > maximum - total)
                return failure("content-hash.file-too-large", "The file exceeds the configured hashing limit.", total);
            state.update(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buffer.data()), static_cast<std::size_t>(read_count)));
            total += count;
        }
        if (input.bad()) return failure("content-hash.file-read-failed", "The file could not be read.", total);
        if (input.eof()) break;
        if (input.fail() || read_count == 0)
            return failure("content-hash.file-read-failed", "The file could not be read.", total);
    }
    if (total != declared_size)
        return failure("content-hash.file-changed", "The file changed while it was being hashed.", total);

    return ContentHashResult{
        true,
        "ok",
        "SHA-256 computed over the file bytes.",
        digest_string(state.finish()),
        total
    };
}

} // namespace noemancer
