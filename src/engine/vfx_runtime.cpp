#include "engine/vfx_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr float fixed_step_seconds = 1.0F / 60.0F;

std::uint32_t particle_random_u32(const std::uint64_t seed,const std::uint32_t particle_index,const std::uint32_t channel) {
    auto value=static_cast<std::uint32_t>(seed)^std::rotl(static_cast<std::uint32_t>(seed>>32U),16)^
        (particle_index*0x9e3779b9U)^(channel*0x85ebca6bU);
    value^=value>>16U; value*=0x7feb352dU; value^=value>>15U; value*=0x846ca68bU; value^=value>>16U;
    return value;
}

float particle_random_range(const std::uint64_t seed,const std::uint32_t particle_index,const std::uint32_t channel,
                            const float minimum,const float maximum) {
    const auto unit=static_cast<float>(particle_random_u32(seed,particle_index,channel)>>8U)*(1.0F/16777216.0F);
    return minimum+(maximum-minimum)*unit;
}

Json vector_json(const VfxVector3& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

Json color_json(const VfxColor& value) {
    return {{"r", value.r}, {"g", value.g}, {"b", value.b}, {"a", value.a}};
}

Json sprite_policy_json(const VfxGraph& graph) {
    return {{"pixelAlignment", graph.pixel_alignment}, {"sizeQuantization", graph.size_quantization},
            {"sampling", graph.sampling}};
}

VfxVector3 parse_vector(const Json& value, const VfxVector3 fallback = {}) {
    if (!value.is_object()) return fallback;
    return {value.value("x", fallback.x), value.value("y", fallback.y), value.value("z", fallback.z)};
}

VfxColor parse_color(const Json& value, const VfxColor fallback = {}) {
    if (!value.is_object()) return fallback;
    return {value.value("r", fallback.r), value.value("g", fallback.g),
            value.value("b", fallback.b), value.value("a", fallback.a)};
}

Json graph_to_json(const VfxGraph& graph) {
    return {
        {"schemaVersion", "noemancer.vfx-graph/0.1"},
        {"graphId", graph.id},
        {"displayName", graph.display_name},
        {"execution", Json{{"preferred", "gpu-compute"}, {"reference", "cpu-deterministic"}}},
        {"capacity", graph.capacity},
        {"nodes", Json::array({
            {{"id", "spawn.burst"}, {"op", "spawn.burst"}, {"parameters", {{"count", graph.burst_count}}}},
            {{"id", "initialize"}, {"op", "particle.initialize"}, {"parameters", {
                {"lifetime", {{"min", graph.lifetime_min}, {"max", graph.lifetime_max}}},
                {"speed", {{"min", graph.speed_min}, {"max", graph.speed_max}}},
                {"size", {{"start", graph.size_start}, {"end", graph.size_end}}},
                {"color", {{"start", color_json(graph.color_start)}, {"end", color_json(graph.color_end)}}}
            }}},
            {{"id", "forces"}, {"op", "particle.forces"}, {"parameters", {
                {"gravity", vector_json(graph.gravity)}, {"drag", graph.drag}}}},
            {{"id", "output"}, {"op", "render.sprite"}, {"parameters", {
                {"blendMode", graph.blend_mode}, {"pixelAlignment", graph.pixel_alignment},
                {"sizeQuantization", graph.size_quantization}, {"sampling", graph.sampling}}}}
        })},
        {"edges", Json::array({
            {{"from", "spawn.burst"}, {"to", "initialize"}},
            {{"from", "initialize"}, {"to", "forces"}},
            {{"from", "forces"}, {"to", "output"}}
        })}
    };
}

void hash_u32(std::uint64_t& hash, const std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= 1099511628211ULL;
    }
}

std::string particle_digest(const std::vector<VfxRuntime::Particle>& particles) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& particle : particles) {
        hash_u32(hash, static_cast<std::uint32_t>(particle.id));
        hash_u32(hash, std::bit_cast<std::uint32_t>(particle.position.x));
        hash_u32(hash, std::bit_cast<std::uint32_t>(particle.position.y));
        hash_u32(hash, std::bit_cast<std::uint32_t>(particle.position.z));
        hash_u32(hash, std::bit_cast<std::uint32_t>(particle.age));
        hash_u32(hash, std::bit_cast<std::uint32_t>(particle.size));
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string text_digest(const std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string pointer_token(const std::string_view token) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const char character : token) {
        if (character == '~') escaped += "~0";
        else if (character == '/') escaped += "~1";
        else escaped += character;
    }
    return escaped;
}

void collect_changed_paths(const Json& before, const Json& after, const std::string& path, Json& changed) {
    if (before.type() != after.type()) {
        changed.push_back(path.empty() ? "/" : path);
        return;
    }
    if (before.is_object()) {
        std::vector<std::string> keys;
        for (const auto& [key, unused] : before.items()) { static_cast<void>(unused); keys.push_back(key); }
        for (const auto& [key, unused] : after.items()) {
            static_cast<void>(unused);
            if (!before.contains(key)) keys.push_back(key);
        }
        std::ranges::sort(keys);
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        for (const auto& key : keys) {
            const auto child_path = path + "/" + pointer_token(key);
            if (!before.contains(key) || !after.contains(key)) changed.push_back(child_path);
            else collect_changed_paths(before.at(key), after.at(key), child_path, changed);
        }
        return;
    }
    if (before.is_array()) {
        if (before != after) changed.push_back(path.empty() ? "/" : path);
        return;
    }
    if (before != after) changed.push_back(path.empty() ? "/" : path);
}

