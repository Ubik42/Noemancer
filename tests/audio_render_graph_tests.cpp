#include "runtime/miniaudio_render_graph.hpp"
#include "engine/process_diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// MSVC's Debug assert opens a modal CRT dialog and blocks unattended CTest.
// Keep each behavioral check, but report it as a normal non-zero test result.
#undef assert
#define assert(condition) do { if (!(condition)) { \
    std::cerr << "audio_render_graph check failed at line " << __LINE__ << ": " #condition "\n"; \
    return __LINE__; \
} } while (false)

namespace {

void append_text(std::vector<std::byte>& out, const char* value) {
    for (int index = 0; index < 4; ++index) out.push_back(static_cast<std::byte>(value[index]));
}

void append_u16(std::vector<std::byte>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (int index = 0; index < 4; ++index) out.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xffU));
}

std::filesystem::path write_test_wav() {
    constexpr std::uint32_t sample_rate = 48000U;
    constexpr std::uint32_t frames = sample_rate * 2U;
    std::vector<std::byte> bytes;
    bytes.reserve(44U + frames * 2U);
    append_text(bytes, "RIFF");
    append_u32(bytes, 36U + frames * 2U);
    append_text(bytes, "WAVE");
    append_text(bytes, "fmt ");
    append_u32(bytes, 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, 1U);
    append_u32(bytes, sample_rate);
    append_u32(bytes, sample_rate * 2U);
    append_u16(bytes, 2U);
    append_u16(bytes, 16U);
    append_text(bytes, "data");
    append_u32(bytes, frames * 2U);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto sample = static_cast<std::int16_t>(12000.0 * std::sin(
            6.283185307179586 * 440.0 * static_cast<double>(frame) / sample_rate));
        append_u16(bytes, static_cast<std::uint16_t>(sample));
    }
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("noemancer-audio-" + std::to_string(unique) + ".wav");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return path;
}

const nlohmann::json* find_asset(const nlohmann::json& status, const std::string_view asset_id) {
    for (const auto& asset : status.at("assets")) {
        if (asset.at("assetId") == std::string(asset_id)) return &asset;
    }
    return nullptr;
}

const nlohmann::json* find_voice(const nlohmann::json& status, const std::uint64_t voice_id) {
    for (const auto& voice : status.at("voices")) {
        if (voice.at("id") == voice_id) return &voice;
    }
    return nullptr;
}

} // namespace

