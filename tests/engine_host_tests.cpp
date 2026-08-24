#include "engine/engine_host.hpp"
#include "engine/process_diagnostics.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestModule final : public noemancer::EngineModule {
public:
    TestModule(noemancer::ModuleDescriptor descriptor, std::vector<std::string>& initialization_order)
        : descriptor_(std::move(descriptor)), initialization_order_(initialization_order) {}

    const noemancer::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    bool initialize(bool, std::string& error) override {
        error.clear();
        initialization_order_.push_back(descriptor_.id);
        return true;
    }
    void tick(noemancer::FramePhase, const noemancer::FrameContext&) override {}
    void shutdown() noexcept override {}

private:
    noemancer::ModuleDescriptor descriptor_;
    std::vector<std::string>& initialization_order_;
};

} // namespace

int main() {
    noemancer::configure_process_diagnostics("test.engine-host");
    noemancer::EngineHost host;
    host.register_default_modules();
    if (!host.initialize(true)) {
        std::cerr << "EngineHost failed to initialize: " << host.last_error() << '\n';
        return 1;
    }
    if (host.statuses().size() != 25) {
        std::cerr << "The default engine skeleton is missing module boundaries\n";
        return 2;
    }

    std::size_t disabled_count = 0;
    for (const auto& status : host.statuses()) {
        disabled_count += status.state == noemancer::ModuleState::Disabled ? 1U : 0U;
    }
    if (disabled_count != 4) {
        std::cerr << "Headless mode did not disable the four interactive modules\n";
        return 3;
    }

    std::uint32_t fixed_update_count = 0;
    host.run_frame(1.0 / 120.0, [&fixed_update_count] {
        ++fixed_update_count;
    });
    host.run_frame(1.0 / 120.0, [&fixed_update_count] {
        ++fixed_update_count;
    });
    if (fixed_update_count != 1 || host.frame_index() != 2 || host.fixed_steps_last_frame() != 1) {
        std::cerr << "EngineHost fixed scheduling did not consume accumulated time\n";
        return 4;
    }

    const auto status_json = host.status_json();
    if (status_json.find(R"("moduleCount":25)") == std::string::npos ||
        status_json.find(R"("id":"render.graph")") == std::string::npos ||
        status_json.find(R"("id":"collaboration.session")") == std::string::npos ||
        status_json.find(R"("id":"agent.control")") == std::string::npos ||
        status_json.find(R"("initLevel":"scene")") == std::string::npos ||
        status_json.find(R"("fixedStepsLastFrame":1)") == std::string::npos) {
        std::cerr << "Engine status is missing structured module or frame data\n";
        return 5;
    }

    host.shutdown();
    if (host.initialized()) {
        std::cerr << "EngineHost did not shut down\n";
        return 6;
    }

    std::vector<std::string> initialization_order;
    noemancer::EngineHost unordered_host;
    unordered_host.register_module(std::make_unique<TestModule>(
        noemancer::ModuleDescriptor{
            .id = "dependent",
            .dependencies = {"foundation"},
            .init_level = noemancer::ModuleInitLevel::Scene
        }, initialization_order));
    unordered_host.register_module(std::make_unique<TestModule>(
        noemancer::ModuleDescriptor{
            .id = "foundation",
            .init_level = noemancer::ModuleInitLevel::Core
        }, initialization_order));
    if (!unordered_host.initialize(true) ||
        initialization_order != std::vector<std::string>{"foundation", "dependent"}) {
        std::cerr << "EngineHost did not resolve an out-of-order dependency graph\n";
        return 7;
    }
    return 0;
}
