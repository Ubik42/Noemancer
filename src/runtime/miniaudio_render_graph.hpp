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

// Runtime-only source resolution. The engine snapshot carries Asset IDs, not
// file paths; this catalog is populated by the host from its Asset Registry
// and never crosses the persistence or Agent ABI boundary.
enum class AudioSourceStorage : std::uint8_t {
    resident,
    stream
};

struct AudioSourceLocation final {
    std::string asset_id;
    std::string source_uri;
    std::string content_hash;
    AudioSourceStorage storage{AudioSourceStorage::resident};
};

// Device-free production mixer built on miniaudio's engine/node graph. The
// gameplay-facing contract stays backend-neutral; all miniaudio objects live in
// the private implementation and are owned by the audio producer thread.
class MiniaudioRenderGraph final {
public:
    explicit MiniaudioRenderGraph(std::shared_ptr<VirtualFileSystem> vfs = {});
    ~MiniaudioRenderGraph();
    MiniaudioRenderGraph(const MiniaudioRenderGraph&) = delete;
    MiniaudioRenderGraph& operator=(const MiniaudioRenderGraph&) = delete;

    [[nodiscard]] bool initialize(std::uint32_t sample_rate, std::uint32_t channels);
    void shutdown();
    void set_source_catalog(std::vector<AudioSourceLocation> locations);
    [[nodiscard]] bool reconcile(const AudioRenderSnapshot& snapshot);
    [[nodiscard]] std::size_t render(std::span<float> interleaved);
    [[nodiscard]] std::string status_json() const;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string last_error_;
};

} // namespace noemancer
