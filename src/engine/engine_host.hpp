#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

enum class FramePhase {
    BeginFrame,
    Input,
    FixedUpdate,
    Update,
    Extract,
    PrepareRender,
    Render,
    Present,
    EndFrame
};

enum class ModuleInitLevel {
    Core,
    Services,
    Scene,
    Editor
};

using FramePhaseMask = std::uint32_t;

[[nodiscard]] constexpr FramePhaseMask phase_bit(const FramePhase phase) noexcept {
    return FramePhaseMask{1} << static_cast<std::uint32_t>(phase);
}

enum class ModuleState {
    Registered,
    Ready,
    Disabled,
    Stopped,
    Failed
};

struct ModuleDescriptor final {
    std::string id;
    std::string display_name;
    std::string domain;
    std::vector<std::string> dependencies;
    ModuleInitLevel init_level{ModuleInitLevel::Services};
    FramePhaseMask phase_mask{};
    bool required{};
    bool interactive_only{};
};

struct ModuleStatus final {
    ModuleDescriptor descriptor;
    ModuleState state{ModuleState::Registered};
    std::uint64_t phase_ticks{};
    std::string detail;
};

struct FrameContext final {
    std::uint64_t frame_index{};
    double delta_seconds{};
    double fixed_delta_seconds{};
    double interpolation_alpha{};
    std::uint32_t fixed_step_index{};
    std::uint32_t fixed_step_count{};
    bool headless{};
};

struct FramePlan final {
    double delta_seconds{};
    double interpolation_alpha{};
    std::uint32_t fixed_step_count{};
    bool time_was_clamped{};
};

class EngineModule {
public:
    virtual ~EngineModule() = default;

    [[nodiscard]] virtual const ModuleDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual bool initialize(bool headless, std::string& error) = 0;
    virtual void tick(FramePhase phase, const FrameContext& context) = 0;
    virtual void shutdown() noexcept = 0;
};

class EngineHost final {
public:
    EngineHost();
    ~EngineHost();
    EngineHost(EngineHost&&) noexcept;
    EngineHost& operator=(EngineHost&&) noexcept;
    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    void register_module(std::unique_ptr<EngineModule> module);
    void register_default_modules();
    [[nodiscard]] bool initialize(bool headless);
    void tick(FramePhase phase, double delta_seconds);
    [[nodiscard]] FramePlan plan_frame(double delta_seconds);
    template <typename FixedUpdate>
    void run_frame(const double delta_seconds, FixedUpdate&& fixed_update) {
        const auto plan = plan_frame(delta_seconds);
        current_frame_plan_ = plan;
        tick(FramePhase::BeginFrame, plan.delta_seconds);
        tick(FramePhase::Input, plan.delta_seconds);
        for (std::uint32_t step = 0; step < plan.fixed_step_count; ++step) {
            current_fixed_step_ = step;
            tick(FramePhase::FixedUpdate, fixed_delta_seconds_);
            fixed_update();
        }
        tick(FramePhase::Update, plan.delta_seconds);
        tick(FramePhase::Extract, plan.delta_seconds);
        tick(FramePhase::PrepareRender, plan.delta_seconds);
        tick(FramePhase::Render, plan.delta_seconds);
        tick(FramePhase::Present, plan.delta_seconds);
        tick(FramePhase::EndFrame, plan.delta_seconds);
    }
    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint64_t frame_index() const noexcept;
    [[nodiscard]] double interpolation_alpha() const noexcept;
    [[nodiscard]] std::uint32_t fixed_steps_last_frame() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;
    [[nodiscard]] const std::vector<ModuleStatus>& statuses() const noexcept;
    [[nodiscard]] std::string status_json() const;

private:
    [[nodiscard]] bool validate_and_sort_graph();

    std::vector<std::unique_ptr<EngineModule>> modules_;
    std::vector<ModuleStatus> statuses_;
    std::uint64_t frame_index_{};
    double fixed_delta_seconds_{1.0 / 60.0};
    double max_frame_delta_seconds_{0.25};
    double accumulator_seconds_{};
    FramePlan current_frame_plan_{};
    std::uint32_t current_fixed_step_{};
    std::uint32_t max_fixed_steps_per_frame_{8};
    bool initialized_{};
    bool headless_{};
    std::string last_error_;
};

[[nodiscard]] std::string_view to_string(FramePhase phase) noexcept;
[[nodiscard]] std::string_view to_string(ModuleInitLevel level) noexcept;
[[nodiscard]] std::string_view to_string(ModuleState state) noexcept;

} // namespace noemancer