Json particle_json(const VfxRuntime::Particle& particle) {
    return {{"id", particle.id}, {"emitterId", particle.emitter_id}, {"graphId", particle.graph_id},
            {"position", vector_json(particle.position)}, {"velocity", vector_json(particle.velocity)},
            {"age", particle.age}, {"lifetime", particle.lifetime}, {"size", particle.size},
            {"color", color_json(particle.color)}, {"blendMode",particle.blend_mode},
            {"spritePolicy",{{"pixelAlignment",particle.pixel_alignment},
                {"sizeQuantization",particle.size_quantization},{"sampling",particle.sampling}}},
            {"spawnProvenance",{{"seed",particle.spawn_seed},{"particleIndex",particle.spawn_index},
                {"origin",vector_json(particle.spawn_origin)},
                {"lifetimeRange",{particle.lifetime_min,particle.lifetime_max}},
                {"speedRange",{particle.speed_min,particle.speed_max}}}}};
}

} // namespace

VfxRuntime::VfxRuntime(const std::size_t global_capacity)
    : global_capacity_(std::max<std::size_t>(global_capacity, 1U)) {
    const auto reserve_count=std::min<std::size_t>(global_capacity_,65536U);
    particle_ids_.reserve(reserve_count); emitter_ids_.reserve(reserve_count); particle_graphs_.reserve(reserve_count);
    positions_.reserve(reserve_count); velocities_.reserve(reserve_count); ages_.reserve(reserve_count);
    lifetimes_.reserve(reserve_count); sizes_.reserve(reserve_count); colors_.reserve(reserve_count); blend_modes_.reserve(reserve_count);
    spawn_seeds_.reserve(reserve_count); spawn_indices_.reserve(reserve_count); spawn_origins_.reserve(reserve_count);
}

std::span<const VfxRuntime::Particle> VfxRuntime::particles() const {
    if(!particle_view_dirty_) return particle_view_;
    particle_view_.clear(); particle_view_.reserve(particle_ids_.size());
    for(std::size_t index=0;index<particle_ids_.size();++index) particle_view_.push_back({
        particle_ids_[index],emitter_ids_[index],particle_graphs_[index]->id,positions_[index],velocities_[index],
        ages_[index],lifetimes_[index],sizes_[index],colors_[index],blend_modes_[index],
        particle_graphs_[index]->pixel_alignment,particle_graphs_[index]->size_quantization,
        particle_graphs_[index]->sampling,
        particle_graphs_[index]->size_start,particle_graphs_[index]->size_end,
        particle_graphs_[index]->color_start,particle_graphs_[index]->color_end,
        particle_graphs_[index]->gravity,particle_graphs_[index]->drag,spawn_seeds_[index],spawn_indices_[index],spawn_origins_[index],
        particle_graphs_[index]->lifetime_min,particle_graphs_[index]->lifetime_max,
        particle_graphs_[index]->speed_min,particle_graphs_[index]->speed_max});
    particle_view_dirty_=false;
    return particle_view_;
}

