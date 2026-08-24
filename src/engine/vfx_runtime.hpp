#pragma once

#include "engine/gameplay_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noemancer {

struct VfxVector3 final {
    float x{};
    float y{};
    float z{};
};

struct VfxColor final {
    float r{1.0F};
    float g{1.0F};
    float b{1.0F};
    float a{1.0F};
};

struct VfxGraph final {
    std::string id;
    std::string display_name;
    std::uint32_t capacity{1024};
    std::uint32_t burst_count{32};
    float lifetime_min{0.5F};
    float lifetime_max{1.0F};
    float speed_min{1.0F};
    float speed_max{3.0F};
    VfxVector3 gravity{0.0F, -9.81F, 0.0F};
    float drag{0.1F};
    float size_start{0.15F};
    float size_end{};
    VfxColor color_start{1.0F, 0.7F, 0.2F, 1.0F};
    VfxColor color_end{1.0F, 0.1F, 0.0F, 0.0F};
    std::string blend_mode{"additive"};
    // Render-sprite policy is part of the graph authoring contract. The
    // profile defaults preserve ordinary Raster behavior while allowing the
    // active Hybrid Pixel profile to opt into its pixel-grid rules.
    std::string pixel_alignment{"profile"};
    std::string size_quantization{"profile"};
    std::string sampling{"profile"};
};

struct VfxLoadResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string graph_id;
};

class VfxRuntime final {
public:
    struct Particle;
    explicit VfxRuntime(std::size_t global_capacity = 65536);

    [[nodiscard]] VfxLoadResult load_graph_json(std::string_view document);
    [[nodiscard]] bool bind_event(std::string event_type, std::string graph_id);
    void consume_gameplay_events(std::span<const GameplayEvent> events);
    void tick(float delta_seconds);

    [[nodiscard]] std::string graph_json(std::string_view graph_id) const;
    [[nodiscard]] std::string gpu_program_json(std::string_view graph_id) const;
    [[nodiscard]] std::string preview_json(std::string_view graph_id, std::uint64_t seed,
                                           std::uint32_t steps, float fixed_delta_seconds,
                                           std::size_t max_particles = 32) const;
    [[nodiscard]] std::string observe_json(std::size_t max_particles = 32) const;
    [[nodiscard]] std::string benchmark_json(std::string_view graph_id,std::uint32_t particle_count,
                                             std::uint32_t steps,float fixed_delta_seconds) const;
    [[nodiscard]] std::string plan_graph_patch_json(std::string_view graph_id, std::string_view patch_json,
                                                    std::uint64_t base_revision) const;
    [[nodiscard]] std::string apply_graph_plan_json(std::string_view plan_json, bool dry_run);
    [[nodiscard]] std::string undo_graph_json(std::uint64_t expected_revision);
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::span<const Particle> particles() const;
    [[nodiscard]] std::size_t particle_count() const noexcept { return particle_ids_.size(); }
    [[nodiscard]] bool spawn(std::string_view graph_id, VfxVector3 origin, std::uint64_t seed,
                             std::uint64_t source_sequence);

    [[nodiscard]] static std::string default_graph_json();
    [[nodiscard]] static std::string default_alpha_graph_json();

    // Engine-owned plain state. Public for renderer adapters and diagnostics; ownership remains in VfxRuntime.
    struct Particle final {
        std::uint64_t id{};
        std::uint64_t emitter_id{};
        std::string graph_id;
        VfxVector3 position;
        VfxVector3 velocity;
        float age{};
        float lifetime{1.0F};
        float size{};
        VfxColor color;
        std::string blend_mode{"additive"};
        std::string pixel_alignment{"profile"};
        std::string size_quantization{"profile"};
        std::string sampling{"profile"};
        float size_start{0.15F};
        float size_end{};
        VfxColor color_start{1.0F, 0.7F, 0.2F, 1.0F};
        VfxColor color_end{1.0F, 0.1F, 0.0F, 0.0F};
        VfxVector3 gravity{0.0F,-9.81F,0.0F};
        float drag{0.1F};
        std::uint64_t spawn_seed{};
        std::uint32_t spawn_index{};
        VfxVector3 spawn_origin;
        float lifetime_min{0.5F};
        float lifetime_max{1.0F};
        float speed_min{1.0F};
        float speed_max{3.0F};
    };

private:
    void fixed_step(float delta_seconds);

    std::size_t global_capacity_{};
    std::unordered_map<std::string, VfxGraph> graphs_;
    std::unordered_map<std::string, std::string> event_bindings_;
    // Hot simulation state is structure-of-arrays. The AoS view is materialized only at API/render extraction boundaries.
    std::vector<std::uint64_t> particle_ids_;
    std::vector<std::uint64_t> emitter_ids_;
    std::vector<const VfxGraph*> particle_graphs_;
    std::vector<VfxVector3> positions_;
    std::vector<VfxVector3> velocities_;
    std::vector<float> ages_;
    std::vector<float> lifetimes_;
    std::vector<float> sizes_;
    std::vector<VfxColor> colors_;
    std::vector<std::string> blend_modes_;
    std::vector<std::uint64_t> spawn_seeds_;
    std::vector<std::uint32_t> spawn_indices_;
    std::vector<VfxVector3> spawn_origins_;
    mutable std::vector<Particle> particle_view_;
    mutable bool particle_view_dirty_{true};
    std::uint64_t last_event_sequence_{};
    std::uint64_t next_emitter_id_{1};
    std::uint64_t next_particle_id_{1};
    std::uint64_t revision_{1};
    std::uint64_t dropped_particles_{};
    std::uint64_t simulated_steps_{};
    float accumulator_{};
    struct GraphHistoryEntry final {
        std::string graph_id;
        std::string before_json;
        std::string after_json;
    };
    std::vector<GraphHistoryEntry> graph_history_;
};

} // namespace noemancer
