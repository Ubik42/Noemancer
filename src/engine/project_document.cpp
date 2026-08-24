#include "engine/project_document.hpp"
#include "engine/project_input_authoring.hpp"
#include "engine/semantic_ui.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <cctype>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::string_view project_schema_v1 = "noemancer.project/0.1";
constexpr std::string_view project_schema_v2 = "noemancer.project/0.2";

void add_error(ProjectLoadResult& result, std::string code, std::string path, std::string message) {
    result.errors.push_back({std::move(code), std::move(path), std::move(message)});
}

bool safe_relative_path(const std::filesystem::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name() || value.has_root_directory()) return false;
    const auto normalized = value.lexically_normal();
    return normalized != "." && normalized.begin() != normalized.end() && *normalized.begin() != "..";
}

bool safe_input_id(const std::string_view value) {
    return !value.empty()&&value.size()<=96U&&std::ranges::all_of(value,[](const unsigned char character){
        return std::isalnum(character)||character=='.'||character=='-'||character=='_';});
}

void parse_input_actions(const Json& input,ProjectDocument& project,ProjectLoadResult& result) {
    if(!input.contains("inputActions")) {project.input_actions=default_input_action_definitions();return;}
    const auto& actions=input.at("inputActions");
    if(!actions.is_array()||actions.empty()||actions.size()>64U) {
        add_error(result,"project.invalid-input-actions","/inputActions","inputActions must contain between 1 and 64 action definitions.");return;
    }
    std::unordered_set<std::string> action_ids;
    for(std::size_t action_index=0;action_index<actions.size();++action_index) {
        const auto path="/inputActions/"+std::to_string(action_index);const auto& action=actions.at(action_index);
        if(!action.is_object()) {add_error(result,"project.invalid-input-action",path,"Input action must be an object.");continue;}
        for(const auto& [field,unused]:action.items()) {static_cast<void>(unused);if(field!="id"&&field!="kind"&&field!="bindings")
            add_error(result,"project.unknown-input-action-field",path+"/"+field,"Unknown input action fields are rejected.");}
        const auto id=action.contains("id")&&action.at("id").is_string()?action.at("id").get<std::string>():std::string{};
        const auto kind=action.contains("kind")&&action.at("kind").is_string()?action.at("kind").get<std::string>():std::string{};
        if(!safe_input_id(id)||!action_ids.insert(id).second) {add_error(result,"project.invalid-input-action-id",path+"/id","Input action ID must be unique and use stable identifier characters.");continue;}
        InputActionDefinition definition{.id=id};
        if(kind=="button")definition.kind=InputActionKind::button;
        else if(kind=="axis1d")definition.kind=InputActionKind::axis_1d;
        else {add_error(result,"project.invalid-input-action-kind",path+"/kind","Input action kind must be button or axis1d.");continue;}
        if(!action.contains("bindings")||!action.at("bindings").is_array()||action.at("bindings").empty()||action.at("bindings").size()>16U) {
            add_error(result,"project.invalid-input-bindings",path+"/bindings","An input action needs between 1 and 16 bindings.");continue;
        }
        std::unordered_set<std::string> sources;
        for(std::size_t binding_index=0;binding_index<action.at("bindings").size();++binding_index) {
            const auto binding_path=path+"/bindings/"+std::to_string(binding_index);const auto& binding=action.at("bindings").at(binding_index);
            if(!binding.is_object()) {add_error(result,"project.invalid-input-binding",binding_path,"Input binding must be an object.");continue;}
            for(const auto& [field,unused]:binding.items()) {static_cast<void>(unused);if(field!="source"&&field!="scale"&&field!="deadZone")
                add_error(result,"project.unknown-input-binding-field",binding_path+"/"+field,"Unknown input binding fields are rejected.");}
            const auto source=binding.contains("source")&&binding.at("source").is_string()?binding.at("source").get<std::string>():std::string{};
            if((binding.contains("scale")&&!binding.at("scale").is_number())||
               (binding.contains("deadZone")&&!binding.at("deadZone").is_number())) {
                add_error(result,"project.invalid-input-binding",binding_path,"Binding scale and deadZone must be numbers.");continue;
            }
            const auto scale=binding.value("scale",1.0F);const auto dead_zone=binding.value("deadZone",0.0F);
            if(!safe_input_id(source)||!sources.insert(source).second||!std::isfinite(scale)||scale==0.0F||std::abs(scale)>4.0F||
               !std::isfinite(dead_zone)||dead_zone<0.0F||dead_zone>=1.0F) {
                add_error(result,"project.invalid-input-binding",binding_path,"Binding source must be unique and scale/deadZone must be finite and bounded.");continue;
            }
            definition.bindings.push_back({source,scale,dead_zone});
        }
        if(!definition.bindings.empty())project.input_actions.push_back(std::move(definition));
    }
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

} // namespace