VfxLoadResult VfxRuntime::load_graph_json(const std::string_view document) {
    try {
        const auto root = Json::parse(document);
        if (!root.is_object()) return {false, "vfx.graph.invalid-document", "Root must be an object", {}};
        if (root.value("schemaVersion", std::string{}) != "noemancer.vfx-graph/0.1")
            return {false, "vfx.graph.unsupported-schema", "Expected noemancer.vfx-graph/0.1", {}};
        VfxGraph graph;
        graph.id = root.value("graphId", std::string{});
        graph.display_name = root.value("displayName", graph.id);
        graph.capacity = root.value("capacity", 1024U);
        if (graph.id.empty()) return {false, "vfx.graph.missing-id", "graphId must not be empty", {}};
        if (graph.capacity == 0 || graph.capacity > global_capacity_)
            return {false, "vfx.graph.invalid-capacity", "capacity exceeds the runtime pool budget", graph.id};
        const auto nodes = root.value("nodes", Json::array());
        if (!nodes.is_array()) return {false, "vfx.graph.invalid-nodes", "nodes must be an array", graph.id};
        bool has_spawn = false;
        bool has_output = false;
        for (const auto& node : nodes) {
            const auto op = node.value("op", std::string{});
            const auto parameters = node.value("parameters", Json::object());
            if (op == "spawn.burst") {
                has_spawn = true;
                graph.burst_count = parameters.value("count", graph.burst_count);
            } else if (op == "particle.initialize") {
                const auto lifetime = parameters.value("lifetime", Json::object());
                const auto speed = parameters.value("speed", Json::object());
                const auto size = parameters.value("size", Json::object());
                const auto color = parameters.value("color", Json::object());
                graph.lifetime_min = lifetime.value("min", graph.lifetime_min);
                graph.lifetime_max = lifetime.value("max", graph.lifetime_max);
                graph.speed_min = speed.value("min", graph.speed_min);
                graph.speed_max = speed.value("max", graph.speed_max);
                graph.size_start = size.value("start", graph.size_start);
                graph.size_end = size.value("end", graph.size_end);
                graph.color_start = parse_color(color.value("start", Json::object()), graph.color_start);
                graph.color_end = parse_color(color.value("end", Json::object()), graph.color_end);
            } else if (op == "particle.forces") {
                graph.gravity = parse_vector(parameters.value("gravity", Json::object()), graph.gravity);
                graph.drag = parameters.value("drag", graph.drag);
            } else if (op == "render.sprite") {
                has_output = true;
                graph.blend_mode = parameters.value("blendMode", graph.blend_mode);
                if (parameters.contains("pixelAlignment")) {
                    if (!parameters.at("pixelAlignment").is_string())
                        return {false, "vfx.graph.invalid-parameter",
                            "render.sprite pixelAlignment must be profile or none", graph.id};
                    graph.pixel_alignment = parameters.at("pixelAlignment").get<std::string>();
                }
                if (parameters.contains("sizeQuantization")) {
                    if (!parameters.at("sizeQuantization").is_string())
                        return {false, "vfx.graph.invalid-parameter",
                            "render.sprite sizeQuantization must be profile or none", graph.id};
                    graph.size_quantization = parameters.at("sizeQuantization").get<std::string>();
                }
                if (parameters.contains("sampling")) {
                    if (!parameters.at("sampling").is_string())
                        return {false, "vfx.graph.invalid-parameter",
                            "render.sprite sampling must be profile or linear", graph.id};
                    graph.sampling = parameters.at("sampling").get<std::string>();
                }
            }
        }
        if (!has_spawn || !has_output)
            return {false, "vfx.graph.missing-required-node", "spawn.burst and render.sprite are required", graph.id};
        const bool invalid_ranges = graph.burst_count == 0 || graph.burst_count > graph.capacity ||
            graph.lifetime_min <= 0.0F || graph.lifetime_max < graph.lifetime_min ||
            graph.speed_min < 0.0F || graph.speed_max < graph.speed_min || graph.drag < 0.0F;
        const bool invalid_sprite_policy =
            (graph.pixel_alignment != "profile" && graph.pixel_alignment != "none") ||
            (graph.size_quantization != "profile" && graph.size_quantization != "none") ||
            (graph.sampling != "profile" && graph.sampling != "linear");
        if (invalid_ranges || (graph.blend_mode != "additive" && graph.blend_mode != "alpha" && graph.blend_mode != "cutout") ||
            invalid_sprite_policy)
            return {false, "vfx.graph.invalid-parameter", "Graph parameters are outside supported bounds", graph.id};
        const auto id = graph.id;
        graphs_.insert_or_assign(id, std::move(graph));
        ++revision_;
        return {true, "ok", "Graph loaded", id};
    } catch (const std::exception& error) {
        return {false, "vfx.graph.parse-error", error.what(), {}};
    }
}

bool VfxRuntime::bind_event(std::string event_type, std::string graph_id) {
    if (event_type.empty() || !graphs_.contains(graph_id)) return false;
    event_bindings_.insert_or_assign(std::move(event_type), std::move(graph_id));
    ++revision_;
    return true;
}

void VfxRuntime::consume_gameplay_events(const std::span<const GameplayEvent> events) {
    for (const auto& event : events) {
        if (event.sequence <= last_event_sequence_) continue;
        last_event_sequence_ = event.sequence;
        const auto binding = event_bindings_.find(event.type);
        if (binding == event_bindings_.end()) continue;
        VfxVector3 origin{};
        try {
            const auto payload = Json::parse(event.payload_json);
            origin = parse_vector(payload.value("position", Json::object()));
        } catch (...) {
        }
        static_cast<void>(spawn(binding->second, origin, event.sequence, event.sequence));
    }
}

