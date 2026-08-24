#include "engine/engine_host.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

class SkeletonModule final : public EngineModule {
public:
    explicit SkeletonModule(ModuleDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}

    const ModuleDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    bool initialize(const bool headless, std::string& error) override {
        static_cast<void>(headless);
        error.clear();
        initialized_ = true;
        return true;
    }

    void tick(const FramePhase phase, const FrameContext& context) override {
        static_cast<void>(phase);
        static_cast<void>(context);
        if (initialized_) {
            ++phase_ticks_;
        }
    }

    void shutdown() noexcept override {
        initialized_ = false;
    }

    [[nodiscard]] std::uint64_t phase_ticks() const noexcept {
        return phase_ticks_;
    }

private:
    ModuleDescriptor descriptor_;
    bool initialized_{};
    std::uint64_t phase_ticks_{};
};

ModuleDescriptor module(
    std::string id,
    std::string display_name,
    std::string domain,
    std::vector<std::string> dependencies,
    const ModuleInitLevel init_level,
    const FramePhaseMask phase_mask,
    const bool required = false,
    const bool interactive_only = false) {
    return ModuleDescriptor{
        .id = std::move(id),
        .display_name = std::move(display_name),
        .domain = std::move(domain),
        .dependencies = std::move(dependencies),
        .init_level = init_level,
        .phase_mask = phase_mask,
        .required = required,
        .interactive_only = interactive_only
    };
}

constexpr FramePhaseMask phases(std::initializer_list<FramePhase> values) {
    FramePhaseMask result{};
    for (const auto value : values) {
        result |= phase_bit(value);
    }
    return result;
}

} // namespace

std::string_view to_string(const FramePhase phase) noexcept {
    switch (phase) {
    case FramePhase::BeginFrame: return "begin-frame";
    case FramePhase::Input: return "input";
    case FramePhase::FixedUpdate: return "fixed-update";
    case FramePhase::Update: return "update";
    case FramePhase::Extract: return "extract";
    case FramePhase::PrepareRender: return "prepare-render";
    case FramePhase::Render: return "render";
    case FramePhase::Present: return "present";
    case FramePhase::EndFrame: return "end-frame";
    }
    return "unknown";
}

std::string_view to_string(const ModuleInitLevel level) noexcept {
    switch (level) {
    case ModuleInitLevel::Core: return "core";
    case ModuleInitLevel::Services: return "services";
    case ModuleInitLevel::Scene: return "scene";
    case ModuleInitLevel::Editor: return "editor";
    }
    return "unknown";
}

std::string_view to_string(const ModuleState state) noexcept {
    switch (state) {
    case ModuleState::Registered: return "registered";
    case ModuleState::Ready: return "ready";
    case ModuleState::Disabled: return "disabled";
    case ModuleState::Stopped: return "stopped";
    case ModuleState::Failed: return "failed";
    }
    return "unknown";
}

EngineHost::EngineHost() = default;
EngineHost::~EngineHost() {
    shutdown();
}
EngineHost::EngineHost(EngineHost&&) noexcept = default;
EngineHost& EngineHost::operator=(EngineHost&&) noexcept = default;

void EngineHost::register_module(std::unique_ptr<EngineModule> module_instance) {
    if (initialized_) {
        throw std::logic_error("Cannot register a module after EngineHost initialization");
    }
    if (module_instance == nullptr) {
        throw std::invalid_argument("Cannot register a null engine module");
    }
    modules_.push_back(std::move(module_instance));
}

