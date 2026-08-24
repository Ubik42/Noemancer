#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

enum class StartupHubProjectStatus : std::uint8_t {
    available,
    missing,
    not_directory
};

[[nodiscard]] const char* startup_hub_project_status_name(StartupHubProjectStatus status) noexcept;

struct StartupHubRecentProject final {
    std::string path;
    std::string display_name;
    std::uint64_t last_opened_unix_seconds{};
};

struct StartupHubProject final {
    std::string path;
    std::string display_name;
    std::uint64_t last_opened_unix_seconds{};
    StartupHubProjectStatus status{StartupHubProjectStatus::missing};
    bool exists{};
    bool directory{};
};

enum class StartupHubActionKind : std::uint8_t {
    open_project,
    new_project,
    empty_workspace
};

[[nodiscard]] const char* startup_hub_action_kind_name(StartupHubActionKind kind) noexcept;

struct StartupHubRequest final {
    StartupHubActionKind kind{StartupHubActionKind::empty_workspace};
    std::string request_id;
    std::string project_path;
    std::string project_name;
};

struct StartupHubOptions final {
    std::size_t max_recent_projects{12U};
    std::string path_base;
};

enum class StartupHubLogoPrimitiveKind : std::uint8_t {
    polygon,
    line
};

// Small, renderer-neutral primitives let a native GUI draw the mark without
// importing SVG or a third-party UI type. Coordinates are in a 64x64 view box.
struct StartupHubLogoPrimitive final {
    StartupHubLogoPrimitiveKind kind{StartupHubLogoPrimitiveKind::polygon};
    std::vector<float> points;
    std::string fill;
    std::string stroke;
    float stroke_width{};
};

struct StartupHubBrand final {
    std::string id{"noemancer"};
    std::string title{"Noemancer"};
    std::string subtitle{"Project Hub"};
    std::string palette_name{"paper-and-ochre"};
    std::vector<StartupHubLogoPrimitive> logo;
};

struct StartupHubView final {
    std::string schema_version{"noemancer.startup-hub/0.1"};
    std::string selected_project_path;
    std::vector<StartupHubProject> recent_projects;
    bool can_open_project{};
    bool can_new_project{true};
    bool can_empty_workspace{true};
};

// Native-GUI-independent startup model. It owns no Project or Editor state;
// callers consume a request and decide which authoritative loader to invoke.
class StartupHubModel final {
public:
    explicit StartupHubModel(StartupHubOptions options = {});

    [[nodiscard]] const StartupHubOptions& options() const noexcept;
    [[nodiscard]] const StartupHubView& view() const noexcept;
    [[nodiscard]] const StartupHubBrand& brand() const noexcept;

    void set_recent_projects(std::vector<StartupHubRecentProject> projects);
    void refresh_project_states();
    [[nodiscard]] bool select_project(std::string_view path);

    [[nodiscard]] bool request_open_project();
    [[nodiscard]] bool request_new_project(std::string path, std::string project_name = {});
    [[nodiscard]] bool request_empty_workspace();
    [[nodiscard]] std::optional<StartupHubRequest> consume_request();

    // This is the Agent-facing and accessibility-facing projection of the
    // same model. It contains normalized paths and explicit status/action data.
    [[nodiscard]] std::string semantic_snapshot_json() const;

    // Self-authored, flat SVG; native callers may use logo() instead.
    [[nodiscard]] static std::string brand_svg();
    [[nodiscard]] static StartupHubBrand default_brand();

private:
    [[nodiscard]] std::string normalize_path(std::string_view path) const;
    [[nodiscard]] static std::string display_name_for_path(std::string_view path);
    [[nodiscard]] static std::string request_id(const StartupHubRequest& request);
    [[nodiscard]] bool queue_request(StartupHubRequest request);
    void sort_and_deduplicate(std::vector<StartupHubRecentProject> projects);
    void rebuild_project_status(StartupHubProject& project) const;

    StartupHubOptions options_;
    StartupHubBrand brand_;
    StartupHubView view_;
    std::optional<StartupHubRequest> pending_request_;
    std::string last_error_;
};

} // namespace noemancer