ProjectLoadResult load_project(const std::filesystem::path& project_path) {
    ProjectLoadResult result;
    std::error_code error;
    auto manifest_path = project_path;
    if (std::filesystem::is_directory(project_path, error)) manifest_path /= "noemancer.project.json";
    const auto manifest_text = read_text(manifest_path);
    if (!manifest_text) {
        add_error(result, "project.manifest-not-found", manifest_path.generic_string(),
            "Expected a readable noemancer.project.json manifest.");
        return result;
    }

    const auto input = Json::parse(*manifest_text, nullptr, false);
    if (input.is_discarded() || !input.is_object()) {
        add_error(result, "project.invalid-json", manifest_path.generic_string(),
            "Project manifest must be a JSON object.");
        return result;
    }
    const auto manifest_schema = input.contains("schema") && input.at("schema").is_string() ?
        input.at("schema").get<std::string>() : std::string{};
    const auto is_project_v1 = manifest_schema == project_schema_v1;
    const auto is_project_v2 = manifest_schema == project_schema_v2;
    for (const auto& [field, unused] : input.items()) {
        static_cast<void>(unused);
        const auto known = field == "schema" || field == "projectId" || field == "name" ||
            field == "startupScene" || field == "assetRoots" || field == "packagedAssets" ||
            field == "scriptProject" || field == "hudDocument" || field == "inputActions";
        if (field == "hybridPixelProfile") {
            if (is_project_v2) continue;
            if (is_project_v1) {
                add_error(result, "project.hybrid-pixel-profile-schema", "/hybridPixelProfile",
                    "hybridPixelProfile requires noemancer.project/0.2 and cannot be silently accepted by a 0.1 manifest.");
                continue;
            }
        }
        if (!known) {
            add_error(result, "project.unknown-field", "/" + field,
                "Unknown project fields are rejected to prevent silent data loss.");
        }
    }
    ProjectDocument project;
    project.root = std::filesystem::absolute(manifest_path.parent_path(), error).lexically_normal();
    if (!is_project_v1 && !is_project_v2) {
        add_error(result, "project.unsupported-schema", "/schema",
            "Expected noemancer.project/0.1 or noemancer.project/0.2.");
    } else {
        // Preserve the source schema so input authoring can round-trip a
        // legacy 0.1 manifest instead of silently upgrading it in memory.
        project.schema = manifest_schema;
    }
    if (!input.contains("projectId") || !input.at("projectId").is_string() || input.at("projectId").get<std::string>().empty())
        add_error(result, "project.invalid-project-id", "/projectId", "A non-empty projectId is required.");
    else project.project_id = input.at("projectId").get<std::string>();
    if (!input.contains("name") || !input.at("name").is_string() || input.at("name").get<std::string>().empty())
        add_error(result, "project.invalid-name", "/name", "A non-empty project name is required.");
    else project.name = input.at("name").get<std::string>();
    if(input.contains("packagedAssets")) {
        if(!input.at("packagedAssets").is_array()||input.at("packagedAssets").size()>256U)
            add_error(result,"project.invalid-packaged-assets","/packagedAssets","packagedAssets must be an array of at most 256 stable Asset IDs.");
        else {std::unordered_set<std::string> ids;for(std::size_t index=0;index<input.at("packagedAssets").size();++index) {
            const auto& item=input.at("packagedAssets").at(index);
            if(!item.is_string()||item.get<std::string>().empty()||!ids.insert(item.get<std::string>()).second)
                add_error(result,"project.invalid-packaged-asset","/packagedAssets/"+std::to_string(index),
                    "Packaged Asset IDs must be non-empty and unique.");
            else project.packaged_assets.push_back(item.get<std::string>());
        }}
    }

    if (!input.contains("startupScene") || !input.at("startupScene").is_string()) {
        add_error(result, "project.invalid-startup-scene", "/startupScene", "A relative startup scene path is required.");
    } else {
        project.startup_scene = std::filesystem::path(input.at("startupScene").get<std::string>()).lexically_normal();
        if (!safe_relative_path(project.startup_scene))
            add_error(result, "project.unsafe-path", "/startupScene", "Startup scene must stay inside the project root.");
    }

    if (!input.contains("assetRoots") || !input.at("assetRoots").is_array()) {
        add_error(result, "project.invalid-asset-roots", "/assetRoots", "assetRoots must be an array of relative paths.");
    } else {
        for (std::size_t index = 0; index < input.at("assetRoots").size(); ++index) {
            const auto& item = input.at("assetRoots").at(index);
            if (!item.is_string()) {
                add_error(result, "project.invalid-asset-root", "/assetRoots/" + std::to_string(index),
                    "Asset root must be a relative string path.");
                continue;
            }
            auto root = std::filesystem::path(item.get<std::string>()).lexically_normal();
            if (!safe_relative_path(root)) {
                add_error(result, "project.unsafe-path", "/assetRoots/" + std::to_string(index),
                    "Asset root must stay inside the project root.");
                continue;
            }
            project.asset_roots.push_back(std::move(root));
        }
    }
    if (input.contains("scriptProject")) {
        if (!input.at("scriptProject").is_string()) {
            add_error(result, "project.invalid-script-project", "/scriptProject", "scriptProject must be a relative .csproj path.");
        } else {
            auto script_project = std::filesystem::path(input.at("scriptProject").get<std::string>()).lexically_normal();
            if (!safe_relative_path(script_project) || script_project.extension() != ".csproj")
                add_error(result, "project.unsafe-path", "/scriptProject", "Script project must be a relative .csproj inside the project root.");
            else project.script_project = std::move(script_project);
        }
    }
    if(input.contains("hudDocument")) {
        if(!input.at("hudDocument").is_string())add_error(result,"project.invalid-hud-document","/hudDocument",
            "hudDocument must be a project-relative Semantic UI JSON path.");
        else {
            auto hud_document=std::filesystem::path(input.at("hudDocument").get<std::string>()).lexically_normal();
            if(!safe_relative_path(hud_document)||hud_document.extension()!=".json")
                add_error(result,"project.unsafe-path","/hudDocument","HUD document must be a relative JSON file inside the project root.");
            else {
                const auto source=read_text(project.root/hud_document);
                if(!source)add_error(result,"project.hud-document-not-found","/hudDocument","HUD document could not be read.");
                else {
                    const auto validation=Json::parse(semantic_ui_validation_json(*source),nullptr,false);
                    if(!validation.is_object()||!validation.value("valid",false))
                        add_error(result,"project.invalid-hud-document","/hudDocument","HUD document must satisfy noemancer.ui-document/0.1.");
                    else {project.hud_document=std::move(hud_document);project.hud_document_json=*source;}
                }
            }
        }
    }
    if (is_project_v2 && input.contains("hybridPixelProfile")) {
        const auto parsed_profile = HybridPixelProfileCodec::parse_json(
            input.at("hybridPixelProfile").dump());
        if (!parsed_profile) {
            for (const auto& profile_error : parsed_profile.errors) {
                const auto profile_path = profile_error.path == "/" ?
                    std::string("/hybridPixelProfile") :
                    std::string("/hybridPixelProfile") + profile_error.path;
                add_error(result, "project.hybrid-pixel-profile." + profile_error.code,
                    profile_path, profile_error.message);
            }
        } else {
            project.hybrid_pixel_profile = *parsed_profile.document;
        }
    }
    parse_input_actions(input,project,result);
    if (!result.errors.empty()) return result;

    const auto scene_path = (project.root / project.startup_scene).lexically_normal();
    const auto scene_text = read_text(scene_path);
    if (!scene_text) {
        add_error(result, "project.startup-scene-not-found", scene_path.generic_string(),
            "The startup scene could not be read.");
        return result;
    }
    const auto parsed_scene = SceneDocumentCodec::parse_json(*scene_text, scene_path.generic_string());
    if (!parsed_scene) {
        for (const auto& scene_error : parsed_scene.errors)
            add_error(result, "project.startup-scene-invalid." + scene_error.code, scene_error.path, scene_error.message);
        return result;
    }
    result.project = std::move(project);
    result.startup_scene = *parsed_scene.document;
    return result;
}

