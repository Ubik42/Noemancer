#include "runtime/audio_output_backend.hpp"
#include "runtime/miniaudio_render_graph.hpp"

#include <miniaudio.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace noemancer {

struct AudioOutputBackend::Impl final {
    ma_device device{};
    ma_pcm_rb ring{};
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    std::vector<AudioOutputSource> sources;
    std::size_t target_frames{4800};
    bool ring_ready{};
    bool device_ready{};
    std::atomic<std::uint64_t> submitted_frames{};
    std::uint64_t primed_frames{};
    std::atomic<std::uint64_t> consumed_frames{};
    std::atomic<std::uint64_t> underrun_frames{};
    std::atomic<std::uint64_t> produced_frames{};
    std::atomic<std::uint64_t> published_snapshots{};
    std::atomic<std::uint64_t> applied_snapshots{};
    std::mutex snapshot_mutex;
    std::condition_variable_any wake;
    AudioRenderSnapshot pending_snapshot;
    std::uint64_t pending_generation{};
    std::string render_graph_status{"{}"};
    std::string producer_error;
    std::atomic<bool> graph_ready{};
    std::jthread producer;
    static void callback(ma_device* device,void* output,const void*,ma_uint32 frame_count);
    void produce(std::stop_token stop);
};

void AudioOutputBackend::Impl::callback(ma_device* device,void* output,const void*,const ma_uint32 frame_count) {
    auto& state=*static_cast<Impl*>(device->pUserData);
    auto* destination=static_cast<float*>(output);
    std::fill_n(destination,static_cast<std::size_t>(frame_count)*state.channels,0.0F);
    ma_uint32 remaining=frame_count;
    while(remaining>0) {
        ma_uint32 available=remaining;void* source=nullptr;
        if(ma_pcm_rb_acquire_read(&state.ring,&available,&source)!=MA_SUCCESS||available==0)break;
        const auto consumed=frame_count-remaining;
        std::memcpy(destination+static_cast<std::size_t>(consumed)*state.channels,source,
            static_cast<std::size_t>(available)*state.channels*sizeof(float));
        ma_pcm_rb_commit_read(&state.ring,available);remaining-=available;
    }
    state.consumed_frames.fetch_add(frame_count-remaining,std::memory_order_relaxed);
    state.underrun_frames.fetch_add(remaining,std::memory_order_relaxed);
}

void AudioOutputBackend::Impl::produce(const std::stop_token stop) {
    MiniaudioRenderGraph render_graph;
    std::vector<AudioSourceLocation> source_catalog;
    source_catalog.reserve(sources.size());
    for (const auto& source : sources) source_catalog.push_back({
        source.asset_id, source.source_path, source.content_hash,
        source.storage == AudioAssetStorage::stream ? AudioSourceStorage::stream : AudioSourceStorage::resident});
    render_graph.set_source_catalog(std::move(source_catalog));
    if (!render_graph.initialize(sample_rate, channels)) {
        std::lock_guard lock(snapshot_mutex);
        producer_error=render_graph.last_error();
        render_graph_status=render_graph.status_json();
        return;
    }
    graph_ready.store(true,std::memory_order_release);
    {
        std::lock_guard lock(snapshot_mutex);
        render_graph_status=render_graph.status_json();
    }
    std::uint64_t applied_generation{};
    std::vector<float> pcm(800U*channels);
    while(!stop.stop_requested()) {
        AudioRenderSnapshot next_snapshot;
        bool has_snapshot_update=false;
        {
            std::lock_guard lock(snapshot_mutex);
            if(pending_generation!=applied_generation) {
                next_snapshot=pending_snapshot;
                applied_generation=pending_generation;
                has_snapshot_update=true;
            }
        }
        // Resource Manager initialization and graph reconciliation may wait on
        // decoder setup or a node detach. Never hold the publication mutex
        // across that work: publishing the latest immutable snapshot and
        // querying status must remain independent of the audio producer.
        if(has_snapshot_update) {
            if(!render_graph.reconcile(next_snapshot)) producer_error=render_graph.last_error();
            {
                std::lock_guard lock(snapshot_mutex);
                render_graph_status=render_graph.status_json();
            }
            applied_snapshots.fetch_add(1U,std::memory_order_relaxed);
        }
        if(ma_pcm_rb_available_read(&ring)<target_frames) {
            std::ranges::fill(pcm, 0.0F);
            const auto rendered=render_graph.render(pcm);
            if(rendered<800U) {
                std::ranges::fill(pcm.begin()+static_cast<std::ptrdiff_t>(rendered*channels),pcm.end(),0.0F);
            }
            ma_uint32 remaining=800U,written_total=0U;
            while(remaining>0U&&!stop.stop_requested()) {
                ma_uint32 writable=remaining;void* destination=nullptr;
                if(ma_pcm_rb_acquire_write(&ring,&writable,&destination)!=MA_SUCCESS||writable==0U)break;
                std::memcpy(destination,pcm.data()+static_cast<std::size_t>(written_total)*channels,
                    static_cast<std::size_t>(writable)*channels*sizeof(float));
                ma_pcm_rb_commit_write(&ring,writable);remaining-=writable;written_total+=writable;
            }
            produced_frames.fetch_add(written_total,std::memory_order_relaxed);
            if(written_total>0U)continue;
        }
        std::unique_lock lock(snapshot_mutex);
        wake.wait_for(lock,stop,std::chrono::milliseconds(3),[&]{return pending_generation!=applied_generation;});
    }
    graph_ready.store(false,std::memory_order_release);
}

