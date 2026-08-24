#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace noemancer {

struct AudioClip final {
    std::string asset_id;
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    std::vector<float> samples;
    [[nodiscard]] std::size_t frame_count() const noexcept { return channels==0?0:samples.size()/channels; }
};

struct AudioDecodeResult final {
    bool valid{};
    std::string code;
    std::string detail;
    AudioClip clip;
};

[[nodiscard]] AudioDecodeResult decode_wav(std::string asset_id,std::span<const std::byte> bytes);
[[nodiscard]] AudioDecodeResult decode_wav_file(std::string asset_id,const std::filesystem::path& path);
[[nodiscard]] AudioDecodeResult decode_audio(std::string asset_id,std::span<const std::byte> bytes);
[[nodiscard]] AudioDecodeResult decode_audio_file(std::string asset_id,const std::filesystem::path& path);
[[nodiscard]] std::string audio_clip_metadata_json(const AudioDecodeResult& result);

} // namespace noemancer