std::string project_load_errors_json(const ProjectLoadResult& result) {
    Json errors = Json::array();
    for (const auto& error : result.errors)
        errors.push_back({{"code", error.code}, {"path", error.path}, {"message", error.message}});
    return Json{{"schemaVersion", "noemancer.project-load/0.1"}, {"success", static_cast<bool>(result)},
        {"errors", std::move(errors)}}.dump();
}

namespace {

constexpr std::size_t max_input_actions = 64U;
constexpr std::size_t max_input_bindings = 16U;

bool authoring_safe_id(const std::string_view value) {
    return !value.empty() && value.size() <= 96U && std::ranges::all_of(value, [](const unsigned char character) {
        return std::isalnum(character) || character == '.' || character == '-' || character == '_';
    });
}

bool authoring_valid_kind(const InputActionKind kind) {
    return kind == InputActionKind::button || kind == InputActionKind::axis_1d;
}

const char* authoring_kind_name(const InputActionKind kind) {
    if (kind == InputActionKind::button) return "button";
    if (kind == InputActionKind::axis_1d) return "axis1d";
    return "invalid";
}

void authoring_add_diagnostic(std::vector<ProjectInputDiagnostic>& diagnostics,
                              const ProjectInputDiagnosticSeverity severity,
                              std::string code, std::string path, std::string message) {
    diagnostics.push_back({severity, std::move(code), std::move(path), std::move(message)});
}

bool authoring_has_errors(const std::vector<ProjectInputDiagnostic>& diagnostics) {
    return std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == ProjectInputDiagnosticSeverity::error;
    });
}