void EngineHost::register_default_modules() {
    if (!modules_.empty()) {
        throw std::logic_error("Default engine modules can only be registered once");
    }

    const std::vector<ModuleDescriptor> descriptors{
        module("core.platform", "Platform", "core", {}, ModuleInitLevel::Core, phases({FramePhase::BeginFrame, FramePhase::EndFrame}), true),
        module("core.memory", "Memory and containers", "core", {"core.platform"}, ModuleInitLevel::Core, 0, true),
        module("core.jobs", "Job system", "core", {"core.memory"}, ModuleInitLevel::Core, phases({FramePhase::BeginFrame, FramePhase::EndFrame}), true),
        module("core.schema", "Reflection and schema", "core", {"core.memory"}, ModuleInitLevel::Core, 0, true),
        module("diagnostics.runtime", "Diagnostics and telemetry", "diagnostics", {"core.platform"}, ModuleInitLevel::Core, phases({FramePhase::BeginFrame, FramePhase::EndFrame}), true),
        module("world.scene", "World and scene lifecycle", "world", {"core.jobs", "core.schema"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate, FramePhase::Update}), true),
        module("asset.registry", "Asset registry", "asset", {"core.schema"}, ModuleInitLevel::Services, phases({FramePhase::Update}), true),
        module("asset.cooker", "Asset importer and cooker", "asset", {"asset.registry", "core.jobs"}, ModuleInitLevel::Services, phases({FramePhase::Update})),
        module("input.actions", "Input actions", "input", {"core.platform"}, ModuleInitLevel::Services, phases({FramePhase::Input, FramePhase::EndFrame})),
        module("physics.world", "Physics world", "physics", {"world.scene", "core.jobs"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate})),
        module("animation.runtime", "Animation runtime", "animation", {"world.scene", "asset.registry", "core.jobs"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate, FramePhase::Update})),
        module("audio.mixer", "Audio mixer", "audio", {"asset.registry", "core.jobs"}, ModuleInitLevel::Services, phases({FramePhase::Update})),
        module("scripting.host", "Gameplay scripting host", "scripting", {"world.scene", "core.schema"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate, FramePhase::Update})),
        module("navigation.world", "Navigation world", "navigation", {"world.scene", "core.jobs"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate, FramePhase::Update})),
        module("network.transport", "Network transport", "network", {"core.platform", "core.jobs"}, ModuleInitLevel::Services, phases({FramePhase::Input, FramePhase::EndFrame})),
        module("network.replication", "Network replication", "network", {"network.transport", "world.scene", "core.schema"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate, FramePhase::EndFrame})),
        module("collaboration.session", "Collaborative editing session", "collaboration", {"network.transport", "world.scene", "core.schema"}, ModuleInitLevel::Editor, phases({FramePhase::Update})),
        module("gameplay.framework", "Gameplay framework", "gameplay", {"world.scene", "input.actions", "scripting.host"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate, FramePhase::Update})),
        module("render.rhi", "Render hardware interface", "render", {"core.platform", "diagnostics.runtime"}, ModuleInitLevel::Services, phases({FramePhase::PrepareRender, FramePhase::Present}), false, true),
        module("render.graph", "Render graph", "render", {"render.rhi", "asset.registry"}, ModuleInitLevel::Services, phases({FramePhase::Extract, FramePhase::PrepareRender, FramePhase::Render}), false, true),
        module("ui.core", "Semantic UI and text", "ui", {"render.graph", "input.actions", "core.schema"}, ModuleInitLevel::Editor, phases({FramePhase::Input, FramePhase::Update, FramePhase::Extract, FramePhase::Render}), false, true),
        module("vfx.runtime", "VFX and particles", "vfx", {"render.graph", "world.scene", "asset.registry"}, ModuleInitLevel::Scene, phases({FramePhase::FixedUpdate, FramePhase::Extract, FramePhase::PrepareRender, FramePhase::Render}), false, true),
        module("plugin.host", "Plugin host", "plugin", {"core.schema", "asset.registry"}, ModuleInitLevel::Services, 0),
        module("build.pipeline", "Build and package pipeline", "build", {"asset.cooker", "plugin.host", "diagnostics.runtime"}, ModuleInitLevel::Editor, 0),
        module("agent.control", "Agent control plane", "devtools", {"core.schema", "world.scene", "asset.registry", "diagnostics.runtime"}, ModuleInitLevel::Editor, phases({FramePhase::BeginFrame, FramePhase::EndFrame}))
    };

    for (const auto& descriptor : descriptors) {
        register_module(std::make_unique<SkeletonModule>(descriptor));
    }
}

