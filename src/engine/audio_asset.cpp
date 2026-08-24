#include "engine/audio_asset.hpp"

#include <miniaudio.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>

namespace noemancer {
namespace {
using Json=nlohmann::json;
}

AudioDecodeResult decode_audio(std::string asset_id,const std::span<const std::byte> bytes) {
    AudioDecodeResult result{.code="audio.decode-failed",.detail="Audio source is empty or unsupported.",.clip={.asset_id=std::move(asset_id)}};
    if(bytes.empty())return result;
    auto config=ma_decoder_config_init(ma_format_f32,0,0);
    ma_decoder decoder{};
    const auto initialized=ma_decoder_init_memory(bytes.data(),bytes.size(),&config,&decoder);
    if(initialized!=MA_SUCCESS){result.detail=std::string("miniaudio: ")+ma_result_description(initialized);return result;}
    ma_uint64 frame_count{};
    const auto measured=ma_decoder_get_length_in_pcm_frames(&decoder,&frame_count);
    if(measured!=MA_SUCCESS||frame_count==0||decoder.outputChannels==0||decoder.outputChannels>8) {
        result.code="audio.empty-or-unbounded";result.detail=std::string("miniaudio: ")+ma_result_description(measured);
        ma_decoder_uninit(&decoder);return result;
    }
    result.clip.sample_rate=decoder.outputSampleRate;result.clip.channels=static_cast<std::uint16_t>(decoder.outputChannels);
    result.clip.samples.resize(static_cast<std::size_t>(frame_count)*decoder.outputChannels);
    ma_uint64 frames_read{};
    const auto decoded=ma_decoder_read_pcm_frames(&decoder,result.clip.samples.data(),frame_count,&frames_read);
    ma_decoder_uninit(&decoder);
    result.clip.samples.resize(static_cast<std::size_t>(frames_read)*result.clip.channels);
    result.valid=(decoded==MA_SUCCESS||decoded==MA_AT_END)&&frames_read>0;
    result.code=result.valid?"ok":"audio.decode-failed";
    result.detail=result.valid?"Decoded by miniaudio 0.11.25 to interleaved float PCM.":std::string("miniaudio: ")+ma_result_description(decoded);
    return result;
}

AudioDecodeResult decode_wav(std::string asset_id,const std::span<const std::byte> bytes) {return decode_audio(std::move(asset_id),bytes);}

AudioDecodeResult decode_wav_file(std::string asset_id,const std::filesystem::path& path) {
    return decode_audio_file(std::move(asset_id),path);
}

AudioDecodeResult decode_audio_file(std::string asset_id,const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary); if(!stream) return {.code="audio.file-unavailable",.detail="Audio source file could not be opened."};
    std::vector<char> raw{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
    return decode_audio(std::move(asset_id),{reinterpret_cast<const std::byte*>(raw.data()),raw.size()});
}

std::string audio_clip_metadata_json(const AudioDecodeResult& result) {
    return Json{{"schemaVersion","noemancer.audio-clip/0.1"},{"valid",result.valid},{"code",result.code},{"detail",result.detail},
        {"assetId",result.clip.asset_id},{"sampleRate",result.clip.sample_rate},{"channels",result.clip.channels},
        {"frames",result.clip.frame_count()},{"durationSeconds",result.clip.sample_rate?static_cast<double>(result.clip.frame_count())/result.clip.sample_rate:0.0},
        {"storage",result.valid?"resident-f32":"none"},{"decoder","miniaudio/0.11.25"}}.dump();
}

} // namespace noemancer