void authoring_canonicalize(std::vector<InputActionDefinition>& definitions) {
    for (auto& action : definitions) {
        std::stable_sort(action.bindings.begin(), action.bindings.end(), [](const auto& left, const auto& right) {
            return left.source < right.source;
        });
    }
    std::stable_sort(definitions.begin(), definitions.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
}

std::vector<ProjectInputDiagnostic> authoring_validate(
    const std::vector<InputActionDefinition>& definitions) {
    std::vector<ProjectInputDiagnostic> diagnostics;
    if (definitions.empty()) {
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.empty-actions", "/actions", "A project input map must contain at least one action.");
        return diagnostics;
    }
    if (definitions.size() > max_input_actions) {
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.too-many-actions", "/actions", "A project input map may contain at most 64 actions.");
    }

    std::unordered_set<std::string> action_ids;
    std::unordered_map<std::string, std::size_t> source_owners;
    for (std::size_t action_index = 0; action_index < definitions.size(); ++action_index) {
        const auto& action = definitions[action_index];
        const auto action_path = "/actions/" + std::to_string(action_index);
        if (!authoring_safe_id(action.id)) {
            authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                "input.invalid-action-id", action_path + "/id",
                "Action ID must be 1-96 ASCII identifier characters (letters, digits, '.', '-' or '_').");
        } else if (!action_ids.insert(action.id).second) {
            authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                "input.duplicate-action-id", action_path + "/id",
                "Action IDs are stable and must be unique within the project input map.");
        }
        if (!authoring_valid_kind(action.kind)) {
            authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                "input.invalid-action-kind", action_path + "/kind",
                "Action kind must be button or axis1d.");
        }
        if (action.bindings.empty()) {
            authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                "input.empty-bindings", action_path + "/bindings",
                "Every input action must retain at least one binding.");
        } else if (action.bindings.size() > max_input_bindings) {
            authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                "input.too-many-bindings", action_path + "/bindings",
                "An input action may contain at most 16 bindings.");
        }

        std::unordered_set<std::string> action_sources;
        for (std::size_t binding_index = 0; binding_index < action.bindings.size(); ++binding_index) {
            const auto& binding = action.bindings[binding_index];
            const auto binding_path = action_path + "/bindings/" + std::to_string(binding_index);
            if (!authoring_safe_id(binding.source)) {
                authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                    "input.invalid-binding-source", binding_path + "/source",
                    "Binding source must be a stable 1-96 character identifier.");
            } else if (!action_sources.insert(binding.source).second) {
                authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                    "input.duplicate-binding-source", binding_path + "/source",
                    "A source may occur at most once in one action.");
            } else if (const auto owner = source_owners.find(binding.source); owner != source_owners.end() &&
                       owner->second != action_index) {
                authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                    "input.binding-conflict", binding_path + "/source",
                    "The stable source is already bound to another action; remap or remove the existing binding first.");
            } else {
                source_owners.emplace(binding.source, action_index);
            }
            if (!std::isfinite(binding.scale) || binding.scale == 0.0F || std::abs(binding.scale) > 4.0F) {
                authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                    "input.invalid-binding-scale", binding_path + "/scale",
                    "Binding scale must be finite, non-zero and within [-4, 4].");
            }
            if (!std::isfinite(binding.dead_zone) || binding.dead_zone < 0.0F || binding.dead_zone >= 1.0F) {
                authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
                    "input.invalid-binding-dead-zone", binding_path + "/deadZone",
                    "Binding deadZone must be finite and in [0, 1).");
            }
        }
    }
    return diagnostics;
}

nlohmann::json authoring_actions_json(const std::vector<InputActionDefinition>& source) {
    auto definitions = source;
    authoring_canonicalize(definitions);
    nlohmann::json actions = nlohmann::json::array();
    for (const auto& action : definitions) {
        nlohmann::json bindings = nlohmann::json::array();
        for (const auto& binding : action.bindings) {
            bindings.push_back({{"source", binding.source}, {"scale", binding.scale},
                {"deadZone", binding.dead_zone}});
        }
        actions.push_back({{"id", action.id}, {"kind", authoring_kind_name(action.kind)},
            {"bindings", std::move(bindings)}});
    }
    return actions;
}

ProjectInputEditResult authoring_failure(const std::string_view code, const std::string_view detail,
                                         const std::uint64_t revision,
                                         std::vector<ProjectInputDiagnostic> diagnostics = {}) {
    return {false, false, std::string(code), std::string(detail), revision, std::move(diagnostics)};
}

ProjectInputEditResult authoring_success(const std::string_view code, const std::string_view detail,
                                         const std::uint64_t revision, const bool changed,
                                         std::vector<ProjectInputDiagnostic> diagnostics = {}) {
    return {true, changed, std::string(code), std::string(detail), revision, std::move(diagnostics)};
}

ProjectInputEditResult authoring_begin_edit(const ProjectInputEditOptions& options,
                                            const std::uint64_t revision) {
    if (options.expected_revision && *options.expected_revision != revision) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.revision-conflict", "/revision",
            "The input authoring revision changed; refresh the project map before applying this edit.");
        return authoring_failure("input.revision-conflict", "Expected revision does not match the current project input revision.",
            revision, std::move(diagnostics));
    }
    return authoring_success("ok", "Edit candidate accepted for validation.", revision, false);
}

