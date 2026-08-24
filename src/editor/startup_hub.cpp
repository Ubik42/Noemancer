#include "editor/startup_hub.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;
constexpr std::size_t maximum_path_bytes = 4096U;
constexpr std::size_t maximum_name_bytes = 256U;

std::string lower_ascii(std::string value) {
    for (auto& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::string casefold_path(const std::string_view path) {
#ifdef _WIN32
    return lower_ascii(std::string(path));
#else
    return std::string(path);
#endif
}

std::string capped(std::string value, const std::size_t maximum) {
    if (value.size() > maximum) value.resize(maximum);
    return value;
}

void feed_hash(std::uint64_t& hash, const std::string_view value) {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    hash ^= 0xFFU;
    hash *= prime;
}

std::string hash_hex(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16U, '0');
    auto remaining = value;
    for (std::size_t index = result.size(); index > 0U; --index) {
        result[index - 1U] = digits[remaining & 0x0FU];
        remaining >>= 4U;
    }
    return result;
}

} // namespace

const char* startup_hub_project_status_name(const StartupHubProjectStatus status) noexcept {
    switch (status) {
    case StartupHubProjectStatus::available: return "available";
    case StartupHubProjectStatus::missing: return "missing";
    case StartupHubProjectStatus::not_directory: return "not-directory";
    }
    return "missing";
}

const char* startup_hub_action_kind_name(const StartupHubActionKind kind) noexcept {
    switch (kind) {
    case StartupHubActionKind::open_project: return "openProject";
    case StartupHubActionKind::new_project: return "newProject";
    case StartupHubActionKind::empty_workspace: return "emptyWorkspace";
    }
    return "emptyWorkspace";
}

StartupHubModel::StartupHubModel(StartupHubOptions options)
    : options_(std::move(options)), brand_(default_brand()) {
    if (options_.max_recent_projects > 128U) options_.max_recent_projects = 128U;
}

const StartupHubOptions& StartupHubModel::options() const noexcept {
    return options_;
}

const StartupHubView& StartupHubModel::view() const noexcept {
    return view_;
}

const StartupHubBrand& StartupHubModel::brand() const noexcept {
    return brand_;
}

void StartupHubModel::set_recent_projects(std::vector<StartupHubRecentProject> projects) {
    const auto previous_selection = view_.selected_project_path;
    sort_and_deduplicate(std::move(projects));
    if (!previous_selection.empty() && select_project(previous_selection)) return;
    last_error_.clear();
    view_.selected_project_path.clear();
    for (const auto& project : view_.recent_projects) {
        if (project.status == StartupHubProjectStatus::available) {
            view_.selected_project_path = project.path;
            break;
        }
    }
    if (view_.selected_project_path.empty() && !view_.recent_projects.empty())
        view_.selected_project_path = view_.recent_projects.front().path;
}

void StartupHubModel::refresh_project_states() {
    for (auto& project : view_.recent_projects) rebuild_project_status(project);
}

bool StartupHubModel::select_project(const std::string_view path) {
    const auto normalized = normalize_path(path);
    const auto key = casefold_path(normalized);
    const auto found = std::ranges::find_if(view_.recent_projects, [&](const auto& project) {
        return casefold_path(project.path) == key;
    });
    if (normalized.empty() || found == view_.recent_projects.end()) {
        last_error_ = "The requested project is not in the recent-project list.";
        return false;
    }
    view_.selected_project_path = found->path;
    last_error_.clear();
    return true;
}

bool StartupHubModel::request_open_project() {
    if (view_.selected_project_path.empty()) {
        last_error_ = "Select a recent project before opening it.";
        return false;
    }
    return queue_request({.kind = StartupHubActionKind::open_project,
                          .project_path = view_.selected_project_path});
}

bool StartupHubModel::request_new_project(std::string path, std::string project_name) {
    path = normalize_path(path);
    if (path.empty()) {
        last_error_ = "A project path is required before creating a project.";
        return false;
    }
    if (project_name.empty()) project_name = display_name_for_path(path);
    project_name = capped(std::move(project_name), maximum_name_bytes);
    if (project_name.empty()) {
        last_error_ = "A project name is required before creating a project.";
        return false;
    }
    return queue_request({.kind = StartupHubActionKind::new_project,
                          .project_path = std::move(path), .project_name = std::move(project_name)});
}

