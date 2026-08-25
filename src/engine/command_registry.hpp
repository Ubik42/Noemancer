#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

class World;
class AssetRegistry;
class ProjectUiAuthoringSession;
class ProjectUiAuthoringCommandService;

struct CommandInvocation final {
    int exit_code{};
    std::string output_json;
};

class CommandRegistry final {
public:
    CommandRegistry();
    explicit CommandRegistry(World& world);
    CommandRegistry(World& world, AssetRegistry& assets);
    ~CommandRegistry();
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;
    CommandRegistry(CommandRegistry&&) = delete;
    CommandRegistry& operator=(CommandRegistry&&) = delete;

    [[nodiscard]] std::string manifest_json() const;
    [[nodiscard]] CommandInvocation invoke(
        std::string_view name,
        std::string_view arguments_json) const;
    void attach_project_ui_authoring(ProjectUiAuthoringSession& session);
    void attach_editor_context(
        std::function<std::string()> observe,
        std::function<std::string(std::string_view)> apply_intent);

private:
    void register_commands();

    struct CommandDefinition final {
        std::string name;
        std::string description;
        std::string access;
        bool idempotent{};
        bool supports_dry_run{};
        std::string runtime_state;
        std::string task_kind;
        std::string input_schema_json;
        std::string output_schema_json;
        std::function<std::string(std::string_view)> handler;
    };

    std::unique_ptr<World> owned_world_;
    World* world_{};
    std::unique_ptr<AssetRegistry> owned_assets_;
    AssetRegistry* assets_{};
    std::unique_ptr<ProjectUiAuthoringCommandService> project_ui_authoring_;
    std::function<std::string()> editor_context_observe_;
    std::function<std::string(std::string_view)> editor_context_apply_intent_;
    std::vector<CommandDefinition> commands_;
};

} // namespace noemancer