ProjectInputEditResult authoring_commit_candidate(
    std::vector<InputActionDefinition> candidate, std::vector<InputActionDefinition>& current,
    std::uint64_t& revision, const ProjectInputEditOptions& options) {
    authoring_canonicalize(candidate);
    auto diagnostics = authoring_validate(candidate);
    if (authoring_has_errors(diagnostics)) {
        return authoring_failure("input.invalid-candidate", "The input edit would violate the project input contract.",
            revision, std::move(diagnostics));
    }
    const auto before = authoring_actions_json(current).dump();
    const auto after = authoring_actions_json(candidate).dump();
    if (before == after) {
        return authoring_success("input.no-change", "The requested input edit produced no change.", revision, false,
            std::move(diagnostics));
    }
    if (options.dry_run) {
        return authoring_success("ok", "Input edit validated; dry-run did not mutate the project map.", revision, true,
            std::move(diagnostics));
    }
    current = std::move(candidate);
    ++revision;
    return authoring_success("ok", "Input edit committed to the authoring map.", revision, true,
        std::move(diagnostics));
}

bool authoring_atomic_replace(const std::filesystem::path& temporary,
                              const std::filesystem::path& destination, std::error_code& error) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return true;
#else
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

} // namespace

ProjectInputAuthoring::ProjectInputAuthoring(std::vector<InputActionDefinition> definitions)
    : actions_(std::move(definitions)) {
    authoring_canonicalize(actions_);
}

std::vector<ProjectInputDiagnostic> ProjectInputAuthoring::validate() const {
    return authoring_validate(actions_);
}

ProjectInputEditResult ProjectInputAuthoring::add_action(
    std::string id, const InputActionKind kind, std::vector<InputBinding> bindings,
    const ProjectInputEditOptions options) {
    auto result = authoring_begin_edit(options, revision_);
    if (!result.success) return result;
    auto candidate = actions_;
    candidate.push_back({std::move(id), kind, std::move(bindings)});
    return authoring_commit_candidate(std::move(candidate), actions_, revision_, options);
}

ProjectInputEditResult ProjectInputAuthoring::remove_action(
    const std::string_view id, const ProjectInputEditOptions options) {
    auto result = authoring_begin_edit(options, revision_);
    if (!result.success) return result;
    auto candidate = actions_;
    const auto found = std::find_if(candidate.begin(), candidate.end(), [&](const auto& action) {
        return action.id == id;
    });
    if (found == candidate.end()) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.action-not-found", "/actions", "The requested stable action ID does not exist.");
        return authoring_failure("input.action-not-found", "Cannot remove an unknown input action.", revision_,
            std::move(diagnostics));
    }
    candidate.erase(found);
    return authoring_commit_candidate(std::move(candidate), actions_, revision_, options);
}

ProjectInputEditResult ProjectInputAuthoring::add_binding(
    const std::string_view action_id, InputBinding binding, const ProjectInputEditOptions options) {
    auto result = authoring_begin_edit(options, revision_);
    if (!result.success) return result;
    auto candidate = actions_;
    const auto found = std::find_if(candidate.begin(), candidate.end(), [&](const auto& action) {
        return action.id == action_id;
    });
    if (found == candidate.end()) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.action-not-found", "/actions", "The requested stable action ID does not exist.");
        return authoring_failure("input.action-not-found", "Cannot add a binding to an unknown input action.", revision_,
            std::move(diagnostics));
    }
    found->bindings.push_back(std::move(binding));
    return authoring_commit_candidate(std::move(candidate), actions_, revision_, options);
}

ProjectInputEditResult ProjectInputAuthoring::remove_binding(
    const std::string_view action_id, const std::string_view source, const ProjectInputEditOptions options) {
    auto result = authoring_begin_edit(options, revision_);
    if (!result.success) return result;
    auto candidate = actions_;
    const auto action = std::find_if(candidate.begin(), candidate.end(), [&](const auto& value) {
        return value.id == action_id;
    });
    if (action == candidate.end()) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.action-not-found", "/actions", "The requested stable action ID does not exist.");
        return authoring_failure("input.action-not-found", "Cannot remove a binding from an unknown input action.", revision_,
            std::move(diagnostics));
    }
    const auto binding = std::find_if(action->bindings.begin(), action->bindings.end(), [&](const auto& value) {
        return value.source == source;
    });
    if (binding == action->bindings.end()) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.binding-not-found", "/actions", "The requested stable binding source does not exist.");
        return authoring_failure("input.binding-not-found", "Cannot remove an unknown input binding.", revision_,
            std::move(diagnostics));
    }
    if (action->bindings.size() <= 1U) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.last-binding", "/actions", "An action must retain one binding; remove the action instead.");
        return authoring_failure("input.last-binding", "Cannot remove the only binding from an input action.", revision_,
            std::move(diagnostics));
    }
    action->bindings.erase(binding);
    return authoring_commit_candidate(std::move(candidate), actions_, revision_, options);
}

