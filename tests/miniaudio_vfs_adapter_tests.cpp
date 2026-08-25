#include "runtime/miniaudio_vfs_adapter.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct TempDirectory final {
    std::filesystem::path path;
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

[[nodiscard]] std::vector<std::uint8_t> wav_fixture() {
    // PCM mono, 8-bit, 8 kHz, eight frames. It is intentionally small but has
    // a real RIFF/WAVE header so the byte ranges match a production decoder.
    return {
        'R','I','F','F', 44,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        0x40,0x1f,0,0, 0x40,0x1f,0,0, 1,0, 8,0,
        'd','a','t','a', 8,0,0,0,
        0x00,0x20,0x40,0x60,0x80,0xa0,0xc0,0xff};
}

[[nodiscard]] bool write_fixture(const std::filesystem::path& path,
                                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool expect(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

[[nodiscard]] bool exercise_read_seek(noemancer::MiniaudioVfsAdapter& adapter,
                                      const std::vector<std::uint8_t>& fixture) {
    auto* vfs = adapter.native_vfs();
    ma_vfs_file file{};
    if (!expect(ma_vfs_open(vfs, "asset://audio/test.wav", MA_OPEN_MODE_READ, &file) == MA_SUCCESS,
                "asset URI should open")) return false;

    ma_file_info info{};
    if (!expect(ma_vfs_info(vfs, file, &info) == MA_SUCCESS && info.sizeInBytes == fixture.size(),
                "file info should report WAV byte length")) return false;

    std::array<std::uint8_t, 12> header{};
    size_t read{};
    if (!expect(ma_vfs_read(vfs, file, header.data(), header.size(), &read) == MA_SUCCESS &&
                    read == header.size() && std::equal(header.begin(), header.end(), fixture.begin()),
                "range read should return RIFF header")) return false;

    if (!expect(ma_vfs_seek(vfs, file, 44, ma_seek_origin_start) == MA_SUCCESS,
                "absolute seek should reach PCM payload")) return false;
    std::array<std::uint8_t, 4> samples{};
    if (!expect(ma_vfs_read(vfs, file, samples.data(), samples.size(), &read) == MA_SUCCESS &&
                    read == samples.size() && samples[0] == 0x00 && samples[3] == 0x60,
                "payload range should match WAV samples")) return false;

    if (!expect(ma_vfs_seek(vfs, file, -2, ma_seek_origin_end) == MA_SUCCESS,
                "end-relative seek should work")) return false;
    std::array<std::uint8_t, 8> tail{};
    if (!expect(ma_vfs_read(vfs, file, tail.data(), tail.size(), &read) == MA_SUCCESS &&
                    read == 2U && tail[0] == 0xc0 && tail[1] == 0xff,
                "read should stop at EOF without overrun")) return false;
    if (!expect(ma_vfs_read(vfs, file, tail.data(), 1U, &read) == MA_AT_END && read == 0U,
                "read at EOF should return MA_AT_END")) return false;

    if (!expect(ma_vfs_close(vfs, file) == MA_SUCCESS, "close should release handle")) return false;
    if (!expect(ma_vfs_read(vfs, file, tail.data(), 1U, &read) == MA_INVALID_OPERATION,
                "closed handle must fail without dereferencing stale memory")) return false;
    return expect(adapter.last_status().code == noemancer::MiniaudioVfsStatusCode::closed_handle,
                  "closed handle should publish stable adapter status");
}

} // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    TempDirectory temp{std::filesystem::temp_directory_path() /
                       ("noemancer-miniaudio-vfs-" + std::to_string(nonce))};
    std::error_code error;
    std::filesystem::create_directories(temp.path / "audio", error);
    const auto fixture = wav_fixture();
    if (!expect(!error && write_fixture(temp.path / "audio" / "test.wav", fixture),
                "WAV fixture setup failed")) return 1;

    noemancer::VirtualFileSystem vfs;
    const auto mounted = vfs.mount(noemancer::VfsMountSpec{
        .id = "test.assets", .virtual_root = "asset://", .source_root = temp.path});
    if (!expect(mounted.success, "fixture mount failed")) return 2;

    // Resident path.
    noemancer::MiniaudioVfsAdapter resident(vfs, {
        .max_open_handles = 2U, .max_resident_file_bytes = 1024U, .max_read_bytes_per_call = 1024U});
    if (!exercise_read_seek(resident, fixture)) return 3;

    ma_vfs_file invalid{};
    if (!expect(ma_vfs_open(resident.native_vfs(), "audio/test.wav", MA_OPEN_MODE_READ, &invalid) == MA_INVALID_FILE &&
                    resident.last_status().code == noemancer::MiniaudioVfsStatusCode::invalid_uri,
                "non-asset URI should map to stable invalid-uri status")) return 4;
    if (!expect(ma_vfs_open(resident.native_vfs(), "asset://audio/test.wav", MA_OPEN_MODE_WRITE, &invalid) == MA_ACCESS_DENIED,
                "write open should be rejected")) return 5;

    // Force range-backed streaming and verify the same callback contract.
    noemancer::MiniaudioVfsAdapter streaming(vfs, {
        .max_open_handles = 2U, .max_resident_file_bytes = 1U, .max_read_bytes_per_call = 1024U});
    if (!exercise_read_seek(streaming, fixture)) return 6;

    // Exercise the actual decoder entry point, not only callback helpers. This
    // proves the bridge layout and lifetime satisfy miniaudio's VFS ABI.
    auto decoder_config = ma_decoder_config_init(ma_format_f32, 1U, 8000U);
    ma_decoder decoder{};
    if (!expect(ma_decoder_init_vfs(streaming.native_vfs(), "asset://audio/test.wav",
                                    &decoder_config, &decoder) == MA_SUCCESS,
                "miniaudio decoder should initialize directly from asset URI")) return 7;
    std::array<float, 8> decoded{};
    ma_uint64 decoded_frames{};
    const auto decode_result = ma_decoder_read_pcm_frames(&decoder, decoded.data(), decoded.size(), &decoded_frames);
    if (!expect((decode_result == MA_SUCCESS || decode_result == MA_AT_END) && decoded_frames == decoded.size(),
                "miniaudio decoder should consume all WAV frames through VFS")) {
        ma_decoder_uninit(&decoder);
        return 8;
    }
    ma_decoder_uninit(&decoder);
    if (!expect(streaming.active_handles() == 0U, "decoder teardown should close its VFS handle")) return 9;

    ma_vfs_file first{};
    ma_vfs_file second{};
    ma_vfs_file over_budget{};
    if (!expect(ma_vfs_open(streaming.native_vfs(), "asset://audio/test.wav", MA_OPEN_MODE_READ, &first) == MA_SUCCESS &&
                    ma_vfs_open(streaming.native_vfs(), "asset://audio/test.wav", MA_OPEN_MODE_READ, &second) == MA_SUCCESS &&
                    streaming.active_handles() == 2U && streaming.stream_handles() == 2U,
                "multiple streaming handles should remain independently bounded")) return 10;
    if (!expect(ma_vfs_open(streaming.native_vfs(), "asset://audio/test.wav", MA_OPEN_MODE_READ, &over_budget) ==
                    MA_TOO_MANY_OPEN_FILES &&
                    streaming.last_status().code == noemancer::MiniaudioVfsStatusCode::handle_budget_exceeded,
                "third handle should hit configured budget")) return 11;

    std::atomic<bool> concurrent_ok{true};
    auto reader = [&](ma_vfs_file handle, std::uint8_t expected) {
        std::array<std::uint8_t, 4> bytes{};
        size_t count{};
        if (ma_vfs_seek(streaming.native_vfs(), handle, 44, ma_seek_origin_start) != MA_SUCCESS ||
            ma_vfs_read(streaming.native_vfs(), handle, bytes.data(), bytes.size(), &count) != MA_SUCCESS ||
            count != bytes.size() || bytes.front() != expected) concurrent_ok.store(false);
    };
    std::thread left(reader, first, 0x00);
    std::thread right(reader, second, 0x00);
    left.join();
    right.join();
    if (!expect(concurrent_ok.load(), "independent handles should support concurrent range reads")) return 12;

    if (!expect(ma_vfs_close(streaming.native_vfs(), first) == MA_SUCCESS &&
                    ma_vfs_open(streaming.native_vfs(), "asset://audio/test.wav", MA_OPEN_MODE_READ, &over_budget) == MA_SUCCESS,
                "closing one handle should immediately return budget")) return 13;
    if (!expect(ma_vfs_close(streaming.native_vfs(), second) == MA_SUCCESS &&
                    ma_vfs_close(streaming.native_vfs(), over_budget) == MA_SUCCESS &&
                    streaming.active_handles() == 0U,
                "all handles should close cleanly")) return 14;

    return 0;
}