AudioOutputBackend::AudioOutputBackend():impl_(std::make_unique<Impl>()){}
AudioOutputBackend::~AudioOutputBackend(){shutdown();}

bool AudioOutputBackend::initialize(const std::uint32_t sample_rate,const std::uint32_t channels,
                                    std::vector<AudioOutputSource> sources) {
    shutdown();impl_=std::make_unique<Impl>();impl_->sample_rate=sample_rate;impl_->channels=channels;
    impl_->sources=std::move(sources);
    impl_->target_frames=std::max<std::size_t>(sample_rate/10U,512U);
    if(ma_pcm_rb_init(ma_format_f32,channels,static_cast<ma_uint32>(impl_->target_frames*2U),nullptr,nullptr,&impl_->ring)!=MA_SUCCESS) {
        last_error_="miniaudio ring buffer initialization failed";return false;
    }
    impl_->ring_ready=true;
    std::vector<float> silence(impl_->target_frames*channels,0.0F);
    if(!submit(silence)){last_error_="miniaudio ring buffer priming failed";shutdown();return false;}
    impl_->primed_frames=impl_->target_frames;impl_->submitted_frames.store(0,std::memory_order_relaxed);
    auto config=ma_device_config_init(ma_device_type_playback);
    config.playback.format=ma_format_f32;config.playback.channels=channels;config.sampleRate=sample_rate;
    config.dataCallback=Impl::callback;config.pUserData=impl_.get();
    const auto initialized=ma_device_init(nullptr,&config,&impl_->device);
    if(initialized!=MA_SUCCESS){last_error_=std::string("miniaudio device: ")+ma_result_description(initialized);shutdown();return false;}
    impl_->device_ready=true;
    impl_->producer=std::jthread([state=impl_.get()](const std::stop_token stop) {
        try {
            state->produce(stop);
        } catch (const std::exception& error) {
            std::lock_guard lock(state->snapshot_mutex);
            state->producer_error=std::string{"Audio producer exception: "}+error.what();
            state->graph_ready.store(false,std::memory_order_release);
        } catch (...) {
            std::lock_guard lock(state->snapshot_mutex);
            state->producer_error="Audio producer threw a non-standard exception.";
            state->graph_ready.store(false,std::memory_order_release);
        }
    });
    const auto started=ma_device_start(&impl_->device);
    if(started!=MA_SUCCESS){last_error_=std::string("miniaudio start: ")+ma_result_description(started);shutdown();return false;}
    last_error_.clear();return true;
}