ProjectInputEditResult ProjectInputAuthoring::remap_binding(
    const std::string_view action_id, const std::string_view source, InputBinding replacement,
    const ProjectInputEditOptions options) {
    auto result = authoring_begin_edit(options, revision_);
    if (!result.success) return result;
    auto candidate = actions_;
    const auto action = std::find_if(candidate.begin(), candidate.end(), [&](const auto& value) {
        return value.id == action_id;
    });
    if (action == candidate.end()) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.action-not-found", "/actions", "The requested stable action ID does not exist.");
        return authoring_failure("input.action-not-found", "Cannot remap a binding on an unknown input action.", revision_,
            std::move(diagnostics));
    }
    const auto binding = std::find_if(action->bindings.begin(), action->bindings.end(), [&](const auto& value) {
        return value.source == source;
    });
    if (binding == action->bindings.end()) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.binding-not-found", "/actions", "The requested stable binding source does not exist.");
        return authoring_failure("input.binding-not-found", "Cannot remap an unknown input binding.", revision_,
            std::move(diagnostics));
    }
    *binding = std::move(replacement);
    return authoring_commit_candidate(std::move(candidate), actions_, revision_, options);
}

std::string ProjectInputAuthoring::serialize_input_actions_json() const {
    return authoring_actions_json(actions_).dump(2);
}

std::string ProjectInputAuthoring::serialize_json() const {
    const auto actions = authoring_actions_json(actions_);
    return nlohmann::json{{"schemaVersion", project_input_authoring_schema}, {"revision", revision_},
        {"actions", actions}}.dump(2);
}

ProjectInputEditResult ProjectInputAuthoring::save_project_manifest(
    const std::filesystem::path& manifest_path, const ProjectInputEditOptions options) const {
    if (options.expected_revision && *options.expected_revision != revision_) {
        std::vector<ProjectInputDiagnostic> diagnostics;
        authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.revision-conflict", "/revision",
            "The input authoring revision changed; refresh the project map before saving.");
        return authoring_failure("input.revision-conflict", "Expected revision does not match the current project input revision.",
            revision_, std::move(diagnostics));
    }
    const auto diagnostics = validate();
    if (authoring_has_errors(diagnostics)) {
        return authoring_failure("input.invalid-document", "The project input map is invalid and cannot be saved.",
            revision_, diagnostics);
    }
    std::error_code error;
    const auto destination = std::filesystem::absolute(manifest_path, error).lexically_normal();
    if (error || !std::filesystem::is_regular_file(destination, error) || error) {
        std::vector<ProjectInputDiagnostic> save_diagnostics;
        authoring_add_diagnostic(save_diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.manifest-not-found", manifest_path.generic_string(),
            "The project manifest must already exist; authoring never creates or overwrites a new project root.");
        return authoring_failure("input.manifest-not-found", "Project manifest could not be opened for input editing.",
            revision_, std::move(save_diagnostics));
    }
    std::ifstream input(destination, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        std::vector<ProjectInputDiagnostic> save_diagnostics;
        authoring_add_diagnostic(save_diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.manifest-read-failed", destination.generic_string(),
            "The project manifest could not be read without an I/O error.");
        return authoring_failure("input.manifest-read-failed", "Project manifest read failed.", revision_,
            std::move(save_diagnostics));
    }
    input.close();
    auto document = nlohmann::json::parse(contents.str(), nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        !document.contains("schema") || !document.at("schema").is_string() ||
        (document.at("schema").get<std::string>() != project_schema_v1 &&
         document.at("schema").get<std::string>() != project_schema_v2)) {
        std::vector<ProjectInputDiagnostic> save_diagnostics;
        authoring_add_diagnostic(save_diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.manifest-invalid", "/schema",
            "Expected a noemancer.project/0.1 or noemancer.project/0.2 manifest.");
        return authoring_failure("input.manifest-invalid", "Project manifest schema is not writable by input authoring.",
            revision_, std::move(save_diagnostics));
    }
    document["inputActions"] = authoring_actions_json(actions_);
    const auto serialized = document.dump(2) + "\n";
    if (options.dry_run) {
        return authoring_success("ok", "Project input manifest validated; dry-run did not write.", revision_, false);
    }

    static std::atomic_uint64_t save_sequence{1U};
    const auto temporary = destination.parent_path() / ("." + destination.filename().string() +
        ".input-actions-" + std::to_string(save_sequence.fetch_add(1U)) + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::vector<ProjectInputDiagnostic> save_diagnostics;
            authoring_add_diagnostic(save_diagnostics, ProjectInputDiagnosticSeverity::error,
                "input.manifest-write-failed", temporary.generic_string(),
                "The sibling temporary project manifest could not be opened.");
            return authoring_failure("input.manifest-write-failed", "Project manifest temporary write failed.",
                revision_, std::move(save_diagnostics));
        }
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            std::vector<ProjectInputDiagnostic> save_diagnostics;
            authoring_add_diagnostic(save_diagnostics, ProjectInputDiagnosticSeverity::error,
                "input.manifest-write-failed", temporary.generic_string(),
                "The sibling temporary project manifest could not be flushed.");
            return authoring_failure("input.manifest-write-failed", "Project manifest temporary write failed.",
                revision_, std::move(save_diagnostics));
        }
    }
    if (!authoring_atomic_replace(temporary, destination, error)) {
        const auto replace_error = error;
        std::filesystem::remove(temporary, error);
        std::vector<ProjectInputDiagnostic> save_diagnostics;
        authoring_add_diagnostic(save_diagnostics, ProjectInputDiagnosticSeverity::error,
            "input.manifest-commit-failed", destination.generic_string(),
            "The project input manifest could not be atomically replaced.");
        return authoring_failure("input.manifest-commit-failed",
            "Project manifest atomic replacement failed (error " + std::to_string(replace_error.value()) +
                ": " + replace_error.message() + ").", revision_, std::move(save_diagnostics));
    }
    return authoring_success("ok", "Project input actions were saved with an atomic manifest replacement.", revision_, true);
}