bool VfxRuntime::spawn(const std::string_view graph_id, const VfxVector3 origin, const std::uint64_t seed,
                       const std::uint64_t source_sequence) {
    const auto found = graphs_.find(std::string(graph_id));
    if (found == graphs_.end()) return false;
    const auto& graph = found->second;
    const auto graph_alive = std::ranges::count(particle_graphs_, &graph);
    const auto graph_available = graph_alive < graph.capacity ? graph.capacity - static_cast<std::uint32_t>(graph_alive) : 0U;
    const auto global_available = particle_ids_.size() < global_capacity_ ? global_capacity_ - particle_ids_.size() : 0U;
    const auto spawn_count = std::min<std::size_t>({graph.burst_count, graph_available, global_available});
    dropped_particles_ += graph.burst_count - spawn_count;
    const auto emitter_id = next_emitter_id_++;
    const auto spawn_seed=seed^(source_sequence*0x9e3779b97f4a7c15ULL);
    for (std::size_t index = 0; index < spawn_count; ++index) {
        const auto particle_index=static_cast<std::uint32_t>(index);
        const auto z = particle_random_range(spawn_seed,particle_index,0U,-1.0F,1.0F);
        const auto angle = particle_random_range(spawn_seed,particle_index,1U,0.0F,6.28318530718F);
        const auto radial = std::sqrt(std::max(0.0F, 1.0F - z * z));
        const auto speed = particle_random_range(spawn_seed,particle_index,2U,graph.speed_min,graph.speed_max);
        particle_ids_.push_back(next_particle_id_++); emitter_ids_.push_back(emitter_id); particle_graphs_.push_back(&graph);
        positions_.push_back(origin); velocities_.push_back({std::cos(angle)*radial*speed,std::abs(z)*speed,std::sin(angle)*radial*speed});
        ages_.push_back(0.0F); lifetimes_.push_back(particle_random_range(spawn_seed,particle_index,3U,graph.lifetime_min,graph.lifetime_max));
        sizes_.push_back(graph.size_start); colors_.push_back(graph.color_start); blend_modes_.push_back(graph.blend_mode);
        spawn_seeds_.push_back(spawn_seed); spawn_indices_.push_back(particle_index); spawn_origins_.push_back(origin);
    }
    particle_view_dirty_=true;
    ++revision_;
    return true;
}

void VfxRuntime::fixed_step(const float delta_seconds) {
    for (std::size_t index=0;index<particle_ids_.size();++index) {
        const auto& graph = *particle_graphs_[index];
        ages_[index] += delta_seconds;
        const auto drag_factor = std::max(0.0F, 1.0F - graph.drag * delta_seconds);
        velocities_[index].x=(velocities_[index].x+graph.gravity.x*delta_seconds)*drag_factor;
        velocities_[index].y=(velocities_[index].y+graph.gravity.y*delta_seconds)*drag_factor;
        velocities_[index].z=(velocities_[index].z+graph.gravity.z*delta_seconds)*drag_factor;
        positions_[index].x+=velocities_[index].x*delta_seconds;
        positions_[index].y+=velocities_[index].y*delta_seconds;
        positions_[index].z+=velocities_[index].z*delta_seconds;
        const auto normalized_age = std::clamp(ages_[index] / lifetimes_[index], 0.0F, 1.0F);
        const auto interpolate = [normalized_age](const float start, const float end) {
            return start + (end - start) * normalized_age;
        };
        sizes_[index] = interpolate(graph.size_start, graph.size_end);
        colors_[index] = {interpolate(graph.color_start.r, graph.color_end.r),
            interpolate(graph.color_start.g, graph.color_end.g), interpolate(graph.color_start.b, graph.color_end.b),
            interpolate(graph.color_start.a, graph.color_end.a)};
    }
    std::size_t write{};
    for(std::size_t read=0;read<particle_ids_.size();++read) {
        if(ages_[read]>=lifetimes_[read]) continue;
        if(write!=read) {
            particle_ids_[write]=particle_ids_[read]; emitter_ids_[write]=emitter_ids_[read];
            particle_graphs_[write]=particle_graphs_[read]; positions_[write]=positions_[read];
            velocities_[write]=velocities_[read]; ages_[write]=ages_[read]; lifetimes_[write]=lifetimes_[read];
            sizes_[write]=sizes_[read]; colors_[write]=colors_[read]; blend_modes_[write]=std::move(blend_modes_[read]);
            spawn_seeds_[write]=spawn_seeds_[read]; spawn_indices_[write]=spawn_indices_[read]; spawn_origins_[write]=spawn_origins_[read];
        }
        ++write;
    }
    particle_ids_.resize(write); emitter_ids_.resize(write); particle_graphs_.resize(write); positions_.resize(write);
    velocities_.resize(write); ages_.resize(write); lifetimes_.resize(write); sizes_.resize(write); colors_.resize(write); blend_modes_.resize(write);
    spawn_seeds_.resize(write); spawn_indices_.resize(write); spawn_origins_.resize(write);
    particle_view_dirty_=true;
    ++simulated_steps_;
    ++revision_;
}

void VfxRuntime::tick(const float delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) return;
    accumulator_ += std::min(delta_seconds, fixed_step_seconds * 8.0F);
    std::uint32_t steps = 0;
    while (accumulator_ >= fixed_step_seconds && steps < 8) {
        fixed_step(fixed_step_seconds);
        accumulator_ -= fixed_step_seconds;
        ++steps;
    }
}

std::string VfxRuntime::graph_json(const std::string_view graph_id) const {
    const auto found = graphs_.find(std::string(graph_id));
    if (found == graphs_.end())
        return Json{{"schemaVersion", "noemancer.vfx-graph-inspection/0.1"}, {"valid", false},
                    {"code", "vfx.graph.not-found"}, {"graphId", graph_id}}.dump();
    auto result = graph_to_json(found->second);
    result["valid"] = true;
    result["code"] = "ok";
    return result.dump();
}

