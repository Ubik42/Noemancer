#include "engine/gameplay_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace noemancer {
namespace {
using Json = nlohmann::json;

const char* action_kind_name(const InputActionKind kind) {
    return kind == InputActionKind::button ? "button" : "axis1d";
}

const char* audio_storage_name(const AudioAssetStorage storage) {
    return storage == AudioAssetStorage::stream ? "stream" : "resident";
}

float dot3(const std::array<float,3>& a,const std::array<float,3>& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
std::array<float,3> normalize3(std::array<float,3> value){const auto length=std::sqrt(dot3(value,value));if(length<=0.00001F)return {};for(auto& component:value)component/=length;return value;}
std::array<float,3> cross3(const std::array<float,3>& a,const std::array<float,3>& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
}

InputActionRuntime::InputActionRuntime() {
    static_cast<void>(reset_defaults());
}

std::vector<InputActionDefinition> default_input_action_definitions() {
    return {{"gameplay.move.x",InputActionKind::axis_1d,
                {{"keyboard.a",-1.0F,0.0F},{"keyboard.d",1.0F,0.0F},{"gamepad.left.x",1.0F,0.18F}}},
            {"gameplay.jump",InputActionKind::button,
                {{"keyboard.space",1.0F,0.0F},{"gamepad.south",1.0F,0.0F}}},
            {"gameplay.interact",InputActionKind::button,
                {{"keyboard.e",1.0F,0.0F},{"gamepad.west",1.0F,0.0F}}}};
}

bool InputActionRuntime::define(std::string id, const InputActionKind kind, std::vector<InputBinding> bindings) {
    if (id.empty() || bindings.empty() || std::ranges::any_of(actions_, [&](const auto& action) { return action.id == id; })||
        std::ranges::any_of(bindings,[](const auto& binding){return binding.source.empty()||!std::isfinite(binding.scale)||binding.scale==0.0F||
            !std::isfinite(binding.dead_zone)||binding.dead_zone<0.0F||binding.dead_zone>=1.0F;})) return false;
    std::ranges::sort(bindings, {}, &InputBinding::source);
    actions_.push_back({std::move(id), kind, std::move(bindings)});
    std::ranges::sort(actions_, {}, &InputActionState::id);
    ++revision_;
    return true;
}

bool InputActionRuntime::configure(const std::span<const InputActionDefinition> definitions) {
    if(definitions.empty()||definitions.size()>64U)return false;
    std::vector<InputActionState> candidate;
    candidate.reserve(definitions.size());
    for(const auto& definition:definitions) {
        if(definition.id.empty()||definition.bindings.empty()||definition.bindings.size()>16U||
           std::ranges::any_of(candidate,[&](const auto& action){return action.id==definition.id;})||
           std::ranges::any_of(definition.bindings,[](const auto& binding){return binding.source.empty()||!std::isfinite(binding.scale)||
               binding.scale==0.0F||!std::isfinite(binding.dead_zone)||binding.dead_zone<0.0F||binding.dead_zone>=1.0F;}))return false;
        auto bindings=definition.bindings;std::ranges::sort(bindings,{},&InputBinding::source);
        if(std::ranges::adjacent_find(bindings,{},&InputBinding::source)!=bindings.end())return false;
        candidate.push_back({definition.id,definition.kind,std::move(bindings)});
    }
    std::ranges::sort(candidate,{},&InputActionState::id);
    actions_=std::move(candidate);source_values_.clear();++revision_;return true;
}

bool InputActionRuntime::reset_defaults() {return configure(default_input_action_definitions());}

bool InputActionRuntime::set_source_value(const std::string_view source, const float value) {
    if (source.empty() || !std::isfinite(value)) return false;
    source_values_.insert_or_assign(std::string(source), std::clamp(value, -1.0F, 1.0F));
    ++revision_;
    return true;
}

void InputActionRuntime::evaluate() {
    bool changed = false;
    for (auto& action : actions_) {
        action.previous_value = action.value;
        float next{};
        for (const auto& binding : action.bindings) {
            if (const auto found = source_values_.find(binding.source); found != source_values_.end()) {
                const auto magnitude=std::abs(found->second);
                const auto filtered=magnitude<=binding.dead_zone?0.0F:
                    std::copysign((magnitude-binding.dead_zone)/(1.0F-binding.dead_zone),found->second);
                next += filtered * binding.scale;
            }
        }
        next = action.kind == InputActionKind::button ? (next > 0.5F ? 1.0F : 0.0F) : std::clamp(next, -1.0F, 1.0F);
        action.value = next;
        action.pressed = action.previous_value <= 0.5F && next > 0.5F;
        action.released = action.previous_value > 0.5F && next <= 0.5F;
        changed = changed || action.value != action.previous_value || action.pressed || action.released;
    }
    if (changed) ++revision_;
}

std::string InputActionRuntime::observe_json() const {
    Json actions = Json::array();
    for (const auto& action : actions_) {
        Json bindings = Json::array();
        for (const auto& binding : action.bindings) bindings.push_back({{"source", binding.source}, {"scale", binding.scale},
            {"deadZone",binding.dead_zone}});
        actions.push_back({{"id", action.id}, {"kind", action_kind_name(action.kind)}, {"value", action.value},
            {"pressed", action.pressed}, {"released", action.released}, {"bindings", std::move(bindings)}});
    }
    return Json{{"schemaVersion", "noemancer.input-actions/0.2"}, {"revision", revision_},
        {"actionCount",actions_.size()},{"actions", std::move(actions)}}.dump();
}

AudioMixerRuntime::AudioMixerRuntime() : buses_{{"audio.master", "", 1.0F, false}, {"audio.music", "audio.master", 1.0F, false},
                                                   {"audio.sfx", "audio.master", 1.0F, false}, {"audio.ui", "audio.master", 1.0F, false}} {}

bool AudioMixerRuntime::set_bus(const std::string_view id, const float gain, const bool muted) {
    if (!std::isfinite(gain) || gain < 0.0F || gain > 4.0F) return false;
    const auto found = std::ranges::find(buses_, id, &AudioBusState::id);
    if (found == buses_.end()) return false;
    found->gain = gain; found->muted = muted; ++revision_;
    return true;
}

std::uint64_t AudioMixerRuntime::play(std::string asset_id, std::string bus_id, const float gain, const float pitch, const bool looping) {
    if (asset_id.empty() || !std::isfinite(gain) || gain < 0.0F || !std::isfinite(pitch) || pitch <= 0.0F ||
        std::ranges::find(buses_, bus_id, &AudioBusState::id) == buses_.end()) return 0;
    const auto id = next_voice_id_++;
    voices_.push_back({id, std::move(asset_id), std::move(bus_id), gain, pitch, looping, true, 0.0, 0});
    ++revision_;
    return id;
}

bool AudioMixerRuntime::stop(const std::uint64_t voice_id) {
    const auto found = std::ranges::find(voices_, voice_id, &AudioVoiceState::id);
    if (found == voices_.end() || !found->playing) return false;
    found->playing = false; ++revision_; return true;
}

bool AudioMixerRuntime::set_listener(const std::array<float,3> position,const std::array<float,3> forward,const std::array<float,3> up) {
    if(!std::ranges::all_of(position,[](float v){return std::isfinite(v);})||
       !std::ranges::all_of(forward,[](float v){return std::isfinite(v);})||
       !std::ranges::all_of(up,[](float v){return std::isfinite(v);})) return false;
    const auto normalized_forward=normalize3(forward); const auto normalized_up=normalize3(up);
    if(dot3(normalized_forward,normalized_forward)<0.9F||dot3(normalized_up,normalized_up)<0.9F||
       std::abs(dot3(normalized_forward,normalized_up))>0.999F) return false;
    listener_position_=position; listener_forward_=normalized_forward; listener_up_=normalized_up; ++revision_; return true;
}

bool AudioMixerRuntime::set_voice_spatial(const std::uint64_t voice_id,const bool spatial,const std::array<float,3> position,
                                           const float minimum_distance,const float maximum_distance,const float rolloff) {
    const auto found=std::ranges::find(voices_,voice_id,&AudioVoiceState::id);
    if(found==voices_.end()||!std::ranges::all_of(position,[](float v){return std::isfinite(v);})||
       !std::isfinite(minimum_distance)||!std::isfinite(maximum_distance)||!std::isfinite(rolloff)||
       minimum_distance<=0.0F||maximum_distance<minimum_distance||rolloff<0.0F) return false;
    found->spatial=spatial; found->position=position; found->minimum_distance=minimum_distance;
    found->maximum_distance=maximum_distance; found->rolloff=rolloff; ++revision_; return true;
}

bool AudioMixerRuntime::register_clip(AudioClip clip) {
    if(clip.asset_id.empty()||clip.sample_rate==0||clip.channels==0||clip.samples.empty()||clip.samples.size()%clip.channels!=0) return false;
    auto shared=std::make_shared<const AudioClip>(std::move(clip));
    clips_.insert_or_assign(shared->asset_id,std::move(shared)); ++revision_; return true;
}

bool AudioMixerRuntime::register_asset(AudioAssetDescriptor descriptor) {
    if (descriptor.asset_id.empty() || descriptor.content_hash.empty()) return false;
    assets_.insert_or_assign(descriptor.asset_id, std::move(descriptor));
    ++revision_;
    return true;
}

AudioRenderSnapshot AudioMixerRuntime::render_snapshot() const {
    AudioRenderSnapshot snapshot;
    snapshot.revision=revision_;snapshot.buses=buses_;snapshot.voices=voices_;
    snapshot.assets.reserve(assets_.size());
    for (const auto& [id, asset] : assets_) { static_cast<void>(id); snapshot.assets.push_back(asset); }
    std::ranges::sort(snapshot.assets, {}, &AudioAssetDescriptor::asset_id);
    snapshot.clips.reserve(clips_.size());
    for(const auto& [id,clip]:clips_) {static_cast<void>(id);snapshot.clips.push_back(clip);}
    std::ranges::sort(snapshot.clips,{},[](const auto& clip){return clip->asset_id;});
    snapshot.listener_position=listener_position_;snapshot.listener_forward=listener_forward_;snapshot.listener_up=listener_up_;
    return snapshot;
}

void AudioMixerRuntime::synchronize_from(const AudioRenderSnapshot& authored) {
    std::vector<AudioVoiceState> reconciled;
    reconciled.reserve(authored.voices.size());
    for(const auto& incoming:authored.voices) {
        auto voice=incoming;
        const auto current=std::ranges::find(voices_,incoming.id,&AudioVoiceState::id);
        if(current!=voices_.end()) {
            voice.phase=current->phase;
            voice.clip_cursor=current->clip_cursor;
            voice.rendered_frames=current->rendered_frames;
            // A voice completed by the render transport must not be resurrected
            // by a later, unrelated authored-state publication.
            voice.playing=incoming.playing&&current->playing;
        }
        reconciled.push_back(std::move(voice));
    }
    buses_=authored.buses; voices_=std::move(reconciled);assets_.clear();clips_.clear();
    for (const auto& asset : authored.assets) assets_.insert_or_assign(asset.asset_id, asset);
    for(const auto& clip:authored.clips) if(clip) clips_.insert_or_assign(clip->asset_id,clip);
    next_voice_id_=1U;for(const auto& voice:voices_)next_voice_id_=std::max(next_voice_id_,voice.id+1U);
    listener_position_=authored.listener_position; listener_forward_=authored.listener_forward; listener_up_=authored.listener_up;
    revision_=std::max(revision_,authored.revision)+1U;
}

void AudioMixerRuntime::mix_stereo(const std::span<float> output, const std::uint32_t sample_rate) {
    std::ranges::fill(output,0.0F);
    if(sample_rate==0||output.size()%2U!=0U) return;
    const auto master=std::ranges::find(buses_,std::string_view{"audio.master"},&AudioBusState::id);
    const auto frames=output.size()/2U;
    for(auto& voice:voices_) {
        if(!voice.playing) continue;
        const auto bus=std::ranges::find(buses_,voice.bus_id,&AudioBusState::id);
        if(bus==buses_.end()||bus->muted||master==buses_.end()||master->muted) continue;
        auto gain=voice.gain*bus->gain*master->gain; float left_spatial=1.0F,right_spatial=1.0F;
        if(voice.spatial) {
            const std::array<float,3> delta{voice.position[0]-listener_position_[0],voice.position[1]-listener_position_[1],voice.position[2]-listener_position_[2]};
            const auto distance=std::sqrt(dot3(delta,delta));
            if(distance>=voice.maximum_distance) gain=0.0F;
            else if(distance>voice.minimum_distance) gain*=1.0F/(1.0F+voice.rolloff*(distance-voice.minimum_distance));
            const auto right=normalize3(cross3(listener_forward_,listener_up_));
            const auto pan=distance>0.00001F?std::clamp(dot3(normalize3(delta),right),-1.0F,1.0F):0.0F;
            left_spatial=std::sqrt(0.5F*(1.0F-pan)); right_spatial=std::sqrt(0.5F*(1.0F+pan));
        }
        if(const auto clip=clips_.find(voice.asset_id);clip!=clips_.end()) {
            const auto clip_frames=clip->second->frame_count(); const auto channels=clip->second->channels;
            const auto step=static_cast<double>(clip->second->sample_rate)*voice.pitch/sample_rate;
            for(std::size_t frame=0;frame<frames&&voice.playing;++frame) {
                if(voice.clip_cursor>=clip_frames) {if(voice.looping) voice.clip_cursor=std::fmod(voice.clip_cursor,static_cast<double>(clip_frames));else {voice.playing=false;break;}}
                const auto first=static_cast<std::size_t>(voice.clip_cursor);
                const auto second=voice.looping?(first+1U)%clip_frames:std::min(first+1U,clip_frames-1U);
                const auto alpha=static_cast<float>(voice.clip_cursor-first);
                const auto sample_channel=[&](const std::size_t channel){const auto c=std::min<std::size_t>(channel,channels-1U);
                    return std::lerp(clip->second->samples[first*channels+c],clip->second->samples[second*channels+c],alpha);};
                output[frame*2U]+=gain*left_spatial*sample_channel(0); output[frame*2U+1U]+=gain*right_spatial*sample_channel(channels>1?1:0);
                voice.clip_cursor+=step;
            }
        } else {
            std::uint32_t hash=2166136261U; for(const unsigned char c:voice.asset_id){hash^=c;hash*=16777619U;}
            const auto frequency=220.0+static_cast<double>(hash%440U);
            for(std::size_t frame=0;frame<frames;++frame) {
                const auto sample=0.08F*gain*static_cast<float>(std::sin(voice.phase));
                output[frame*2U]+=left_spatial*sample; output[frame*2U+1U]+=right_spatial*sample;
                voice.phase+=6.283185307179586*frequency*voice.pitch/static_cast<double>(sample_rate);
                if(voice.phase>6.283185307179586) voice.phase=std::fmod(voice.phase,6.283185307179586);
            }
        }
        voice.rendered_frames+=frames;
        if(!clips_.contains(voice.asset_id)&&!voice.looping&&voice.rendered_frames>=sample_rate/4U) voice.playing=false;
    }
    for(auto& sample:output) sample=std::clamp(sample,-1.0F,1.0F);
    mixed_frames_+=frames;
    ++revision_;
}

std::string AudioMixerRuntime::observe_json() const {
    Json buses = Json::array(); Json voices = Json::array();
    for (const auto& bus : buses_) buses.push_back({{"id", bus.id}, {"parent", bus.parent}, {"gain", bus.gain}, {"muted", bus.muted}});
    const auto right=normalize3(cross3(listener_forward_,listener_up_));
    for (const auto& voice : voices_) { const std::array<float,3> delta{voice.position[0]-listener_position_[0],voice.position[1]-listener_position_[1],voice.position[2]-listener_position_[2]};
        const auto distance=std::sqrt(dot3(delta,delta)); const auto pan=voice.spatial&&distance>0.00001F?std::clamp(dot3(normalize3(delta),right),-1.0F,1.0F):0.0F;
        const auto attenuation=!voice.spatial?1.0F:distance>=voice.maximum_distance?0.0F:distance<=voice.minimum_distance?1.0F:1.0F/(1.0F+voice.rolloff*(distance-voice.minimum_distance));
        voices.push_back({{"id", voice.id}, {"assetId", voice.asset_id}, {"busId", voice.bus_id},
        {"gain", voice.gain}, {"pitch", voice.pitch}, {"looping", voice.looping}, {"playing", voice.playing},
        {"spatial",voice.spatial},{"position",voice.position},{"minimumDistance",voice.minimum_distance},{"maximumDistance",voice.maximum_distance},
        {"rolloff",voice.rolloff},{"distance",distance},{"attenuation",attenuation},{"pan",pan},
        {"renderedFrames",voice.rendered_frames},{"clipCursor",voice.clip_cursor}}); }
    Json assets=Json::array(); std::vector<AudioAssetDescriptor> ordered_assets; ordered_assets.reserve(assets_.size());
    for(const auto& [id,asset]:assets_) { static_cast<void>(id); ordered_assets.push_back(asset); }
    std::ranges::sort(ordered_assets,{},&AudioAssetDescriptor::asset_id);
    for(const auto& asset:ordered_assets) assets.push_back({{"assetId",asset.asset_id},{"contentHash",asset.content_hash},{"storage",audio_storage_name(asset.storage)}});
    Json clips=Json::array(); for(const auto& [id,clip]:clips_) clips.push_back({{"assetId",id},{"sampleRate",clip->sample_rate},{"channels",clip->channels},
        {"frames",clip->frame_count()},{"storage","shared-immutable-f32"}});
    return Json{{"schemaVersion", "noemancer.audio-mixer/0.4"}, {"revision", revision_}, {"backend", "pcm-f32-stereo-spatial/0.3"},
        {"sampleRate",48000},{"mixedFrames",mixed_frames_},{"listener",{{"position",listener_position_},{"forward",listener_forward_},{"up",listener_up_}}},
        {"spatialization",{{"panning","equal-power"},{"attenuation","inverse-distance-clamped"}}},
        {"assets",std::move(assets)},{"clips",std::move(clips)},{"buses", std::move(buses)}, {"voices", std::move(voices)}}.dump();
}

void GameplayRuntime::update_from_input(const InputActionRuntime& input) {
    for (const auto& action : input.actions()) {
        if (action.pressed) emit("input.action.pressed", action.id, "gameplay.world", Json{{"value", action.value}}.dump());
        if (action.released) emit("input.action.released", action.id, "gameplay.world", Json{{"value", action.value}}.dump());
    }
}

std::uint64_t GameplayRuntime::emit(std::string type, std::string source, std::string target, std::string payload_json) {
    if (type.empty()) return 0;
    try { static_cast<void>(Json::parse(payload_json)); } catch (...) { payload_json = Json{{"invalidPayload", true}}.dump(); }
    const auto sequence=next_sequence_++; events_.push_back({sequence, std::move(type), std::move(source), std::move(target), std::move(payload_json)});
    if (events_.size() > 256) events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(events_.size() - 256));
    ++revision_;
    return sequence;
}

std::string GameplayRuntime::observe_json(const std::size_t max_events) const {
    Json events = Json::array();
    const auto start = events_.size() > max_events ? events_.size() - max_events : 0U;
    for (std::size_t index = start; index < events_.size(); ++index) {
        const auto& event = events_[index];
        events.push_back({{"sequence", event.sequence}, {"type", event.type}, {"source", event.source}, {"target", event.target},
            {"payload", Json::parse(event.payload_json)}});
    }
    return Json{{"schemaVersion", "noemancer.gameplay-events/0.1"}, {"revision", revision_}, {"events", std::move(events)}}.dump();
}

} // namespace noemancer