bool EngineHost::validate_and_sort_graph() {
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        const auto& module_instance = modules_[index];
        const auto& descriptor = module_instance->descriptor();
        if (descriptor.id.empty()) {
            last_error_ = "Engine module has an empty ID";
            return false;
        }
        if (!indices.emplace(descriptor.id, index).second) {
            last_error_ = "Duplicate engine module ID: " + descriptor.id;
            return false;
        }
    }

    std::vector<std::vector<std::size_t>> dependents(modules_.size());
    std::vector<std::size_t> indegrees(modules_.size());
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        const auto& descriptor = modules_[index]->descriptor();
        for (const auto& dependency : descriptor.dependencies) {
            const auto found = indices.find(dependency);
            if (found == indices.end()) {
                last_error_ = "Module " + descriptor.id + " depends on missing module " + dependency;
                return false;
            }
            if (modules_[found->second]->descriptor().init_level > descriptor.init_level) {
                last_error_ = "Module " + descriptor.id + " depends on later initialization level module " + dependency;
                return false;
            }
            dependents[found->second].push_back(index);
            ++indegrees[index];
        }
    }

    using ReadyModule = std::pair<ModuleInitLevel, std::size_t>;
    std::priority_queue<ReadyModule, std::vector<ReadyModule>, std::greater<>> ready;
    for (std::size_t index = 0; index < indegrees.size(); ++index) {
        if (indegrees[index] == 0) ready.emplace(modules_[index]->descriptor().init_level, index);
    }
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const auto index = ready.top().second;
        ready.pop();
        order.push_back(index);
        for (const auto dependent : dependents[index]) {
            if (--indegrees[dependent] == 0) {
                ready.emplace(modules_[dependent]->descriptor().init_level, dependent);
            }
        }
    }
    if (order.size() != modules_.size()) {
        last_error_ = "Engine module dependency graph contains a cycle";
        return false;
    }
    std::vector<std::unique_ptr<EngineModule>> sorted;
    sorted.reserve(modules_.size());
    for (const auto index : order) sorted.push_back(std::move(modules_[index]));
    modules_ = std::move(sorted);
    return true;
}

bool EngineHost::initialize(const bool headless) {
    if (initialized_) {
        last_error_ = "EngineHost is already initialized";
        return false;
    }
    last_error_.clear();
    headless_ = headless;
    frame_index_ = 0;
    statuses_.clear();

    accumulator_seconds_ = 0.0;
    current_frame_plan_ = {};
    if (!validate_and_sort_graph()) {
        return false;
    }

    statuses_.reserve(modules_.size());
    for (auto& module_instance : modules_) {
        ModuleStatus status{
            .descriptor = module_instance->descriptor(),
            .state = ModuleState::Registered,
            .phase_ticks = 0,
            .detail = "registered"
        };
        if (headless_ && status.descriptor.interactive_only) {
            status.state = ModuleState::Disabled;
            status.detail = "disabled in headless mode";
            statuses_.push_back(std::move(status));
            continue;
        }

        std::string error;
        if (!module_instance->initialize(headless_, error)) {
            status.state = ModuleState::Failed;
            status.detail = error.empty() ? "initialization failed" : error;
            statuses_.push_back(std::move(status));
            last_error_ = "Failed to initialize module " + module_instance->descriptor().id;
            shutdown();
            return false;
        }
        status.state = ModuleState::Ready;
        status.detail = "ready";
        statuses_.push_back(std::move(status));
    }

    initialized_ = true;
    return true;
}

FramePlan EngineHost::plan_frame(const double delta_seconds) {
    if (!initialized_) throw std::logic_error("Cannot plan a frame for an uninitialized EngineHost");
    const double safe_delta = std::max(0.0, delta_seconds);
    const double clamped_delta = std::min(safe_delta, max_frame_delta_seconds_);
    accumulator_seconds_ += clamped_delta;
    const auto available_steps = static_cast<std::uint32_t>(
        std::floor((accumulator_seconds_ + 1.0e-12) / fixed_delta_seconds_));
    const auto fixed_steps = std::min(available_steps, max_fixed_steps_per_frame_);
    accumulator_seconds_ -= static_cast<double>(fixed_steps) * fixed_delta_seconds_;
    if (available_steps > max_fixed_steps_per_frame_) {
        accumulator_seconds_ = std::fmod(accumulator_seconds_, fixed_delta_seconds_);
    }
    accumulator_seconds_ = std::max(0.0, accumulator_seconds_);
    return FramePlan{
        .delta_seconds = clamped_delta,
        .interpolation_alpha = accumulator_seconds_ / fixed_delta_seconds_,
        .fixed_step_count = fixed_steps,
        .time_was_clamped = safe_delta != clamped_delta || available_steps > max_fixed_steps_per_frame_
    };
}