bool StartupHubModel::request_empty_workspace() {
    return queue_request({.kind = StartupHubActionKind::empty_workspace});
}

std::optional<StartupHubRequest> StartupHubModel::consume_request() {
    if (!pending_request_) return std::nullopt;
    auto result = std::move(pending_request_);
    pending_request_.reset();
    return result;
}

std::string StartupHubModel::semantic_snapshot_json() const {
    Json result{{"schemaVersion", view_.schema_version},
                {"code", "ok"},
                {"selectedProjectPath", view_.selected_project_path},
                {"maxRecentProjects", options_.max_recent_projects},
                {"brand", {{"id", brand_.id}, {"title", brand_.title},
                            {"subtitle", brand_.subtitle}, {"palette", brand_.palette_name}}},
                {"recentProjects", Json::array()},
                {"actions", Json::array()}};
    for (const auto& project : view_.recent_projects) {
        result["recentProjects"].push_back({
            {"path", project.path}, {"displayName", project.display_name},
            {"lastOpenedUnixSeconds", project.last_opened_unix_seconds},
            {"status", startup_hub_project_status_name(project.status)},
            {"exists", project.exists}, {"directory", project.directory},
            {"selected", project.path == view_.selected_project_path}});
    }
    result["actions"].push_back({{"kind", startup_hub_action_kind_name(StartupHubActionKind::open_project)},
                                  {"enabled", view_.can_open_project},
                                  {"projectPath", view_.selected_project_path}});
    result["actions"].push_back({{"kind", startup_hub_action_kind_name(StartupHubActionKind::new_project)},
                                  {"enabled", view_.can_new_project}});
    result["actions"].push_back({{"kind", startup_hub_action_kind_name(StartupHubActionKind::empty_workspace)},
                                  {"enabled", view_.can_empty_workspace}});
    if (pending_request_) {
        result["pendingRequest"] = {{"kind", startup_hub_action_kind_name(pending_request_->kind)},
                                     {"requestId", pending_request_->request_id},
                                     {"projectPath", pending_request_->project_path},
                                     {"projectName", pending_request_->project_name}};
    } else {
        result["pendingRequest"] = nullptr;
    }
    if (!last_error_.empty()) {
        result["code"] = "startup-hub.invalid-request";
        result["lastError"] = last_error_;
    }
    return result.dump();
}

std::string StartupHubModel::brand_svg() {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" role="img" aria-labelledby="title desc">
  <title id="title">Noemancer</title>
  <desc id="desc">A flat geometric N mark in navy, ochre and sea green.</desc>
  <rect x="4" y="4" width="56" height="56" rx="10" fill="#142433"/>
  <path d="M16 48V16h8l16 20V16h8v32h-8L24 28v20z" fill="#D4A15D"/>
  <path d="M12 52h40" stroke="#6E9A8B" stroke-width="3" stroke-linecap="round"/>
</svg>)svg";
}

StartupHubBrand StartupHubModel::default_brand() {
    return {
        .id = "noemancer",
        .title = "Noemancer",
        .subtitle = "Project Hub",
        .palette_name = "paper-and-ochre",
        .logo = {
            {.kind = StartupHubLogoPrimitiveKind::polygon,
             .points = {4.0F, 4.0F, 60.0F, 4.0F, 60.0F, 60.0F, 4.0F, 60.0F},
             .fill = "#142433"},
            {.kind = StartupHubLogoPrimitiveKind::polygon,
             .points = {16.0F, 48.0F, 16.0F, 16.0F, 24.0F, 16.0F, 40.0F, 36.0F,
                        40.0F, 16.0F, 48.0F, 16.0F, 48.0F, 48.0F, 40.0F, 48.0F,
                        24.0F, 28.0F, 24.0F, 48.0F},
             .fill = "#D4A15D"},
            {.kind = StartupHubLogoPrimitiveKind::line,
             .points = {12.0F, 52.0F, 52.0F, 52.0F},
             .stroke = "#6E9A8B", .stroke_width = 3.0F}}};
}