std::string VfxRuntime::gpu_program_json(const std::string_view graph_id) const {
    const auto found = graphs_.find(std::string(graph_id));
    if (found == graphs_.end()) return Json{{"schemaVersion", "noemancer.vfx-gpu-program/0.1"},
        {"valid", false}, {"code", "vfx.graph.not-found"}, {"graphId", graph_id}}.dump();
    const auto& graph = found->second;
    constexpr std::uint64_t particle_stride = 112;
    constexpr std::uint64_t spawn_identity_stride = 16;
    constexpr std::uint64_t spawn_graph_stride = 96;
    constexpr std::uint32_t thread_group_size = 64;
    const auto dispatch_groups = (graph.capacity + thread_group_size - 1U) / thread_group_size;
    const auto graph_document = graph_to_json(graph).dump();
    return Json{{"schemaVersion", "noemancer.vfx-gpu-program/0.1"}, {"valid", true}, {"code", "ok"},
        {"graphId", graph.id}, {"spritePolicy", sprite_policy_json(graph)},
        {"compileKey", text_digest(graph_document + "\nvfx-compute-abi/0.7")},
        {"abi", "structured-particle-gpu-lifecycle-indirect/0.7"},
        {"execution", {{"preferred", "gpu-compute"}, {"reference", "cpu-deterministic"},
            {"dispatchActive", false}, {"dispatchScope", "headless-control-plane"},
            {"interactiveRenderer", "query-runtime-evidence"}, {"runtimeEvidencePath", "renderer.status.vfxGpu"},
            {"residency", "gpu-alive-ping-pong/dead-list"},
            {"uploadPolicy", "particle-identity-plus-emitter-graph-parameters"},
            {"randomInitialization", "gpu-stateless-u32-hash/seed-particle-index-channel"},
            {"spawnCatchUp", "gpu-fixed-60hz/max-512-steps"},
            {"curveEvaluation", "gpu-age/color-size-start-end"},
            {"blendGrouping", "gpu-partition/additive-alpha"},
            {"alphaSort", "gpu-bitonic-multi-dispatch/back-to-front/stable-particle-id/dynamic-span/max-8192"}}},
        {"kernel", {{"id", "vfx.kernel.simulate-spawn"}, {"artifactStem", "vfx_sim.comp"},
            {"spawnArtifactStem", "vfx_spawn.comp"}, {"groupArtifactStem", "vfx_group.comp"},
            {"sortArtifactStem", "vfx_sort_alpha.comp"}, {"sourceLanguage", "HLSL 2021"},
            {"entryPoint", "main"}, {"formats", {"DXIL", "SPIR-V"}},
            {"threadGroup", {thread_group_size, 1, 1}}, {"dispatchGroups", {dispatch_groups, 1, 1}}}},
        {"buffers", Json::array({
            {{"binding", 0}, {"name", "particles"}, {"access", "read-write"}, {"layout", "ParticleState/0.2"},
                {"strideBytes", particle_stride}, {"elementCount", graph.capacity}, {"bytes", particle_stride * graph.capacity}},
            {{"binding", 1}, {"name", "aliveIndicesA"}, {"access", "read-write"}, {"strideBytes", 4},
                {"elementCount", graph.capacity}, {"bytes", 4ULL * graph.capacity}},
            {{"binding", 2}, {"name", "aliveIndicesB"}, {"access", "read-write"}, {"strideBytes", 4},
                {"elementCount", graph.capacity}, {"bytes", 4ULL * graph.capacity}},
            {{"binding", 3}, {"name", "deadIndices"}, {"access", "read-write"}, {"strideBytes", 4},
                {"elementCount", graph.capacity}, {"bytes", 4ULL * graph.capacity}},
            {{"binding", 4}, {"name", "indirectA"}, {"access", "read-write-indirect"}, {"bytes", 16},
                {"layout", "SDL_GPUIndirectDrawCommand"}},
            {{"binding", 5}, {"name", "indirectB"}, {"access", "read-write-indirect"}, {"bytes", 16},
                {"layout", "SDL_GPUIndirectDrawCommand"}},
            {{"binding", 6}, {"name", "deadCounter"}, {"access", "read-write"}, {"bytes", 16}},
            {{"binding", 7}, {"name", "spawnIdentities"}, {"access", "read-write"}, {"strideBytes", spawn_identity_stride},
                {"elementCount", graph.capacity}, {"bytes", spawn_identity_stride * graph.capacity}},
            {{"binding", 8}, {"name", "spawnGraphParameters"}, {"access", "read-write"}, {"strideBytes", spawn_graph_stride},
                {"elementCount", graph.capacity}, {"bytes", spawn_graph_stride * graph.capacity}},
            {{"binding", 9}, {"name", "additiveIndices"}, {"access", "read-write-graphics-read"}, {"strideBytes", 4},
                {"elementCount", graph.capacity}, {"bytes", 4ULL * graph.capacity}},
            {{"binding", 10}, {"name", "alphaIndices"}, {"access", "read-write-graphics-read"}, {"strideBytes", 4},
                {"elementCount", graph.capacity}, {"bytes", 4ULL * graph.capacity}},
            {{"binding", 11}, {"name", "additiveIndirect"}, {"access", "read-write-indirect"}, {"bytes", 16}},
            {{"binding", 12}, {"name", "alphaIndirect"}, {"access", "read-write-indirect"}, {"bytes", 16}}
        })},
        {"uniforms", Json::array({{{"binding", 0}, {"name", "SimulationParameters"},
            {"fields", {"deltaSeconds", "inputCount", "capacity", "gravity", "drag"}}}})},
        {"graphicsConsumer", {{"vertexArtifact", "vfx_billboard.vert"}, {"fragmentArtifact", "vfx_billboard.frag"},
            {"primitive", "camera-facing-soft-disc"}, {"submission", "dual-indirect-instanced"},
            {"blendOrder", "additive-then-alpha"},
            {"postChain", {"HDR", "motion-vectors", "reactive-mask", "TAA", "bloom", "tone-map"}}}},
        {"workingSetBytes", (particle_stride + spawn_identity_stride + spawn_graph_stride + 20ULL) * graph.capacity + 80ULL},
        {"limitations", {"cpu-reference-still-materialized-for-observation",
            "multi-dispatch-bitonic-not-yet-tiled-radix", "graph-to-specialized-kernel-pending"}}}.dump();
}

