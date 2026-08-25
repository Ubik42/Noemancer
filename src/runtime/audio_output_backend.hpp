#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/gameplay_runtime.hpp"
#include "engine/virtual_file_system.hpp"

namespace noemancer {

struct AudioOutputSource final {
    std::string asset_id;
    std::string source_uri;
    std::string content_hash;
    AudioAssetStorage storage{AudioAssetStorage::resident};
};

// Interactive audio device adapter. The public boundary remains plain PCM;
// miniaudio objects are confined to the private implementation.
class AudioOutputBackend final {
public:
    AudioOutputBackend();
    ~AudioOutputBackend();
    AudioOutputBackend(const AudioOutputBackend&)=delete;
    AudioOutputBackend& operator=(const AudioOutputBackend&)=delete;

    [[nodiscard]] bool initialize(std::uint32_t sample_rate=48000,std::uint32_t channels=2,
                                  std::vector<AudioOutputSource> sources={},
                                  std::shared_ptr<VirtualFileSystem> vfs={});
    void shutdown();
    [[nodiscard]] std::size_t queued_frames() const;
    [[nodiscard]] std::size_t target_buffer_frames() const;
    // Publishes authored audio state to a dedicated real-time producer. Audio
    // resources are shared immutable data; gameplay World ownership never crosses
    // the worker boundary.
    void publish(AudioRenderSnapshot snapshot);
    [[nodiscard]] std::string status_json() const;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] bool submit(std::span<const float> interleaved);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string last_error_;
};

} // namespace noemancer