void AudioOutputBackend::shutdown() {
    if(!impl_)return;
    if(impl_->device_ready){ma_device_uninit(&impl_->device);impl_->device_ready=false;}
    if(impl_->producer.joinable()){impl_->producer.request_stop();impl_->wake.notify_all();impl_->producer.join();}
    if(impl_->ring_ready){ma_pcm_rb_uninit(&impl_->ring);impl_->ring_ready=false;}
}

std::size_t AudioOutputBackend::queued_frames() const {return impl_&&impl_->ring_ready?ma_pcm_rb_available_read(&impl_->ring):0U;}
std::size_t AudioOutputBackend::target_buffer_frames() const {return impl_?impl_->target_frames:0U;}

bool AudioOutputBackend::submit(const std::span<const float> interleaved) {
    if(!impl_||!impl_->ring_ready||impl_->channels==0||interleaved.size()%impl_->channels!=0)return false;
    ma_uint32 remaining=static_cast<ma_uint32>(interleaved.size()/impl_->channels);ma_uint32 written_total=0;
    while(remaining>0) {
        ma_uint32 writable=remaining;void* destination=nullptr;
        if(ma_pcm_rb_acquire_write(&impl_->ring,&writable,&destination)!=MA_SUCCESS||writable==0)break;
        std::memcpy(destination,interleaved.data()+static_cast<std::size_t>(written_total)*impl_->channels,
            static_cast<std::size_t>(writable)*impl_->channels*sizeof(float));
        ma_pcm_rb_commit_write(&impl_->ring,writable);remaining-=writable;written_total+=writable;
    }
    impl_->submitted_frames.fetch_add(written_total,std::memory_order_relaxed);return remaining==0;
}

void AudioOutputBackend::publish(AudioRenderSnapshot snapshot) {
    if(!impl_)return;
    {
        std::lock_guard lock(impl_->snapshot_mutex);
        impl_->pending_snapshot=std::move(snapshot);++impl_->pending_generation;
    }
    impl_->published_snapshots.fetch_add(1U,std::memory_order_relaxed);
    impl_->wake.notify_one();
}

std::string AudioOutputBackend::status_json() const {
    using Json=nlohmann::json;
    Json graph=Json::object();
    std::string producer_error;
    if(impl_) {
        std::lock_guard lock(impl_->snapshot_mutex);
        graph=Json::parse(impl_->render_graph_status,nullptr,false);
        if(graph.is_discarded())graph=Json::object();
        producer_error=impl_->producer_error;
    }
    return Json{{"schemaVersion","noemancer.audio-output/0.3"},{"backend","miniaudio/0.11.25"},
        {"ready",impl_&&impl_->device_ready&&impl_->graph_ready.load(std::memory_order_acquire)},
        {"sourceCatalog",impl_?impl_->sources.size():0U},
        {"sampleRate",impl_?impl_->sample_rate:0U},{"channels",impl_?impl_->channels:0U},
        {"producer",{{"mode","dedicated-node-graph-thread"},{"renderGraph","miniaudio-engine-node-graph"},{"quantumFrames",800U},
            {"producedFrames",impl_?impl_->produced_frames.load():0U},{"publishedSnapshots",impl_?impl_->published_snapshots.load():0U},
            {"appliedSnapshots",impl_?impl_->applied_snapshots.load():0U},{"graph",std::move(graph)},{"error",producer_error}}},
        {"queuedFrames",queued_frames()},{"targetBufferFrames",target_buffer_frames()},
        {"primedFrames",impl_?impl_->primed_frames:0U},
        {"submittedFrames",impl_?impl_->submitted_frames.load():0U},{"consumedFrames",impl_?impl_->consumed_frames.load():0U},
        {"underrunFrames",impl_?impl_->underrun_frames.load():0U},{"error",last_error_}}.dump();
}

} // namespace noemancer
