#include "runtime/miniaudio_render_graph.hpp"

#include <miniaudio.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

constexpr const char* kBusIds[]{"audio.master", "audio.music", "audio.sfx", "audio.ui"};

[[nodiscard]] float effective_gain(const AudioBusState& bus) {
    return bus.muted ? 0.0F : std::clamp(bus.gain, 0.0F, 4.0F);
}

[[nodiscard]] const char* storage_name(const AudioSourceStorage storage) noexcept {
    return storage == AudioSourceStorage::stream ? "stream" : "resident";
}

[[nodiscard]] AudioSourceStorage runtime_storage(const AudioAssetStorage storage) noexcept {
    return storage == AudioAssetStorage::stream ? AudioSourceStorage::stream : AudioSourceStorage::resident;
}

[[nodiscard]] ma_uint32 storage_flags(const AudioSourceStorage storage) noexcept {
    return storage == AudioSourceStorage::stream
        ? MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM
        : MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE;
}

[[nodiscard]] const char* result_name(const ma_result result) noexcept {
    if (result == MA_SUCCESS) return "success";
    if (result == MA_BUSY) return "busy";
    if (result == MA_AT_END) return "at-end";
    return "error";
}

[[nodiscard]] bool valid_clip(const std::shared_ptr<const AudioClip>& clip) {
    return clip && !clip->asset_id.empty() && clip->sample_rate > 0U && clip->channels > 0U &&
        !clip->samples.empty() && clip->samples.size() % clip->channels == 0U;
}

} // namespace

struct MiniaudioRenderGraph::Impl final {
    struct Clip final {
        std::shared_ptr<const AudioClip> source;
        ma_audio_buffer buffer{};
        bool ready{};

        ~Clip() {
            if (ready) ma_audio_buffer_uninit(&buffer);
        }
    };

    struct SourceStatus final {
        AudioSourceStorage storage{AudioSourceStorage::resident};
        std::string content_hash;
        std::string state{"unrequested"};
        ma_result result{MA_BUSY};
        ma_uint64 available_frames{};
        ma_uint64 length_frames{};
    };

    struct Voice final {
        std::string asset_id;
        std::string bus_id;
        std::filesystem::path source_path;
        std::string content_hash;
        AudioSourceStorage storage{AudioSourceStorage::resident};
        bool resource_source{};
        bool looping{};
        bool sound_ready{};
        bool source_ready{};
        ma_resource_manager_data_source source{};
        ma_sound sound{};
        std::shared_ptr<Clip> clip;
        std::optional<ma_uint64> pending_seek;

        ~Voice() {
            // A sound initialized from an external data source does not own
            // that source. Detach/uninit the graph node before freeing it.
            if (sound_ready) ma_sound_uninit(&sound);
            if (source_ready) ma_resource_manager_data_source_uninit(&source);
        }
    };

    ma_resource_manager resource_manager{};
    bool resource_manager_ready{};
    ma_engine engine{};
    bool engine_ready{};
    std::unordered_map<std::string, std::unique_ptr<ma_sound_group>> groups;
    std::unordered_map<std::string, std::shared_ptr<Clip>> clips;
    std::unordered_map<std::uint64_t, std::unique_ptr<Voice>> voices;
    std::unordered_map<std::string, AudioSourceLocation> source_catalog;
    std::unordered_map<std::string, SourceStatus> source_status;
    std::unordered_set<std::uint64_t> retired_voice_ids;
    std::uint32_t sample_rate{};
    std::uint32_t channels{};
    std::uint32_t resource_job_threads{1U};
    std::uint64_t revision{};
    std::uint64_t reconciliations{};
    std::uint64_t rendered_frames{};
    std::size_t missing_clip_voices{};

    [[nodiscard]] ma_sound_group* group(const std::string_view id) const {
        if (const auto found = groups.find(std::string(id)); found != groups.end()) return found->second.get();
        if (const auto master = groups.find("audio.master"); master != groups.end()) return master->second.get();
        return nullptr;
    }

    void set_source_status(const std::string_view asset_id, const AudioSourceStorage storage,
                           const std::string_view hash, const std::string_view state,
                           const ma_result result = MA_BUSY) {
        auto& status = source_status[std::string(asset_id)];
        status.storage = storage;
        status.content_hash = hash;
        status.state = state;
        status.result = result;
    }

