#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/managed_debug_transport.hpp"

namespace noemancer {

struct ManagedScriptInstance final {
    std::string id;
    std::string entity_id;
    std::string assembly_asset;
    std::string type_name;
    std::string state{"attached"};
    std::string public_state_json{"{}"};
    std::string last_callback;
    std::uint64_t callback_count{};
    std::string properties_json{"{}"};
    bool scene_owned{};
};

class ManagedScriptRuntime final {
public:
    ManagedScriptRuntime();
    ~ManagedScriptRuntime();
    ManagedScriptRuntime(const ManagedScriptRuntime&) = delete;
    ManagedScriptRuntime& operator=(const ManagedScriptRuntime&) = delete;
    [[nodiscard]] std::string abi_json() const;
    [[nodiscard]] std::string observe_json() const;
    [[nodiscard]] std::string attach_json(std::string_view instance_id,std::string_view entity_id,
                                          std::string_view assembly_asset,std::string_view type_name);
    [[nodiscard]] std::string invoke_json(std::string_view instance_id,std::string_view callback,
                                          std::string_view arguments_json,std::string_view context_json="{}");
    [[nodiscard]] std::string configure_project_json(const std::filesystem::path& project_root,
                                                     const std::filesystem::path& script_project);
    [[nodiscard]] std::string load_project_assembly_json(const std::filesystem::path& assembly,
                                                         std::string_view configuration = "Release");
    [[nodiscard]] std::string compile_project_json(std::string_view configuration);
    [[nodiscard]] std::string discover_project_types_json() const;
    [[nodiscard]] std::string project_observe_json() const;
    [[nodiscard]] std::string state_capture_json() const;
    [[nodiscard]] std::string state_restore_json(std::string_view document_json);
    [[nodiscard]] std::string debug_attach_json() const;
    [[nodiscard]] std::string debug_session_start_json();
    [[nodiscard]] std::string debug_session_status_json() const;
    [[nodiscard]] std::string debug_session_request_json(std::string_view command,
                                                         std::string_view arguments_json = "{}",
                                                         std::uint32_t timeout_ms = 5000U);
    [[nodiscard]] std::string debug_session_events_json();
    [[nodiscard]] std::string debug_session_stop_json(std::uint32_t timeout_ms = 2000U);
    [[nodiscard]] std::optional<std::string> instance_entity_id(std::string_view instance_id) const;
    [[nodiscard]] bool type_implements_callback(std::string_view type_name,std::string_view callback) const;
    void synchronize_instances(std::vector<ManagedScriptInstance> desired);
    void release_entity_instances(std::string_view entity_id);
    [[nodiscard]] std::vector<ManagedScriptInstance> automatic_instances() const;
    [[nodiscard]] bool project_ready() const noexcept { return !project_assembly_path_.empty(); }
private:
    void ensure_host() const;
    void release_host_instance(std::string_view instance_id);
    void refresh_source_probe() const;
    std::vector<ManagedScriptInstance> instances_;
    std::uint64_t revision_{1};
    mutable bool host_attempted_{};
    mutable bool host_ready_{};
    mutable void* hostfxr_module_{};
    mutable void* managed_entry_{};
    mutable std::string host_status_{"not-initialized"};
    mutable std::string host_error_;
    mutable std::string hostfxr_path_;
    mutable std::string runtime_config_path_;
    mutable std::string managed_assembly_path_;
    mutable std::string last_managed_result_;
    std::filesystem::path project_root_;
    std::filesystem::path script_project_;
    std::string last_compile_result_;
    std::string compile_fingerprint_;
    std::string project_assembly_path_;
    std::string project_build_fingerprint_;
    std::unique_ptr<ManagedDebugSession> debug_session_;
    mutable std::string type_catalog_fingerprint_;
    mutable std::string type_catalog_json_;
    std::string last_configuration_{"Debug"};
    mutable std::string observed_source_fingerprint_;
    mutable std::chrono::steady_clock::time_point source_probe_time_{};
    std::uint64_t compile_revision_{};
    std::string session_id_;
};

} // namespace noemancer