namespace {

ProjectInputEditReceipt session_receipt(const ProjectInputEditResult& result,
                                        const std::uint64_t revision,
                                        const std::vector<InputActionDefinition>& actions,
                                        const bool persisted = false) {
    return {.success = result.success,
            .changed = result.changed,
            .persisted = persisted,
            .code = result.code,
            .detail = result.detail,
            .revision = revision,
            .actions = actions,
            .diagnostics = result.diagnostics};
}

ProjectInputEditResult session_request_failure(const std::string_view code,
                                                const std::string_view detail,
                                                const std::uint64_t revision,
                                                const std::string_view path = "/request") {
    std::vector<ProjectInputDiagnostic> diagnostics;
    authoring_add_diagnostic(diagnostics, ProjectInputDiagnosticSeverity::error,
        std::string(code), std::string(path), std::string(detail));
    return authoring_failure(code, detail, revision, std::move(diagnostics));
}

} // namespace

ProjectInputEditSession::ProjectInputEditSession(
    std::vector<InputActionDefinition> definitions, std::filesystem::path manifest_path)
    : authoring_(std::move(definitions)), manifest_path_(std::move(manifest_path)) {}

ProjectInputEditSession::ProjectInputEditSession(
    ProjectInputAuthoring authoring, std::filesystem::path manifest_path)
    : authoring_(std::move(authoring)), manifest_path_(std::move(manifest_path)) {}

ProjectInputEditReceipt ProjectInputEditSession::receipt_from_result(
    const ProjectInputEditResult& result, std::vector<InputActionDefinition> actions) const {
    if (actions.empty() && !authoring_.actions().empty()) actions = authoring_.actions();
    return session_receipt(result, authoring_.revision(), actions);
}

ProjectInputEditReceipt ProjectInputEditSession::apply_candidate(
    std::vector<InputActionDefinition> candidate, const ProjectInputEditOptions& options) {
    const auto begin = authoring_begin_edit(options, authoring_.revision());
    if (!begin.success) return receipt_from_result(begin);

    ProjectInputAuthoring candidate_authoring(std::move(candidate));
    const auto candidate_diagnostics = candidate_authoring.validate();
    if (authoring_has_errors(candidate_diagnostics)) {
        const auto invalid = authoring_failure("input.invalid-candidate",
            "The input transaction would violate the project input contract.", authoring_.revision(),
            candidate_diagnostics);
        return receipt_from_result(invalid);
    }

    const auto before = authoring_.serialize_input_actions_json();
    const auto after = candidate_authoring.serialize_input_actions_json();
    if (before == after) {
        const auto no_change = authoring_success("input.no-change",
            "The input transaction produced no change.", authoring_.revision(), false,
            candidate_diagnostics);
        return receipt_from_result(no_change);
    }

    if (options.dry_run) {
        if (!manifest_path_.empty()) {
            const auto validation = candidate_authoring.save_project_manifest(manifest_path_,
                ProjectInputEditOptions{.expected_revision = {}, .dry_run = true});
            if (!validation.success) return receipt_from_result(validation);
        }
        const auto dry_run = authoring_success("input.edit.dry-run",
            "The input transaction was validated; memory and disk were not changed.",
            authoring_.revision(), true, candidate_diagnostics);
        return session_receipt(dry_run, authoring_.revision(), candidate_authoring.actions());
    }

    if (authoring_.revision() == std::numeric_limits<std::uint64_t>::max()) {
        const auto overflow = session_request_failure("input.revision-exhausted",
            "The input authoring revision cannot advance further.", authoring_.revision(), "/revision");
        return receipt_from_result(overflow);
    }
    if (manifest_path_.empty()) {
        const auto missing_manifest = session_request_failure("input.manifest-not-found",
            "An active project manifest is required before a mutating input transaction can commit.",
            authoring_.revision(), "/manifestPath");
        return receipt_from_result(missing_manifest);
    }

    const auto saved = candidate_authoring.save_project_manifest(manifest_path_);
    if (!saved.success) return receipt_from_result(saved);

    // save_project_manifest has completed the atomic disk commit. The move
    // below is noexcept for the default vector allocator, so publishing the
    // in-memory candidate cannot leave a successful disk write unrepresented.
    authoring_.actions_ = std::move(candidate_authoring.actions_);
    ++authoring_.revision_;
    const auto committed = authoring_success("input.edit.committed",
        "The input transaction was persisted atomically and published at the new revision.",
        authoring_.revision_, true, candidate_diagnostics);
    return session_receipt(committed, authoring_.revision_, authoring_.actions(), true);
}