    void poll_source(const std::string_view asset_id, Voice& voice) {
        if (!voice.resource_source || !voice.source_ready) return;
        const auto result = ma_resource_manager_data_source_result(&voice.source);
        auto& status = source_status[std::string(asset_id)];
        status.storage = voice.storage;
        status.content_hash = voice.content_hash;
        status.result = result;
        status.state = result == MA_SUCCESS ? "ready" : result == MA_BUSY ? "loading" : "error";
        ma_uint64 available{};
        if (ma_resource_manager_data_source_get_available_frames(&voice.source, &available) == MA_SUCCESS) {
            status.available_frames = available;
        }
        ma_uint64 length{};
        if (ma_resource_manager_data_source_get_length_in_pcm_frames(&voice.source, &length) == MA_SUCCESS) {
            status.length_frames = length;
        }
        if (voice.pending_seek && result == MA_SUCCESS) {
            if (ma_sound_seek_to_pcm_frame(&voice.sound, *voice.pending_seek) == MA_SUCCESS) {
                voice.pending_seek.reset();
            }
        }
    }

    void reap_finished_voices() {
        for (auto it = voices.begin(); it != voices.end();) {
            auto& voice = *it->second;
            if (voice.sound_ready && !voice.looping && ma_sound_at_end(&voice.sound)) {
                retired_voice_ids.insert(it->first);
                it = voices.erase(it);
            } else {
                ++it;
            }
        }
    }

    void poll_sources_and_reap() {
        for (auto& [id, voice] : voices) {
            static_cast<void>(id);
            poll_source(voice->asset_id, *voice);
        }
        reap_finished_voices();
    }

    void clear_graph_objects() {
        voices.clear();
        clips.clear();
        // Child groups must be detached before their master group. Keeping a
        // fixed order also makes shutdown deterministic despite unordered maps.
        for (const auto* id : {"audio.ui", "audio.sfx", "audio.music", "audio.master"}) {
            if (const auto found = groups.find(id); found != groups.end()) {
                ma_sound_group_uninit(found->second.get());
            }
        }
        groups.clear();
        retired_voice_ids.clear();
        source_status.clear();
    }
};

MiniaudioRenderGraph::MiniaudioRenderGraph() : impl_(std::make_unique<Impl>()) {}
MiniaudioRenderGraph::~MiniaudioRenderGraph() { shutdown(); }

bool MiniaudioRenderGraph::initialize(const std::uint32_t sample_rate, const std::uint32_t channels) {
    std::unordered_map<std::string, AudioSourceLocation> source_catalog;
    if (impl_) source_catalog = std::move(impl_->source_catalog);
    shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->source_catalog = std::move(source_catalog);
    if (sample_rate == 0U || channels == 0U) {
        last_error_ = "miniaudio graph requires a non-zero sample rate and channel count";
        return false;
    }

    auto resource_config = ma_resource_manager_config_init();
    resource_config.decodedFormat = ma_format_f32;
    resource_config.decodedChannels = 0U;
    resource_config.decodedSampleRate = sample_rate;
    resource_config.jobThreadCount = impl_->resource_job_threads;
    auto result = ma_resource_manager_init(&resource_config, &impl_->resource_manager);
    if (result != MA_SUCCESS) {
        last_error_ = std::string("miniaudio resource manager: ") + ma_result_description(result);
        return false;
    }
    impl_->resource_manager_ready = true;

    auto config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.pResourceManager = &impl_->resource_manager;
    config.sampleRate = sample_rate;
    config.channels = channels;
    config.listenerCount = 1U;
    config.periodSizeInFrames = 800U;
    result = ma_engine_init(&config, &impl_->engine);
    if (result != MA_SUCCESS) {
        last_error_ = std::string("miniaudio engine: ") + ma_result_description(result);
        shutdown();
        return false;
    }
    impl_->engine_ready = true;
    impl_->sample_rate = sample_rate;
    impl_->channels = channels;

    for (const auto* id : kBusIds) {
        auto value = std::make_unique<ma_sound_group>();
        auto* parent = std::string_view(id) == "audio.master" ? nullptr : impl_->group("audio.master");
        const auto group_result = ma_sound_group_init(
            &impl_->engine, MA_SOUND_FLAG_NO_SPATIALIZATION, parent, value.get());
        if (group_result != MA_SUCCESS) {
            last_error_ = std::string("miniaudio sound group '") + id + "': " + ma_result_description(group_result);
            shutdown();
            return false;
        }
        impl_->groups.emplace(id, std::move(value));
    }
    last_error_.clear();
    return true;
}