std::string VfxRuntime::preview_json(const std::string_view graph_id, const std::uint64_t seed,
                                     const std::uint32_t steps, const float fixed_delta_seconds,
                                     const std::size_t max_particles) const {
    const auto found = graphs_.find(std::string(graph_id));
    if (found == graphs_.end() || steps > 3600 || !std::isfinite(fixed_delta_seconds) ||
        fixed_delta_seconds <= 0.0F || fixed_delta_seconds > 0.1F) {
        return Json{{"schemaVersion", "noemancer.vfx-preview/0.1"}, {"valid", false},
                    {"code", found == graphs_.end() ? "vfx.graph.not-found" : "vfx.preview.invalid-budget"},
                    {"graphId", graph_id}}.dump();
    }
    VfxRuntime preview(std::min<std::size_t>(global_capacity_, found->second.capacity));
    preview.graphs_.emplace(found->first, found->second);
    static_cast<void>(preview.spawn(graph_id, {}, seed, 0));
    for (std::uint32_t step = 0; step < steps; ++step) preview.fixed_step(fixed_delta_seconds);
    Json particles = Json::array();
    const auto preview_particles=preview.particles();
    const auto count = std::min(max_particles, preview_particles.size());
    for (std::size_t index = 0; index < count; ++index) particles.push_back(particle_json(preview_particles[index]));
    return Json{{"schemaVersion", "noemancer.vfx-preview/0.1"}, {"valid", true}, {"code", "ok"},
        {"graphId", graph_id}, {"executionTarget", "cpu-deterministic-reference"}, {"seed", seed},
        {"fixedDeltaSeconds", fixed_delta_seconds}, {"steps", steps}, {"aliveCount", preview_particles.size()},
        {"droppedCount", preview.dropped_particles_}, {"digest", particle_digest(preview.particle_view_)},
        {"truncated", count < preview_particles.size()}, {"particles", std::move(particles)}}.dump();
}

std::string VfxRuntime::observe_json(const std::size_t max_particles) const {
    const auto particle_view=particles();
    Json particles = Json::array();
    const auto count = std::min(max_particles, particle_view.size());
    for (std::size_t index = 0; index < count; ++index) particles.push_back(particle_json(particle_view[index]));
    std::vector<std::pair<std::string, std::string>> sorted_bindings(event_bindings_.begin(), event_bindings_.end());
    std::ranges::sort(sorted_bindings);
    Json bindings = Json::array();
    for (const auto& [event_type, graph_id] : sorted_bindings)
        bindings.push_back({{"eventType", event_type}, {"graphId", graph_id}});
    std::vector<std::string> sorted_graph_ids;
    sorted_graph_ids.reserve(graphs_.size());
    for (const auto& [graph_id, unused] : graphs_) {
        static_cast<void>(unused);
        sorted_graph_ids.push_back(graph_id);
    }
    std::ranges::sort(sorted_graph_ids);
    Json sprite_policies = Json::array();
    for (const auto& graph_id : sorted_graph_ids) {
        const auto& graph = graphs_.at(graph_id);
        auto policy = sprite_policy_json(graph);
        policy["graphId"] = graph.id;
        sprite_policies.push_back(std::move(policy));
    }
    return Json{{"schemaVersion", "noemancer.vfx-runtime/0.1"}, {"revision", revision_},
        {"simulation", "fixed-step-cpu-reference"}, {"gpuContract", "soa-pool-data-channel-indirect/0.1"},
        {"poolLayout","structure-of-arrays/0.1"},{"hotArrays",Json::array({"position","velocity","age","lifetime","size","color"})},
        {"globalCapacity", global_capacity_}, {"aliveCount", particle_view.size()}, {"droppedCount", dropped_particles_},
        {"simulatedSteps", simulated_steps_}, {"lastEventSequence", last_event_sequence_},
        {"digest", particle_digest(particle_view_)}, {"bindings", std::move(bindings)},
        {"spritePolicies", std::move(sprite_policies)},
        {"truncated", count < particle_view.size()}, {"particles", std::move(particles)}}.dump();
}