std::string StartupHubModel::normalize_path(const std::string_view path) const {
    if (path.empty() || path.size() > maximum_path_bytes) return {};
    std::filesystem::path candidate{std::string(path)};
    if (candidate.is_relative() && !options_.path_base.empty())
        candidate = std::filesystem::path(options_.path_base) / candidate;
    std::error_code filesystem_error;
    candidate = std::filesystem::absolute(candidate, filesystem_error);
    if (filesystem_error) return {};
    candidate = candidate.lexically_normal();
    const auto weak = std::filesystem::weakly_canonical(candidate, filesystem_error);
    if (!filesystem_error) candidate = weak.lexically_normal();
    const auto normalized = candidate.generic_string();
    return normalized.size() > maximum_path_bytes ? std::string{} : normalized;
}

std::string StartupHubModel::display_name_for_path(const std::string_view path) {
    const std::filesystem::path candidate{std::string(path)};
    auto name = candidate.filename().string();
    if (name.empty()) name = candidate.stem().string();
    if (name.empty()) name = "Project";
    return capped(std::move(name), maximum_name_bytes);
}

std::string StartupHubModel::request_id(const StartupHubRequest& request) {
    std::uint64_t hash = 14695981039346656037ULL;
    feed_hash(hash, startup_hub_action_kind_name(request.kind));
    feed_hash(hash, request.project_path);
    feed_hash(hash, request.project_name);
    return "startup-hub." + std::string(startup_hub_action_kind_name(request.kind)) + "." + hash_hex(hash);
}

bool StartupHubModel::queue_request(StartupHubRequest request) {
    if (pending_request_) {
        last_error_ = "Finish the pending startup action before issuing another request.";
        return false;
    }
    request.request_id = request_id(request);
    pending_request_ = std::move(request);
    last_error_.clear();
    return true;
}

void StartupHubModel::sort_and_deduplicate(std::vector<StartupHubRecentProject> projects) {
    std::unordered_map<std::string, StartupHubProject> unique;
    unique.reserve(projects.size());
    for (auto& source : projects) {
        const auto normalized = normalize_path(source.path);
        if (normalized.empty()) continue;
        StartupHubProject candidate{
            .path = normalized,
            .display_name = source.display_name.empty() ? display_name_for_path(normalized)
                                                        : capped(std::move(source.display_name), maximum_name_bytes),
            .last_opened_unix_seconds = source.last_opened_unix_seconds};
        rebuild_project_status(candidate);
        const auto key = casefold_path(candidate.path);
        const auto found = unique.find(key);
        if (found == unique.end()) {
            unique.emplace(key, std::move(candidate));
            continue;
        }
        auto& existing = found->second;
        if (candidate.last_opened_unix_seconds > existing.last_opened_unix_seconds ||
            (candidate.last_opened_unix_seconds == existing.last_opened_unix_seconds &&
             lower_ascii(candidate.display_name) < lower_ascii(existing.display_name))) {
            existing = std::move(candidate);
        }
    }
    view_.recent_projects.clear();
    view_.recent_projects.reserve(std::min(options_.max_recent_projects, unique.size()));
    for (auto& [unused, project] : unique) {
        static_cast<void>(unused);
        view_.recent_projects.push_back(std::move(project));
    }
    std::ranges::sort(view_.recent_projects, [](const auto& left, const auto& right) {
        if (left.last_opened_unix_seconds != right.last_opened_unix_seconds)
            return left.last_opened_unix_seconds > right.last_opened_unix_seconds;
        const auto left_name = lower_ascii(left.display_name);
        const auto right_name = lower_ascii(right.display_name);
        if (left_name != right_name) return left_name < right_name;
        return casefold_path(left.path) < casefold_path(right.path);
    });
    if (view_.recent_projects.size() > options_.max_recent_projects)
        view_.recent_projects.resize(options_.max_recent_projects);
    view_.can_open_project = false;
    for (const auto& project : view_.recent_projects) {
        if (project.path == view_.selected_project_path &&
            project.status == StartupHubProjectStatus::available) {
            view_.can_open_project = true;
            break;
        }
    }
}

void StartupHubModel::rebuild_project_status(StartupHubProject& project) const {
    std::error_code filesystem_error;
    const std::filesystem::path path(project.path);
    project.exists = std::filesystem::exists(path, filesystem_error);
    project.directory = project.exists && std::filesystem::is_directory(path, filesystem_error);
    project.status = !project.exists ? StartupHubProjectStatus::missing
        : project.directory ? StartupHubProjectStatus::available
                             : StartupHubProjectStatus::not_directory;
}

} // namespace noemancer
