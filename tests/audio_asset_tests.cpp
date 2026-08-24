#include "engine/audio_asset.hpp"
#include "engine/gameplay_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
void text(std::vector<std::byte>& out,const char* value){for(int i=0;i<4;++i)out.push_back(static_cast<std::byte>(value[i]));}
void u16(std::vector<std::byte>& out,const std::uint16_t v){out.push_back(static_cast<std::byte>(v&255));out.push_back(static_cast<std::byte>(v>>8));}
void u32(std::vector<std::byte>& out,const std::uint32_t v){for(int i=0;i<4;++i)out.push_back(static_cast<std::byte>((v>>(i*8))&255));}
}

int main(){
    std::vector<std::byte> wav; text(wav,"RIFF");u32(wav,44);text(wav,"WAVE");text(wav,"fmt ");u32(wav,16);
    u16(wav,1);u16(wav,1);u32(wav,24000);u32(wav,48000);u16(wav,2);u16(wav,16);text(wav,"data");u32(wav,8);
    u16(wav,0);u16(wav,16384);u16(wav,0);u16(wav,static_cast<std::uint16_t>(-16384));
    auto decoded=noemancer::decode_wav("asset.audio.unit",wav);
    if(!decoded.valid||decoded.clip.sample_rate!=24000||decoded.clip.channels!=1||decoded.clip.frame_count()!=4||decoded.clip.samples[1]<0.49F) return 1;
    const auto metadata=nlohmann::json::parse(noemancer::audio_clip_metadata_json(decoded));
    if(metadata.at("storage")!="resident-f32"||metadata.at("frames")!=4||metadata.at("decoder")!="miniaudio/0.11.25") return 2;
    noemancer::AudioMixerRuntime mixer;
    if(!mixer.register_clip(std::move(decoded.clip))) return 3;
    const auto voice=mixer.play("asset.audio.unit","audio.sfx",1.0F,1.0F,false);
    if(voice==0||!mixer.set_listener({0,0,0},{0,0,-1},{0,1,0})||
       !mixer.set_voice_spatial(voice,true,{2,0,0},1.0F,50.0F,1.0F)) return 3;
    std::vector<float> output(20U*2U);mixer.mix_stereo(output,48000);
    const auto observation=nlohmann::json::parse(mixer.observe_json());
    if(std::ranges::all_of(output,[](float sample){return sample==0.0F;})||observation.at("clips").size()!=1||
       observation.at("schemaVersion")!="noemancer.audio-mixer/0.4"||observation.at("voices").at(0).at("pan").get<float>()<0.99F||
       std::abs(output[3])<=std::abs(output[2])) return 4;
    auto truncated=wav;truncated.resize(20);
    const auto rejected=noemancer::decode_wav("asset.audio.bad",truncated);
    if(rejected.valid||nlohmann::json::parse(noemancer::audio_clip_metadata_json(rejected)).at("assetId")!="asset.audio.bad"||
        nlohmann::json::parse(noemancer::audio_clip_metadata_json(rejected)).at("storage")!="none") return 5;
    return 0;
}