void MiniaudioRenderGraph::shutdown() {
    if (!impl_) return;
    if (impl_->engine_ready) {
        impl_->clear_graph_objects();
        ma_engine_uninit(&impl_->engine);
        impl_->engine_ready = false;
    }
    if (impl_->resource_manager_ready) {
        ma_resource_manager_uninit(&impl_->resource_manager);
        impl_->resource_manager_ready = false;
    }
}

void MiniaudioRenderGraph::set_source_catalog(std::vector<AudioSourceLocation> locations) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->source_catalog.clear();
    impl_->source_status.clear();
    for (auto& location : locations) {
        if (location.asset_id.empty()) continue;
        impl_->source_catalog.insert_or_assign(location.asset_id, std::move(location));
    }
}

bool MiniaudioRenderGraph::reconcile(const AudioRenderSnapshot& snapshot) {
    if (!impl_ || !impl_->engine_ready || !impl_->resource_manager_ready) {
        last_error_ = "miniaudio graph is not initialized";
        return false;
    }

    impl_->poll_sources_and_reap();
    for (auto retired = impl_->retired_voice_ids.begin(); retired != impl_->retired_voice_ids.end();) {
        const auto authored = std::ranges::find(snapshot.voices, *retired, &AudioVoiceState::id);
        if (authored == snapshot.voices.end() || !authored->playing) retired = impl_->retired_voice_ids.erase(retired);
        else ++retired;
    }
    ma_engine_listener_set_position(
        &impl_->engine, 0U, snapshot.listener_position[0], snapshot.listener_position[1], snapshot.listener_position[2]);
    ma_engine_listener_set_direction(
        &impl_->engine, 0U, snapshot.listener_forward[0], snapshot.listener_forward[1], snapshot.listener_forward[2]);
    ma_engine_listener_set_world_up(
        &impl_->engine, 0U, snapshot.listener_up[0], snapshot.listener_up[1], snapshot.listener_up[2]);

    for (const auto& bus : snapshot.buses) {
        if (const auto found = impl_->groups.find(bus.id); found != impl_->groups.end()) {
            ma_sound_group_set_volume(found->second.get(), effective_gain(bus));
        }
    }

    std::unordered_map<std::string, std::shared_ptr<const AudioClip>> incoming_clips;
    for (const auto& clip : snapshot.clips) {
        if (valid_clip(clip)) incoming_clips.insert_or_assign(clip->asset_id, clip);
    }

    std::unordered_set<std::string> replaced_clips;
    for (const auto& [id, existing] : impl_->clips) {
        const auto incoming = incoming_clips.find(id);
        if (incoming == incoming_clips.end() || incoming->second.get() != existing->source.get()) {
            replaced_clips.insert(id);
        }
    }
    for (const auto& [id, source] : incoming_clips) {
        if (replaced_clips.contains(id)) impl_->clips.erase(id);
        if (impl_->clips.contains(id)) continue;
        auto clip = std::make_shared<Impl::Clip>();
        clip->source = source;
        auto config = ma_audio_buffer_config_init(
            ma_format_f32, source->channels, source->frame_count(), source->samples.data(), nullptr);
        config.sampleRate = source->sample_rate;
        if (ma_audio_buffer_init(&config, &clip->buffer) != MA_SUCCESS) continue;
        clip->ready = true;
        impl_->clips.emplace(id, std::move(clip));
    }

    struct Selection final {
        const AudioSourceLocation* location{};
        std::shared_ptr<Impl::Clip> clip;
        AudioSourceStorage storage{AudioSourceStorage::resident};
        std::string content_hash;
    };
    std::unordered_map<std::string, AudioAssetDescriptor> descriptors;
    for (const auto& asset : snapshot.assets) {
        if (!asset.asset_id.empty()) descriptors.insert_or_assign(asset.asset_id, asset);
    }
    const auto select = [&](const std::string_view asset_id) {
        Selection selection;
        if (const auto source = impl_->source_catalog.find(std::string(asset_id)); source != impl_->source_catalog.end()) {
            selection.location = &source->second;
            selection.storage = source->second.storage;
            selection.content_hash = source->second.content_hash;
            if (const auto descriptor = descriptors.find(std::string(asset_id)); descriptor != descriptors.end()) {
                selection.storage = runtime_storage(descriptor->second.storage);
                selection.content_hash = descriptor->second.content_hash;
            }
        } else if (const auto clip = impl_->clips.find(std::string(asset_id)); clip != impl_->clips.end()) {
            selection.clip = clip->second;
            if (const auto descriptor = descriptors.find(std::string(asset_id)); descriptor != descriptors.end()) {
                selection.storage = runtime_storage(descriptor->second.storage);
                selection.content_hash = descriptor->second.content_hash;
            }
        }
        return selection;
    };

    std::unordered_map<std::uint64_t, ma_uint64> preserved_cursors;
    for (auto it = impl_->voices.begin(); it != impl_->voices.end();) {
        const auto incoming = std::ranges::find(snapshot.voices, it->first, &AudioVoiceState::id);
        if (incoming == snapshot.voices.end() || !incoming->playing) {
            impl_->retired_voice_ids.erase(it->first);
            it = impl_->voices.erase(it);
            continue;
        }

        const auto selection = select(incoming->asset_id);
        const auto& current = *it->second;
        const bool source_changed = current.asset_id != incoming->asset_id ||
            (selection.location != nullptr && (!current.resource_source ||
                current.source_path != selection.location->source_path ||
                current.content_hash != selection.content_hash ||
                current.storage != selection.storage)) ||
            (selection.location == nullptr && current.resource_source) ||
            (selection.location == nullptr && selection.clip && current.clip.get() != selection.clip.get()) ||
            (selection.location == nullptr && !selection.clip && current.resource_source);
        if (source_changed) {
            if (current.asset_id == incoming->asset_id) {
                ma_uint64 cursor{};
                if (ma_sound_get_cursor_in_pcm_frames(&current.sound, &cursor) == MA_SUCCESS) {
                    preserved_cursors.emplace(it->first, cursor);
                }
            }
            it = impl_->voices.erase(it);
        } else {
            ++it;
        }
    }

    impl_->missing_clip_voices = 0U;
    for (const auto& authored : snapshot.voices) {
        if (!authored.playing) continue;
        if (impl_->retired_voice_ids.contains(authored.id) && impl_->voices.find(authored.id) == impl_->voices.end()) {
            // A completed one-shot remains retired until its authored voice is
            // removed. This prevents unrelated snapshot revisions from
            // restarting it.
            continue;
        }

        const auto selection = select(authored.asset_id);
        if (!selection.location && !selection.clip) {
            ++impl_->missing_clip_voices;
            continue;
        }

        auto found = impl_->voices.find(authored.id);
        if (found == impl_->voices.end()) {
            auto voice = std::make_unique<Impl::Voice>();
            voice->asset_id = authored.asset_id;
            voice->bus_id = authored.bus_id;
            voice->looping = authored.looping;
            ma_result init_result = MA_SUCCESS;
            if (selection.location) {
                voice->resource_source = true;
                voice->source_path = selection.location->source_path;
                voice->content_hash = selection.content_hash;
                voice->storage = selection.storage;
                auto source_config = ma_resource_manager_data_source_config_init();
                const auto path = selection.location->source_path.string();
                source_config.pFilePath = path.c_str();
                source_config.flags = storage_flags(selection.storage) |
                    MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC |
                    MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_WAIT_INIT;
                if (authored.looping) source_config.flags |= MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_LOOPING;
                init_result = ma_resource_manager_data_source_init_ex(
                    &impl_->resource_manager, &source_config, &voice->source);
                if (init_result == MA_SUCCESS) voice->source_ready = true;
            } else {
                voice->clip = selection.clip;
                voice->storage = AudioSourceStorage::resident;
            }

            if (init_result == MA_SUCCESS) {
                ma_data_source* data_source = voice->resource_source
                    ? reinterpret_cast<ma_data_source*>(&voice->source)
                    : reinterpret_cast<ma_data_source*>(&voice->clip->buffer);
                init_result = ma_sound_init_from_data_source(
                    &impl_->engine, data_source, 0U, impl_->group(authored.bus_id), &voice->sound);
                if (init_result == MA_SUCCESS) voice->sound_ready = true;
            }
            if (init_result != MA_SUCCESS) {
                if (voice->resource_source) {
                    impl_->set_source_status(authored.asset_id, voice->storage, voice->content_hash, "error", init_result);
                }
                last_error_ = std::string("miniaudio voice '") + std::to_string(authored.id) + ": " +
                    ma_result_description(init_result);
                continue;
            }

            const auto cursor = preserved_cursors.contains(authored.id)
                ? preserved_cursors.at(authored.id)
                : static_cast<ma_uint64>(std::max(0.0, authored.clip_cursor));
            if (cursor > 0U) {
                const auto seek_result = ma_sound_seek_to_pcm_frame(&voice->sound, cursor);
                if (seek_result != MA_SUCCESS && voice->resource_source) voice->pending_seek = cursor;
            }
            if (ma_sound_start(&voice->sound) != MA_SUCCESS) {
                last_error_ = std::string("miniaudio voice '") + std::to_string(authored.id) + " failed to start";
                continue;
            }
            found = impl_->voices.emplace(authored.id, std::move(voice)).first;
        }

        auto& voice = *found->second;
        if (voice.bus_id != authored.bus_id) {
            if (auto* target = impl_->group(authored.bus_id)) {
                static_cast<void>(ma_node_attach_output_bus(
                    reinterpret_cast<ma_node*>(&voice.sound), 0U, reinterpret_cast<ma_node*>(target), 0U));
                voice.bus_id = authored.bus_id;
            }
        }
        voice.looping = authored.looping;
        ma_sound_set_volume(&voice.sound, std::max(0.0F, authored.gain));
        ma_sound_set_pitch(&voice.sound, std::max(0.001F, authored.pitch));
        ma_sound_set_looping(&voice.sound, authored.looping ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(&voice.sound, authored.spatial ? MA_TRUE : MA_FALSE);
        ma_sound_set_position(&voice.sound, authored.position[0], authored.position[1], authored.position[2]);
        ma_sound_set_attenuation_model(&voice.sound, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&voice.sound, std::max(0.001F, authored.minimum_distance));
        ma_sound_set_max_distance(&voice.sound, std::max(authored.minimum_distance, authored.maximum_distance));
        ma_sound_set_rolloff(&voice.sound, std::max(0.0F, authored.rolloff));
        if (voice.resource_source) impl_->poll_source(voice.asset_id, voice);
    }

    for (const auto& asset : snapshot.assets) {
        if (!asset.asset_id.empty() && !impl_->source_status.contains(asset.asset_id)) {
            const auto storage = runtime_storage(asset.storage);
            impl_->set_source_status(asset.asset_id, storage, asset.content_hash, "unrequested");
        }
    }
    for (const auto& [id, location] : impl_->source_catalog) {
        if (!impl_->source_status.contains(id)) {
            impl_->set_source_status(id, location.storage, location.content_hash, "unrequested");
        }
    }
    impl_->revision = snapshot.revision;
    ++impl_->reconciliations;
    last_error_.clear();
    return true;
}

std::size_t MiniaudioRenderGraph::render(const std::span<float> interleaved) {
    if (!impl_ || !impl_->engine_ready || impl_->channels == 0U || interleaved.size() % impl_->channels != 0U) return 0U;
    std::ranges::fill(interleaved, 0.0F);
    impl_->poll_sources_and_reap();
    const auto requested = static_cast<ma_uint64>(interleaved.size() / impl_->channels);
    ma_uint64 rendered{};
    const auto result = ma_engine_read_pcm_frames(&impl_->engine, interleaved.data(), requested, &rendered);
    if (result != MA_SUCCESS && result != MA_AT_END && result != MA_BUSY && result != MA_NO_DATA_AVAILABLE) {
        last_error_ = std::string("miniaudio graph render: ") + ma_result_description(result);
        return 0U;
    }
    impl_->rendered_frames += rendered;
    impl_->poll_sources_and_reap();
    return static_cast<std::size_t>(rendered);
}

std::string MiniaudioRenderGraph::status_json() const {
    using Json = nlohmann::json;
    Json assets = Json::array();
    std::vector<std::string> asset_ids;
    if (impl_) {
        asset_ids.reserve(impl_->source_catalog.size() + impl_->clips.size());
        for (const auto& [id, location] : impl_->source_catalog) {
            static_cast<void>(location);
            asset_ids.push_back(id);
        }
        for (const auto& [id, clip] : impl_->clips) {
            static_cast<void>(clip);
            if (!impl_->source_catalog.contains(id)) asset_ids.push_back(id);
        }
        for (const auto& [id, status] : impl_->source_status) {
            static_cast<void>(status);
            asset_ids.push_back(id);
        }
        std::ranges::sort(asset_ids);
        asset_ids.erase(std::ranges::unique(asset_ids).begin(), asset_ids.end());
        for (const auto& id : asset_ids) {
            const auto source = impl_->source_catalog.find(id);
            const auto status = impl_->source_status.find(id);
            if (source != impl_->source_catalog.end()) {
                const auto state = status == impl_->source_status.end()
                    ? std::string_view{"unrequested"} : std::string_view{status->second.state};
                const auto result = status == impl_->source_status.end() ? MA_BUSY : status->second.result;
                const auto storage = status == impl_->source_status.end() ? source->second.storage : status->second.storage;
                assets.push_back({{"assetId", id}, {"storage", storage_name(storage)},
                    {"state", state}, {"result", result_name(result)},
                    {"availableFrames", status == impl_->source_status.end() ? 0U : status->second.available_frames},
                    {"lengthFrames", status == impl_->source_status.end() ? 0U : status->second.length_frames}});
            } else if (status != impl_->source_status.end()) {
                assets.push_back({{"assetId", id}, {"storage", storage_name(status->second.storage)},
                    {"state", status->second.state}, {"result", result_name(status->second.result)},
                    {"availableFrames", status->second.available_frames}, {"lengthFrames", status->second.length_frames}});
            } else {
                const auto& clip = impl_->clips.at(id)->source;
                assets.push_back({{"assetId", id}, {"storage", "resident"}, {"state", "ready"},
                    {"result", "success"}, {"availableFrames", clip->frame_count()},
                    {"lengthFrames", clip->frame_count()}});
            }
        }
    }

    Json voices = Json::array();
    std::size_t streaming_voices{};
    std::size_t resident_sources{};
    if (impl_) {
        std::vector<std::uint64_t> voice_ids;
        voice_ids.reserve(impl_->voices.size());
        for (const auto& [id, voice] : impl_->voices) {
            static_cast<void>(voice);
            voice_ids.push_back(id);
        }
        std::ranges::sort(voice_ids);
        for (const auto id : voice_ids) {
            const auto& voice = *impl_->voices.at(id);
            ma_uint64 cursor{};
            static_cast<void>(ma_sound_get_cursor_in_pcm_frames(&voice.sound, &cursor));
            if (voice.resource_source && voice.storage == AudioSourceStorage::stream) ++streaming_voices;
            if (voice.resource_source && voice.storage == AudioSourceStorage::resident) ++resident_sources;
            voices.push_back({{"id", id}, {"assetId", voice.asset_id}, {"storage", storage_name(voice.storage)},
                {"playing", ma_sound_is_playing(&voice.sound) == MA_TRUE}, {"cursorFrames", cursor},
                {"atEnd", ma_sound_at_end(&voice.sound) == MA_TRUE},
                {"resourceSource", voice.resource_source}, {"looping", voice.looping}});
        }
    }

    return Json{{"schemaVersion", "noemancer.audio-render-graph/0.2"},
        {"backend", "miniaudio-engine-node-graph"}, {"ready", impl_ && impl_->engine_ready},
        {"sampleRate", impl_ ? impl_->sample_rate : 0U}, {"channels", impl_ ? impl_->channels : 0U},
        {"revision", impl_ ? impl_->revision : 0U}, {"groups", impl_ ? impl_->groups.size() : 0U},
        {"residentClips", impl_ ? impl_->clips.size() : 0U}, {"liveSounds", impl_ ? impl_->voices.size() : 0U},
        {"missingClipVoices", impl_ ? impl_->missing_clip_voices : 0U},
        {"retiredVoices", impl_ ? impl_->retired_voice_ids.size() : 0U},
        {"sourceCatalog", impl_ ? impl_->source_catalog.size() : 0U},
        {"residentSources", resident_sources}, {"streamingVoices", streaming_voices},
        {"reconciliations", impl_ ? impl_->reconciliations : 0U},
        {"renderedFrames", impl_ ? impl_->rendered_frames : 0U},
        {"resourceManager", {{"ready", impl_ && impl_->resource_manager_ready},
            {"decodedFormat", "f32"}, {"decodedSampleRate", impl_ ? impl_->sample_rate : 0U},
            {"jobThreadCount", impl_ ? impl_->resource_job_threads : 0U}}},
        {"assets", std::move(assets)}, {"voices", std::move(voices)}, {"error", last_error_}}.dump();
}

} // namespace noemancer