void EngineHost::tick(const FramePhase phase, const double delta_seconds) {
    if (!initialized_) {
        throw std::logic_error("Cannot tick an uninitialized EngineHost");
    }
    const FrameContext context{
        .frame_index = frame_index_,
        .delta_seconds = delta_seconds,
        .fixed_delta_seconds = fixed_delta_seconds_,
        .interpolation_alpha = current_frame_plan_.interpolation_alpha,
        .fixed_step_index = current_fixed_step_,
        .fixed_step_count = current_frame_plan_.fixed_step_count,
        .headless = headless_
    };
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        if (statuses_.at(index).state != ModuleState::Ready) {
            continue;
        }
        if ((statuses_.at(index).descriptor.phase_mask & phase_bit(phase)) == 0) {
            continue;
        }
        modules_.at(index)->tick(phase, context);
        ++statuses_.at(index).phase_ticks;
    }
    if (phase == FramePhase::EndFrame) {
        ++frame_index_;
    }
}

void EngineHost::shutdown() noexcept {
    if (statuses_.empty()) {
        initialized_ = false;
        return;
    }
    for (std::size_t index = statuses_.size(); index > 0; --index) {
        auto& status = statuses_.at(index - 1);
        if (status.state == ModuleState::Ready) {
            modules_.at(index - 1)->shutdown();
            status.state = ModuleState::Stopped;
            status.detail = "stopped";
        }
    }
    initialized_ = false;
}

bool EngineHost::initialized() const noexcept {
    return initialized_;
}

std::uint64_t EngineHost::frame_index() const noexcept {
    return frame_index_;
}

double EngineHost::interpolation_alpha() const noexcept { return current_frame_plan_.interpolation_alpha; }
std::uint32_t EngineHost::fixed_steps_last_frame() const noexcept { return current_frame_plan_.fixed_step_count; }

std::string_view EngineHost::last_error() const noexcept {
    return last_error_;
}

const std::vector<ModuleStatus>& EngineHost::statuses() const noexcept {
    return statuses_;
}

std::string EngineHost::status_json() const {
    Json modules = Json::array();
    std::size_t ready_count = 0;
    std::size_t disabled_count = 0;
    for (const auto& status : statuses_) {
        ready_count += status.state == ModuleState::Ready ? 1U : 0U;
        disabled_count += status.state == ModuleState::Disabled ? 1U : 0U;
        modules.push_back({
            {"id", status.descriptor.id},
            {"displayName", status.descriptor.display_name},
            {"domain", status.descriptor.domain},
            {"dependencies", status.descriptor.dependencies},
            {"initLevel", std::string(to_string(status.descriptor.init_level))},
            {"phaseMask", status.descriptor.phase_mask},
            {"required", status.descriptor.required},
            {"interactiveOnly", status.descriptor.interactive_only},
            {"state", std::string(to_string(status.state))},
            {"phaseTicks", status.phase_ticks},
            {"detail", status.detail}
        });
    }

    const Json result = {
        {"schemaVersion", "0.1"},
        {"lifecycle", initialized_ ? "running" : "stopped"},
        {"headless", headless_},
        {"frame", frame_index_},
        {"fixedStepsLastFrame", current_frame_plan_.fixed_step_count},
        {"interpolationAlpha", current_frame_plan_.interpolation_alpha},
        {"timeWasClamped", current_frame_plan_.time_was_clamped},
        {"moduleCount", statuses_.size()},
        {"readyCount", ready_count},
        {"disabledCount", disabled_count},
        {"lastError", last_error_},
        {"modules", std::move(modules)}
    };
    return result.dump();
}

} // namespace noemancer