ProjectInputEditReceipt ProjectInputEditSession::apply(const ProjectInputEditRequest& request) {
    const ProjectInputEditOptions options{
        .expected_revision = request.expected_revision,
        .dry_run = request.dry_run};
    const auto begin = authoring_begin_edit(options, authoring_.revision());
    if (!begin.success) return receipt_from_result(begin);

    ProjectInputAuthoring candidate(authoring_.actions());
    ProjectInputEditResult operation;
    switch (request.kind) {
    case ProjectInputMutationKind::add:
        if (request.action) {
            if (!request.action_id.empty() && request.action->id != request.action_id) {
                operation = session_request_failure("input.request-invalid",
                    "The action ID and action payload ID must match.", authoring_.revision(), "/actionId");
            } else {
                operation = candidate.add_action(request.action->id, request.action->kind,
                    request.action->bindings);
            }
        } else if (request.binding) {
            if (request.action_id.empty()) {
                operation = session_request_failure("input.request-invalid",
                    "Adding a binding requires a stable action ID.", authoring_.revision(), "/actionId");
            } else {
                operation = candidate.add_binding(request.action_id, *request.binding);
            }
        } else {
            operation = session_request_failure("input.request-invalid",
                "The add mutation requires an action or binding payload.", authoring_.revision(), "/action");
        }
        break;
    case ProjectInputMutationKind::remove:
        if (request.action_id.empty()) {
            operation = session_request_failure("input.request-invalid",
                "Removing an action or binding requires a stable action ID.", authoring_.revision(), "/actionId");
        } else if (request.source.empty()) {
            operation = candidate.remove_action(request.action_id);
        } else {
            operation = candidate.remove_binding(request.action_id, request.source);
        }
        break;
    case ProjectInputMutationKind::remap:
        if (request.action_id.empty() || request.source.empty() || !request.replacement) {
            operation = session_request_failure("input.request-invalid",
                "Remapping requires action ID, source and replacement binding fields.",
                authoring_.revision(), "/replacement");
        } else {
            operation = candidate.remap_binding(request.action_id, request.source, *request.replacement);
        }
        break;
    default:
        operation = session_request_failure("input.request-kind-invalid",
            "The input mutation kind is not recognized by this Engine contract.",
            authoring_.revision(), "/kind");
        break;
    }

    if (!operation.success) return receipt_from_result(operation);
    if (!operation.changed) return receipt_from_result(operation, candidate.actions());
    return apply_candidate(std::move(candidate.actions_), options);
}

ProjectInputEditReceipt ProjectInputEditSession::replace(
    std::vector<InputActionDefinition> definitions, const ProjectInputEditOptions options) {
    return apply_candidate(std::move(definitions), options);
}

ProjectInputEditReceipt ProjectInputEditSession::publish_reload(
    std::vector<InputActionDefinition> definitions, std::filesystem::path manifest_path,
    const std::uint64_t source_revision) {
    if (source_revision == 0U) {
        const auto invalid = session_request_failure("input.revision-invalid",
            "Reload source revision must be greater than zero.", authoring_.revision(), "/revision");
        return receipt_from_result(invalid);
    }
    ProjectInputAuthoring incoming(std::move(definitions));
    const auto diagnostics = incoming.validate();
    if (authoring_has_errors(diagnostics)) {
        const auto invalid = authoring_failure("input.invalid-document",
            "The replacement input document is invalid and was not loaded.", authoring_.revision(), diagnostics);
        return receipt_from_result(invalid);
    }
    const auto unchanged = authoring_.serialize_input_actions_json() ==
        incoming.serialize_input_actions_json() && manifest_path_ == manifest_path &&
        authoring_.revision() == source_revision;
    if (unchanged) {
        const auto no_change = authoring_success("input.no-change",
            "The replacement input document is already active.", authoring_.revision(), false, diagnostics);
        return receipt_from_result(no_change);
    }

    authoring_.actions_ = std::move(incoming.actions_);
    authoring_.revision_ = source_revision;
    manifest_path_ = std::move(manifest_path);
    const auto reloaded = authoring_success("input.reload.committed",
        "The input definitions and manifest path were replaced in memory.", authoring_.revision(), true,
        diagnostics);
    return session_receipt(reloaded, authoring_.revision(), authoring_.actions());
}

ProjectInputEditReceipt ProjectInputEditSession::reload(
    std::vector<InputActionDefinition> definitions, std::filesystem::path manifest_path,
    const std::uint64_t source_revision) {
    if (manifest_path.empty() && !manifest_path_.empty()) manifest_path = manifest_path_;
    return publish_reload(std::move(definitions), std::move(manifest_path), source_revision);
}

} // namespace noemancer