int main() {
    using namespace noemancer;
    configure_process_diagnostics("test.audio-render-graph");
    const auto source_path = write_test_wav();
    auto vfs=std::make_shared<VirtualFileSystem>();
    assert(vfs->mount({.id="test.audio",.virtual_root="asset://test",.source_root=source_path.parent_path()}).success);
    MiniaudioRenderGraph graph(vfs);
    assert(graph.initialize(48000U, 2U));

    auto clip = std::make_shared<AudioClip>();
    clip->asset_id = "test.tone";
    clip->sample_rate = 48000U;
    clip->channels = 1U;
    clip->samples.resize(4800U);
    for (std::size_t index = 0; index < clip->samples.size(); ++index) {
        clip->samples[index] = 0.2F * std::sin(6.283185307F * 440.0F * static_cast<float>(index) / 48000.0F);
    }

    AudioRenderSnapshot snapshot;
    snapshot.revision = 7U;
    snapshot.buses = {{"audio.master", "", 1.0F, false}, {"audio.music", "audio.master", 1.0F, false},
        {"audio.sfx", "audio.master", 1.0F, false}, {"audio.ui", "audio.master", 1.0F, false}};
    snapshot.clips.push_back(clip);
    snapshot.voices.push_back({.id=42U, .asset_id="test.tone", .bus_id="audio.sfx", .gain=1.0F,
        .pitch=1.0F, .looping=true, .playing=true});
    assert(graph.reconcile(snapshot));

    std::vector<float> first(800U * 2U);
    std::vector<float> second(800U * 2U);
    assert(graph.render(first) == 800U);
    snapshot.revision = 8U;
    snapshot.buses[2].gain = 0.5F;
    assert(graph.reconcile(snapshot));
    assert(graph.render(second) == 800U);
    assert(std::ranges::any_of(first, [](const float sample) { return std::abs(sample) > 0.001F; }));
    assert(std::ranges::any_of(second, [](const float sample) { return std::abs(sample) > 0.001F; }));

    auto status = nlohmann::json::parse(graph.status_json());
    assert(status["backend"] == "miniaudio-engine-node-graph");
    assert(status["groups"] == 4U);
    assert(status["residentClips"] == 1U);
    assert(status["liveSounds"] == 1U);
    assert(status["missingClipVoices"] == 0U);
    assert(status["reconciliations"] == 2U);

    snapshot.revision = 9U;
    snapshot.voices.push_back({.id=99U, .asset_id="missing.asset", .bus_id="audio.sfx", .playing=true});
    assert(graph.reconcile(snapshot));
    status = nlohmann::json::parse(graph.status_json());
    assert(status["liveSounds"] == 1U);
    assert(status["missingClipVoices"] == 1U);

    const auto source_uri="asset://test/"+source_path.filename().generic_string();
    graph.set_source_catalog({
        {"asset.audio.resident", source_uri, "sha256:resident", AudioSourceStorage::resident},
        {"asset.audio.stream", source_uri, "sha256:stream", AudioSourceStorage::stream}
    });
    snapshot.revision = 10U;
    snapshot.clips.clear();
    snapshot.assets = {
        {"asset.audio.resident", "sha256:resident", AudioAssetStorage::resident},
        {"asset.audio.stream", "sha256:stream", AudioAssetStorage::stream}
    };
    snapshot.voices = {
        {.id=100U, .asset_id="asset.audio.resident", .bus_id="audio.sfx", .gain=1.0F,
            .pitch=1.0F, .looping=true, .playing=true},
        {.id=101U, .asset_id="asset.audio.stream", .bus_id="audio.music", .gain=0.8F,
            .pitch=1.0F, .looping=true, .playing=true}
    };
    assert(graph.reconcile(snapshot));

    bool resident_ready = false;
    bool stream_ready = false;
    bool signal_seen = false;
    for (int iteration = 0; iteration < 200; ++iteration) {
        std::vector<float> rendered(800U * 2U);
        assert(graph.render(rendered) == 800U);
        signal_seen = signal_seen || std::ranges::any_of(rendered, [](const float sample) {
            return std::abs(sample) > 0.001F;
        });
        status = nlohmann::json::parse(graph.status_json());
        if (const auto* asset = find_asset(status, "asset.audio.resident")) resident_ready = asset->at("state") == "ready";
        if (const auto* asset = find_asset(status, "asset.audio.stream")) stream_ready = asset->at("state") == "ready";
        if (resident_ready && stream_ready && signal_seen) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    status = nlohmann::json::parse(graph.status_json());
    assert(status["resourceManager"]["ready"] == true);
    assert(status["sourceCatalog"] == 2U);
    assert(status["residentSources"] == 1U);
    assert(status["streamingVoices"] == 1U);
    assert(resident_ready && stream_ready && signal_seen);
    assert(find_asset(status, "asset.audio.resident")->at("storage") == "resident");
    assert(find_asset(status, "asset.audio.stream")->at("storage") == "stream");
    assert(status.dump().find(source_path.generic_string()) == std::string::npos);
    const auto first_cursor = find_voice(status, 100U)->at("cursorFrames").get<std::uint64_t>();
    assert(first_cursor > 0U);

    // Moving a voice between groups must preserve its real playback node and
    // cursor rather than tearing down the Resource Manager data source.
    snapshot.revision = 11U;
    snapshot.voices[0].bus_id = "audio.ui";
    assert(graph.reconcile(snapshot));
    std::vector<float> moved(800U * 2U);
    assert(graph.render(moved) == 800U);
    status = nlohmann::json::parse(graph.status_json());
    assert(find_voice(status, 100U)->at("cursorFrames").get<std::uint64_t>() > first_cursor);

    // A completed one-shot is retired and must not be resurrected by an
    // unrelated authored revision while its logical voice remains present.
    snapshot.revision = 12U;
    snapshot.voices.push_back({.id=102U, .asset_id="asset.audio.resident", .bus_id="audio.sfx", .gain=1.0F,
        .pitch=8.0F, .looping=false, .playing=true});
    assert(graph.reconcile(snapshot));
    for (int iteration = 0; iteration < 80; ++iteration) {
        std::vector<float> rendered(800U * 2U);
        static_cast<void>(graph.render(rendered));
        status = nlohmann::json::parse(graph.status_json());
        if (status["retiredVoices"].get<std::size_t>() > 0U) break;
    }
    status = nlohmann::json::parse(graph.status_json());
    assert(status["retiredVoices"] > 0U);
    snapshot.revision = 13U;
    assert(graph.reconcile(snapshot));
    status = nlohmann::json::parse(graph.status_json());
    assert(status["retiredVoices"] > 0U);
    assert(find_voice(status, 102U) == nullptr);

    snapshot.voices.pop_back();
    snapshot.revision = 14U;
    assert(graph.reconcile(snapshot));
    status = nlohmann::json::parse(graph.status_json());
    assert(status["retiredVoices"] == 0U);
    graph.shutdown();
    std::error_code cleanup_error;
    std::filesystem::remove(source_path, cleanup_error);
}