std::string VfxRuntime::benchmark_json(const std::string_view graph_id,const std::uint32_t particle_count,
                                       const std::uint32_t steps,const float fixed_delta_seconds) const {
    const auto found=graphs_.find(std::string(graph_id));
    if(found==graphs_.end()||particle_count==0||particle_count>65536U||steps==0||steps>600U||
       !std::isfinite(fixed_delta_seconds)||fixed_delta_seconds<=0.0F||fixed_delta_seconds>0.1F)
        return Json{{"schemaVersion","noemancer.vfx-benchmark/0.1"},{"valid",false},{"code","vfx.benchmark.invalid-budget"},
            {"graphId",graph_id},{"particleCount",particle_count},{"steps",steps}}.dump();
    VfxRuntime benchmark(particle_count); auto graph=found->second; graph.capacity=particle_count; graph.burst_count=particle_count;
    benchmark.graphs_.emplace(graph.id,std::move(graph)); static_cast<void>(benchmark.spawn(graph_id,{},42,0));
    const auto started=std::chrono::steady_clock::now();
    for(std::uint32_t step=0;step<steps;++step) benchmark.fixed_step(fixed_delta_seconds);
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
    const auto view=benchmark.particles();
    return Json{{"schemaVersion","noemancer.vfx-benchmark/0.1"},{"valid",true},{"code","ok"},{"graphId",graph_id},
        {"layout","structure-of-arrays/0.1"},{"requestedParticleCount",particle_count},{"aliveAfter",view.size()},
        {"steps",steps},{"fixedDeltaSeconds",fixed_delta_seconds},{"elapsedMicroseconds",elapsed},
        {"particleSteps",static_cast<std::uint64_t>(particle_count)*steps},
        {"particleStepsPerSecond",elapsed>0?static_cast<double>(particle_count)*steps*1000000.0/static_cast<double>(elapsed):0.0},
        {"digest",particle_digest(benchmark.particle_view_)},{"scope","cpu-deterministic-reference-not-gpu-performance"}}.dump();
}

std::string VfxRuntime::plan_graph_patch_json(const std::string_view graph_id, const std::string_view patch_json,
                                              const std::uint64_t base_revision) const {
    const auto found = graphs_.find(std::string(graph_id));
    if (found == graphs_.end()) return Json{{"schemaVersion", "noemancer.vfx-change-plan/0.1"}, {"valid", false},
        {"code", "vfx.graph.not-found"}, {"graphId", graph_id}, {"baseRevision", base_revision}}.dump();
    if (base_revision != revision_) return Json{{"schemaVersion", "noemancer.vfx-change-plan/0.1"}, {"valid", false},
        {"code", "vfx.revision-conflict"}, {"graphId", graph_id}, {"baseRevision", base_revision},
        {"currentRevision", revision_}}.dump();
    try {
        auto patch = Json::parse(patch_json);
        if (!patch.is_object()) throw std::invalid_argument("patch must be an object");
        auto before = graph_to_json(found->second);
        auto candidate = before;
        candidate.merge_patch(patch);
        if (candidate.value("graphId", std::string{}) != graph_id ||
            candidate.value("schemaVersion", std::string{}) != "noemancer.vfx-graph/0.1") {
            return Json{{"schemaVersion", "noemancer.vfx-change-plan/0.1"}, {"valid", false},
                {"code", "vfx.patch.protected-identity"}, {"graphId", graph_id}, {"baseRevision", base_revision}}.dump();
        }
        VfxRuntime validator(global_capacity_);
        const auto validation = validator.load_graph_json(candidate.dump());
        if (!validation.success) return Json{{"schemaVersion", "noemancer.vfx-change-plan/0.1"}, {"valid", false},
            {"code", validation.code}, {"detail", validation.detail}, {"graphId", graph_id},
            {"baseRevision", base_revision}}.dump();
        candidate = Json::parse(validator.graph_json(graph_id));
        candidate.erase("valid");
        candidate.erase("code");
        Json changed_paths = Json::array();
        collect_changed_paths(before, candidate, {}, changed_paths);
        const auto hash_source = std::string(graph_id) + "\n" + std::to_string(base_revision) + "\n" + candidate.dump();
        const auto content_hash = text_digest(hash_source);
        return Json{{"schemaVersion", "noemancer.vfx-change-plan/0.1"}, {"valid", true}, {"code", "ok"},
            {"planId", "vfx-plan-" + content_hash.substr(content_hash.find(':') + 1)}, {"contentHash", content_hash},
            {"graphId", graph_id}, {"baseRevision", base_revision}, {"before", std::move(before)},
            {"candidate", std::move(candidate)}, {"comparison", {{"changedPathCount", changed_paths.size()},
            {"changedPaths", std::move(changed_paths)}}}}.dump();
    } catch (const std::exception& error) {
        return Json{{"schemaVersion", "noemancer.vfx-change-plan/0.1"}, {"valid", false},
            {"code", "vfx.patch.invalid-json"}, {"detail", error.what()}, {"graphId", graph_id},
            {"baseRevision", base_revision}}.dump();
    }
}

std::string VfxRuntime::apply_graph_plan_json(const std::string_view plan_json, const bool dry_run) {
    const auto revision_before = revision_;
    try {
        const auto plan = Json::parse(plan_json);
        const auto graph_id = plan.at("graphId").get<std::string>();
        const auto base_revision = plan.at("baseRevision").get<std::uint64_t>();
        const auto candidate = plan.at("candidate");
        auto receipt = Json{{"schemaVersion", "noemancer.vfx-change-receipt/0.1"}, {"success", false},
            {"dryRun", dry_run}, {"graphId", graph_id}, {"planId", plan.value("planId", std::string{})},
            {"revisionBefore", revision_before}, {"revisionAfter", revision_before}};
        if (!plan.value("valid", false)) {
            receipt["code"] = "vfx.plan.integrity-failed";
            return receipt.dump();
        }
        if (base_revision != revision_) {
            receipt["code"] = "vfx.revision-conflict";
            return receipt.dump();
        }
        const auto found = graphs_.find(graph_id);
        if (found == graphs_.end()) {
            receipt["code"] = "vfx.graph.not-found";
            return receipt.dump();
        }
        VfxRuntime validator(global_capacity_);
        const auto validation = validator.load_graph_json(candidate.dump());
        if (!validation.success) {
            receipt["code"] = validation.code;
            return receipt.dump();
        }
        auto normalized_candidate = Json::parse(validator.graph_json(graph_id));
        normalized_candidate.erase("valid");
        normalized_candidate.erase("code");
        const auto expected_hash = text_digest(graph_id + "\n" + std::to_string(base_revision) + "\n" + normalized_candidate.dump());
        if (plan.value("contentHash", std::string{}) != expected_hash) {
            receipt["code"] = "vfx.plan.integrity-failed";
            return receipt.dump();
        }
        if (!dry_run) {
            const auto before = graph_to_json(found->second).dump();
            const auto result = load_graph_json(normalized_candidate.dump());
            if (!result.success) {
                receipt["code"] = result.code;
                return receipt.dump();
            }
            graph_history_.push_back({graph_id, before, graph_to_json(graphs_.at(graph_id)).dump()});
            if (graph_history_.size() > 64) graph_history_.erase(graph_history_.begin());
        }
        receipt["success"] = true;
        receipt["code"] = dry_run ? "vfx.plan.valid" : "ok";
        receipt["revisionAfter"] = revision_;
        return receipt.dump();
    } catch (const std::exception& error) {
        return Json{{"schemaVersion", "noemancer.vfx-change-receipt/0.1"}, {"success", false},
            {"dryRun", dry_run}, {"code", "vfx.plan.invalid"}, {"detail", error.what()},
            {"revisionBefore", revision_before}, {"revisionAfter", revision_}}.dump();
    }
}

std::string VfxRuntime::undo_graph_json(const std::uint64_t expected_revision) {
    const auto revision_before = revision_;
    if (expected_revision != revision_) return Json{{"schemaVersion", "noemancer.vfx-change-receipt/0.1"},
        {"success", false}, {"dryRun", false}, {"code", "vfx.revision-conflict"},
        {"revisionBefore", revision_before}, {"revisionAfter", revision_before}}.dump();
    if (graph_history_.empty()) return Json{{"schemaVersion", "noemancer.vfx-change-receipt/0.1"},
        {"success", false}, {"dryRun", false}, {"code", "vfx.undo.empty"},
        {"revisionBefore", revision_before}, {"revisionAfter", revision_before}}.dump();
    const auto entry = graph_history_.back();
    graph_history_.pop_back();
    const auto result = load_graph_json(entry.before_json);
    return Json{{"schemaVersion", "noemancer.vfx-change-receipt/0.1"}, {"success", result.success},
        {"dryRun", false}, {"code", result.success ? "ok" : result.code}, {"graphId", entry.graph_id},
        {"operation", "vfx.graph.undo"}, {"revisionBefore", revision_before}, {"revisionAfter", revision_}}.dump();
}

std::string VfxRuntime::default_graph_json() {
    VfxGraph graph;
    graph.id = "vfx.debug-impact";
    graph.display_name = "Debug Impact";
    graph.capacity = 2048;
    graph.burst_count = 48;
    return graph_to_json(graph).dump();
}

std::string VfxRuntime::default_alpha_graph_json() {
    VfxGraph graph;
    graph.id = "vfx.debug-smoke";
    graph.display_name = "Debug Smoke";
    graph.capacity = 2048;
    graph.burst_count = 48;
    graph.lifetime_min = 1.4F;
    graph.lifetime_max = 2.2F;
    graph.speed_min = 0.35F;
    graph.speed_max = 1.1F;
    graph.gravity = {0.0F,0.45F,0.0F};
    graph.drag = 0.35F;
    graph.size_start = 0.24F;
    graph.size_end = 0.72F;
    graph.color_start = {0.28F,0.58F,1.0F,0.48F};
    graph.color_end = {0.12F,0.18F,0.32F,0.0F};
    graph.blend_mode = "alpha";
    return graph_to_json(graph).dump();
}

} // namespace noemancer
