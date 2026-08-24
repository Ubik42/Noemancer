#include "engine/project_ui_authoring.hpp"

#include "engine/semantic_ui.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_source_bytes = 1024U * 1024U;
constexpr std::size_t max_node_count = 4096U;
constexpr std::size_t max_depth = 128U;
constexpr std::size_t max_id_bytes = 128U;
constexpr std::size_t history_limit = 128U;
constexpr std::size_t max_component_count = 256U;
constexpr std::size_t max_component_depth = 16U;

const std::set<std::string> runtime_document_fields{
    // Legacy 0.1 project documents authored `valid:true` and `code:"ok"`.
    // Keep accepting those harmless compatibility markers so existing
    // projects can enter the visual authoring path; richer projection output
    // remains forbidden from source persistence.
    "validation", "resources", "resourceRevision",
    "localizationDiagnostics", "themeId", "textDirection", "bindingState",
    "renderPacket", "runtime", "resolution"};
const std::set<std::string> runtime_node_fields{
    "fingerprint", "bindingState", "layout", "renderPacket", "runtime",
    "resolved", "resolvedLocale", "editableValue", "computed", "componentChain"};

void add_diagnostic(std::vector<ProjectUiDiagnostic>& diagnostics,
                    const ProjectUiDiagnosticSeverity severity,
                    std::string code, std::string path, std::string message) {
    diagnostics.push_back({severity, std::move(code), std::move(path), std::move(message)});
}

const char* severity_name(const ProjectUiDiagnosticSeverity severity) {
    return severity == ProjectUiDiagnosticSeverity::warning ? "warning" : "error";
}

Json canonicalize(const Json& value) {
    if (value.is_array()) {
        Json output = Json::array();
        for (const auto& item : value) output.push_back(canonicalize(item));
        return output;
    }
    if (!value.is_object()) return value;

    std::vector<std::string> keys;
    keys.reserve(value.size());
    for (const auto& [key, ignored] : value.items()) {
        static_cast<void>(ignored);
        keys.push_back(key);
    }
    std::ranges::sort(keys);
    Json output = Json::object();
    for (const auto& key : keys) output[key] = canonicalize(value.at(key));
    return output;
}

std::string canonical_dump(const Json& value) {
    return canonicalize(value).dump();
}

std::string fingerprint_of(const Json& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : canonical_dump(value)) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string fingerprint_of_json(const std::string_view source) {
    const auto parsed = Json::parse(source, nullptr, false);
    return parsed.is_discarded() ? std::string{} : fingerprint_of(parsed);
}

std::filesystem::path absolute_path(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

bool safe_id(const std::string_view id) {
    if (id.empty() || id.size() > max_id_bytes || id == "." || id == "..") return false;
    const auto first = static_cast<unsigned char>(id.front());
    if (!(std::isalnum(first) || first == '_' || first == ':')) return false;
    return std::ranges::all_of(id, [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return std::isalnum(byte) || value == '_' || value == '-' || value == '.' ||
            value == ':';
    });
}

bool is_null(const Json& value) {
    return value.is_null();
}

bool parse_object_value(const std::optional<std::string>& source,
                        const std::string_view path,
                        const std::string_view field,
                        Json& output,
                        std::vector<ProjectUiDiagnostic>& diagnostics) {
    if (!source) return true;
    output = Json::parse(*source, nullptr, false);
    if (output.is_discarded()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-json-value", std::string(path),
            "The " + std::string(field) + " value is not valid JSON.");
        return false;
    }
    if (is_null(output)) return true;
    if (!output.is_object()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-json-value", std::string(path),
            "The " + std::string(field) + " value must be an object or null.");
        return false;
    }
    return true;
}

bool parse_array_value(const std::optional<std::string>& source,
                       const std::string_view path,
                       const std::string_view field,
                       Json& output,
                       std::vector<ProjectUiDiagnostic>& diagnostics) {
    if (!source) return true;
    output = Json::parse(*source, nullptr, false);
    if (output.is_discarded()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-json-value", std::string(path),
            "The " + std::string(field) + " value is not valid JSON.");
        return false;
    }
    if (is_null(output)) return true;
    if (!output.is_array()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-json-value", std::string(path),
            "The " + std::string(field) + " value must be an array or null.");
        return false;
    }
    return true;
}

bool parse_any_value(const std::optional<std::string>& source,
                     const std::string_view path,
                     const std::string_view field,
                     Json& output,
                     std::vector<ProjectUiDiagnostic>& diagnostics) {
    if (!source) return true;
    output = Json::parse(*source, nullptr, false);
    if (output.is_discarded()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-json-value", std::string(path),
            "The " + std::string(field) + " value is not valid JSON.");
        return false;
    }
    return true;
}

bool contains_key(const Json& object, const std::set<std::string>& keys,
                  std::string& found) {
    if (!object.is_object()) return false;
    for (const auto& [key, ignored] : object.items()) {
        static_cast<void>(ignored);
        if (keys.contains(key)) {
            found = key;
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> node_index(const Json& document, const std::string_view id) {
    if (!document.is_object() || !document.contains("nodes") || !document.at("nodes").is_array())
        return {};
    for (std::size_t index = 0; index < document.at("nodes").size(); ++index) {
        const auto& node = document.at("nodes").at(index);
        if (node.is_object() && node.value("id", "") == id) return index;
    }
    return {};
}

std::string parent_id_of(const Json& node) {
    if (!node.is_object() || !node.contains("parentId") || node.at("parentId").is_null() ||
        !node.at("parentId").is_string()) return {};
    return node.at("parentId").get<std::string>();
}

bool is_descendant(const Json& document, const std::string_view candidate,
                  const std::string_view ancestor) {
    std::string current(candidate);
    std::unordered_set<std::string> visited;
    while (!current.empty() && visited.insert(current).second) {
        if (current == ancestor) return true;
        const auto index = node_index(document, current);
        if (!index) return false;
        current = parent_id_of(document.at("nodes").at(*index));
    }
    return false;
}

std::vector<std::string> subtree_ids(const Json& document, const std::string_view root) {
    std::vector<std::string> output;
    if (!document.is_object() || !document.contains("nodes") || !document.at("nodes").is_array()) return output;
    for (const auto& node : document.at("nodes")) {
        if (!node.is_object()) continue;
        const auto id = node.value("id", "");
        if (id == root || is_descendant(document, id, root)) output.push_back(id);
    }
    return output;
}

std::size_t subtree_end(const Json& document, const std::string_view root) {
    std::size_t end = 0U;
    bool found = false;
    if (!document.contains("nodes") || !document.at("nodes").is_array()) return end;
    for (std::size_t index = 0; index < document.at("nodes").size(); ++index) {
        const auto& node = document.at("nodes").at(index);
        if (!node.is_object()) continue;
        const auto id = node.value("id", "");
        if (id == root || is_descendant(document, id, root)) {
            end = index + 1U;
            found = true;
        }
    }
    return found ? end : 0U;
}

std::vector<std::string> direct_children(const Json& document, const std::string_view parent) {
    std::vector<std::string> output;
    if (!document.contains("nodes") || !document.at("nodes").is_array()) return output;
    for (const auto& node : document.at("nodes")) {
        if (!node.is_object()) continue;
        if (parent_id_of(node) == parent) output.push_back(node.value("id", ""));
    }
    return output;
}

std::size_t json_depth(const Json& value, const std::size_t depth = 0U) {
    if (!value.is_array() && !value.is_object()) return depth;
    std::size_t maximum = depth;
    if (value.is_array()) {
        for (const auto& item : value) maximum = std::max(maximum, json_depth(item, depth + 1U));
    } else {
        for (const auto& [key, item] : value.items()) {
            static_cast<void>(key);
            maximum = std::max(maximum, json_depth(item, depth + 1U));
        }
    }
    return maximum;
}

const Json* find_component(const Json& document, const std::string_view id) {
    if (!document.is_object() || !document.contains("components") || !document.at("components").is_array()) return nullptr;
    for (const auto& component : document.at("components")) {
        if (component.is_object() && component.value("id", "") == id) return &component;
    }
    return nullptr;
}

std::vector<ProjectUiDiagnostic> validate_components(const Json& document) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    if (!document.contains("components")) return diagnostics;
    const auto& components = document.at("components");
    if (!components.is_array()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-components", "/components", "components must be an array when present.");
        return diagnostics;
    }
    if (components.size() > max_component_count) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-limit-exceeded", "/components", "The source exceeds the reusable component limit.");
    }
    std::unordered_map<std::string, std::size_t> indexes;
    for (std::size_t index = 0U; index < components.size(); ++index) {
        const auto& component = components.at(index);
        const auto path = "/components/" + std::to_string(index);
        if (!component.is_object()) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-component", path, "Every reusable component declaration must be an object.");
            continue;
        }
        const auto id = component.value("id", "");
        if (!safe_id(id)) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.unsafe-component-id", path + "/id", "Component ids must be stable path-safe identifiers.");
        } else if (!indexes.emplace(id, index).second) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.duplicate-component-id", path + "/id", "Component ids must be unique.");
        }
        const auto role = component.value("role", "");
        if (role.empty() || role.size() > max_id_bytes) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-component-role", path + "/role", "Component role must be a bounded non-empty string.");
        }
        std::string derived;
        if (contains_key(component, runtime_node_fields, derived)) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.runtime-derived-field", path + "/" + derived,
                "Runtime-derived component fields must not be persisted in source.");
        }
        if (component.contains("componentRef")) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.component-nested-reference", path + "/componentRef",
                "Reusable declarations cannot reference component instances; use node componentRef instead.");
        }
        const auto validate_object_field = [&](const std::string_view field) {
            if (component.contains(field) && !component.at(field).is_null() && !component.at(field).is_object()) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.invalid-component-field", path + "/" + std::string(field),
                    std::string(field) + " must be an object when present.");
            } else if (component.contains(field) && json_depth(component.at(field)) > max_component_depth) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.component-depth-limit-exceeded", path + "/" + std::string(field),
                    "Reusable component layout/control data exceeds the depth limit.");
            }
        };
        for (const auto legacy_field : {std::string_view("layout"), std::string_view("control"), std::string_view("defaults")}) {
            if (component.contains(legacy_field)) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.component-legacy-field", path + "/" + std::string(legacy_field),
                    "Use the existing presentation node field; component declarations do not introduce a second control/layout schema.");
            }
        }
        validate_object_field("presentation");
        validate_object_field("binding");
        validate_object_field("state");
        if (component.contains("actions") && !component.at("actions").is_null() && !component.at("actions").is_array()) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-component-field", path + "/actions", "actions must be an array when present.");
        }
        if (component.contains("label") && (!component.at("label").is_string() || component.at("label").get<std::string>().size() > max_id_bytes)) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-component-field", path + "/label", "label must be a bounded string when present.");
        }
        if (component.contains("extends")) {
            if (!component.at("extends").is_string() || !safe_id(component.at("extends").get<std::string>())) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.invalid-component-extends", path + "/extends", "extends must be a stable component id.");
            }
        }
    }
    for (const auto& [id, index] : indexes) {
        static_cast<void>(id);
        const auto& component = components.at(index);
        if (!component.contains("extends") || !component.at("extends").is_string()) continue;
        if (!indexes.contains(component.at("extends").get<std::string>())) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.component-extends-not-found", "/components/" + std::to_string(index) + "/extends",
                "extends must name another declaration in the same document.");
        }
    }
    for (const auto& [id, index] : indexes) {
        static_cast<void>(index);
        std::unordered_set<std::string> visited;
        std::string current = id;
        std::size_t depth = 0U;
        while (!current.empty()) {
            if (!visited.insert(current).second) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.component-cycle", "/components", "Component extends links must be acyclic.");
                break;
            }
            const auto found = indexes.find(current);
            if (found == indexes.end()) break;
            const auto& component = components.at(found->second);
            if (!component.contains("extends") || !component.at("extends").is_string()) break;
            current = component.at("extends").get<std::string>();
            ++depth;
            if (depth > max_component_depth) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.component-depth-limit-exceeded", "/components", "Component extends depth exceeds the limit.");
                break;
            }
        }
    }
    return diagnostics;
}

void merge_component_fields(Json& target, const Json& source, const bool skip_identity = true) {
    if (!source.is_object()) return;
    for (const auto& [key, value] : source.items()) {
        if (skip_identity && (key == "id" || key == "extends")) continue;
        if (value.is_object() && target.contains(key) && target.at(key).is_object()) {
            merge_component_fields(target[key], value, false);
        } else {
            // Arrays (notably actions) intentionally replace the inherited
            // array as one stable declaration unit. Scalars and objects use
            // the derived value after the recursive object merge above.
            target[key] = value;
        }
    }
}

bool resolve_component(const Json& document, const std::string_view id,
                       Json& output, std::vector<std::string>& chain,
                       std::unordered_set<std::string>& visited,
                       std::vector<ProjectUiDiagnostic>& diagnostics) {
    if (!visited.insert(std::string(id)).second) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-cycle", "/components", "Component extends links must be acyclic.");
        return false;
    }
    const auto* component = find_component(document, id);
    if (!component) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-reference-not-found", "/componentRef",
            "The referenced component declaration does not exist.");
        return false;
    }
    Json inherited = Json::object();
    if (component->contains("extends") && component->at("extends").is_string()) {
        if (!resolve_component(document, component->at("extends").get<std::string>(),
                               inherited, chain, visited, diagnostics)) return false;
    }
    merge_component_fields(inherited, *component);
    output = std::move(inherited);
    chain.push_back(std::string(id));
    return true;
}

bool resolve_source_node(const Json& document, const Json& source_node,
                         Json& resolved, std::vector<std::string>& chain,
                         std::vector<ProjectUiDiagnostic>& diagnostics) {
    resolved = Json::object();
    std::unordered_set<std::string> visited;
    if (source_node.contains("componentRef")) {
        if (!source_node.at("componentRef").is_string() ||
            !resolve_component(document, source_node.at("componentRef").get<std::string>(),
                               resolved, chain, visited, diagnostics)) return false;
    }
    // The node is the final override layer.  id/parentId and all authored
    // fields are kept; componentRef remains as source provenance.
    for (const auto& [key, value] : source_node.items()) {
        if (key == "componentRef") continue;
        if (value.is_object() && resolved.contains(key) && resolved.at(key).is_object()) {
            merge_component_fields(resolved[key], value, false);
        } else {
            resolved[key] = value;
        }
    }
    if (source_node.contains("componentRef")) resolved["componentRef"] = source_node.at("componentRef");
    return diagnostics.empty();
}

std::size_t insertion_position(const Json& document, const std::string_view parent,
                               const std::size_t sibling_index) {
    const auto children = direct_children(document, parent);
    if (sibling_index > children.size()) return std::numeric_limits<std::size_t>::max();
    if (sibling_index == 0U) {
        if (parent.empty()) return 0U;
        const auto parent_index = node_index(document, parent);
        return parent_index ? *parent_index + 1U : std::numeric_limits<std::size_t>::max();
    }
    return subtree_end(document, children.at(sibling_index - 1U));
}

void refresh_roots(Json& document) {
    if (!document.contains("roots")) return;
    Json roots = Json::array();
    if (document.contains("nodes") && document.at("nodes").is_array()) {
        for (const auto& node : document.at("nodes")) {
            if (node.is_object() && parent_id_of(node).empty()) roots.push_back(node.value("id", ""));
        }
    }
    document["roots"] = std::move(roots);
}

std::vector<ProjectUiDiagnostic> validate_document(const Json& document,
                                                    const std::string_view serialized) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    // Semantic UI validates the materialized node contract.  Source nodes may
    // intentionally inherit role/presentation/state from componentRef, so
    // validating the unexpanded source would reject a valid reusable node.
    auto semantic_document = document;
    std::vector<ProjectUiDiagnostic> resolution_diagnostics;
    if (semantic_document.is_object() && semantic_document.contains("nodes") &&
        semantic_document.at("nodes").is_array()) {
        for (std::size_t index = 0U; index < semantic_document.at("nodes").size(); ++index) {
            const auto& source_node = document.at("nodes").at(index);
            if (!source_node.is_object()) continue;
            Json resolved_node;
            std::vector<std::string> chain;
            if (resolve_source_node(document, source_node, resolved_node, chain,
                                    resolution_diagnostics))
                semantic_document["nodes"][index] = std::move(resolved_node);
        }
    }
    const auto semantic = Json::parse(
        semantic_ui_validation_json(canonical_dump(semantic_document)), nullptr, false);
    if (semantic.is_discarded() || !semantic.value("valid", false)) {
        if (!semantic.is_discarded() && semantic.contains("errors") && semantic.at("errors").is_array()) {
            for (const auto& error : semantic.at("errors")) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    error.value("code", "ui.invalid-document"),
                    error.value("path", "/"), error.value("detail", "Invalid UI document."));
            }
        } else {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-document", "/", "The UI document could not be validated.");
        }
    }
    diagnostics.insert(diagnostics.end(), resolution_diagnostics.begin(), resolution_diagnostics.end());
    if (serialized.size() > max_source_bytes) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.source-too-large", "/", "The source UI document exceeds the 1 MiB authoring limit.");
    }
    if (!document.is_object()) return diagnostics;

    const auto document_id = document.value("documentId", "");
    if (!safe_id(document_id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.unsafe-document-id", "/documentId",
            "documentId must be a stable path-safe identifier.");
    }
    std::string derived;
    if (contains_key(document, runtime_document_fields, derived)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.runtime-derived-field", "/" + derived,
            "Runtime-derived UI fields must not be persisted in the source document.");
    }
    const auto component_diagnostics = validate_components(document);
    diagnostics.insert(diagnostics.end(), component_diagnostics.begin(), component_diagnostics.end());

    if (!document.contains("nodes") || !document.at("nodes").is_array()) return diagnostics;
    const auto& nodes = document.at("nodes");
    if (nodes.size() > max_node_count) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.node-limit-exceeded", "/nodes",
            "The source UI document exceeds the bounded node count.");
    }

    std::unordered_map<std::string, std::size_t> indexes;
    indexes.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes.at(index);
        const auto path = "/nodes/" + std::to_string(index);
        if (!node.is_object()) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-node", path, "Every node must be an object.");
            continue;
        }
        const auto id = node.value("id", "");
        if (!safe_id(id)) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.unsafe-node-id", path + "/id",
                "Node IDs must be stable path-safe identifiers.");
        } else if (!indexes.emplace(id, index).second) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.duplicate-node-id", path + "/id", "Node IDs must be unique.");
        }
        const auto role = semantic_document.is_object() && semantic_document.contains("nodes") &&
            semantic_document.at("nodes").is_array() && index < semantic_document.at("nodes").size() &&
            semantic_document.at("nodes").at(index).is_object()
            ? semantic_document.at("nodes").at(index).value("role", "") : node.value("role", "");
        if (role.empty() || role.size() > max_id_bytes) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-node-role", path + "/role", "Node role must be a bounded non-empty string.");
        }
        if (contains_key(node, runtime_node_fields, derived)) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.runtime-derived-field", path + "/" + derived,
                "Runtime-derived node fields must not be persisted in the source document.");
        }
        if (node.contains("binding") && !node.at("binding").is_null() && !node.at("binding").is_object()) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-binding", path + "/binding", "binding must be an object or null.");
        }
        if (node.contains("state") && !node.at("state").is_null() && !node.at("state").is_object()) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-state", path + "/state", "state must be an object or null.");
        }
        if (node.contains("actions") && !node.at("actions").is_null() && !node.at("actions").is_array()) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-actions", path + "/actions", "actions must be an array or null.");
        }
        if (node.contains("componentRef")) {
            if (!node.at("componentRef").is_string() || node.at("componentRef").get<std::string>().empty()) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.invalid-component-reference", path + "/componentRef",
                    "componentRef must be a non-empty component id.");
            } else if (!find_component(document, node.at("componentRef").get<std::string>())) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.component-reference-not-found", path + "/componentRef",
                    "componentRef must name a declaration in the same document.");
            }
        }
    }

    std::vector<std::string> roots;
    for (const auto& [id, index] : indexes) {
        static_cast<void>(index);
        const auto& node = nodes.at(indexes.at(id));
        if (parent_id_of(node).empty()) roots.push_back(id);
        else if (!indexes.contains(parent_id_of(node))) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.parent-not-found", "/nodes/" + std::to_string(index) + "/parentId",
                "Every parentId must name a node in the same document.");
        }
    }
    std::ranges::sort(roots);
    if (roots.size() != 1U) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.root-count", "/nodes",
            "A project UI source document must contain exactly one root node.");
    }
    if (document.contains("roots")) {
        if (!document.at("roots").is_array()) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.invalid-roots", "/roots", "roots must be an array when present.");
        } else {
            std::vector<std::string> declared;
            for (const auto& root : document.at("roots")) {
                if (!root.is_string()) {
                    add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                        "ui.invalid-roots", "/roots", "roots entries must be strings.");
                    continue;
                }
                declared.push_back(root.get<std::string>());
            }
            std::ranges::sort(declared);
            if (declared != roots) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.roots-mismatch", "/roots", "roots must match the document's computed root node.");
            }
        }
    }

    for (const auto& [id, index] : indexes) {
        static_cast<void>(index);
        std::unordered_set<std::string> visited;
        std::string current = id;
        std::size_t depth = 0U;
        while (!current.empty()) {
            if (!visited.insert(current).second) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.parent-cycle", "/nodes", "Node parent links must form an acyclic tree.");
                break;
            }
            const auto parent_index = indexes.find(current);
            if (parent_index == indexes.end()) break;
            current = parent_id_of(nodes.at(parent_index->second));
            ++depth;
            if (depth > max_depth) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.depth-limit-exceeded", "/nodes", "The source UI tree exceeds the depth limit.");
                break;
            }
        }
    }
    return diagnostics;
}

Json parse_document(const std::string_view source, std::vector<ProjectUiDiagnostic>& diagnostics) {
    const auto parsed = Json::parse(source, nullptr, false);
    if (parsed.is_discarded()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-json", "/", "The source UI document is not valid JSON.");
        return Json::object();
    }
    if (!parsed.is_object()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-document", "/", "The source UI document must be a JSON object.");
        return Json::object();
    }
    return parsed;
}

std::string read_file(const std::filesystem::path& path, std::vector<ProjectUiDiagnostic>& diagnostics) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.document-not-found", path.generic_string(),
            "The source UI document could not be opened.");
        return {};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.document-read-failed", path.generic_string(),
            "The source UI document could not be read without an I/O error.");
        return {};
    }
    return contents.str();
}

bool atomic_replace(const std::filesystem::path& temporary,
                    const std::filesystem::path& destination, std::error_code& error) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return true;
#else
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

bool persist_candidate(const std::filesystem::path& path, const std::string_view expected_fingerprint,
                       const Json& candidate, std::vector<ProjectUiDiagnostic>& diagnostics) {
    std::error_code error;
    const auto absolute = absolute_path(path);
    if (absolute.empty() || !std::filesystem::is_regular_file(absolute, error) || error) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.document-not-found", path.generic_string(),
            "A regular source UI document is required before a mutating edit can commit.");
        return false;
    }
    const auto current_source = read_file(absolute, diagnostics);
    if (!diagnostics.empty()) return false;
    const auto current = Json::parse(current_source, nullptr, false);
    if (current.is_discarded() || !current.is_object()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.persistence-conflict", absolute.generic_string(),
            "The source UI document changed externally and is no longer valid JSON.");
        return false;
    }
    if (fingerprint_of(current) != expected_fingerprint) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.persistence-conflict", absolute.generic_string(),
            "The source UI document changed externally; refresh before committing this edit.");
        return false;
    }

    static std::atomic_uint64_t write_sequence{1U};
    const auto temporary = absolute.parent_path() / ("." + absolute.filename().string() +
        ".ui-authoring-" + std::to_string(write_sequence.fetch_add(1U)) + ".tmp");
    const auto serialized = canonical_dump(candidate) + "\n";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.document-write-failed", temporary.generic_string(),
                "The sibling temporary UI document could not be opened.");
            return false;
        }
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code cleanup;
            std::filesystem::remove(temporary, cleanup);
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.document-write-failed", temporary.generic_string(),
                "The sibling temporary UI document could not be flushed.");
            return false;
        }
    }
    std::error_code replace_error;
    if (!atomic_replace(temporary, absolute, replace_error)) {
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.document-commit-failed", absolute.generic_string(),
            "The source UI document could not be atomically replaced (error " +
                std::to_string(replace_error.value()) + ": " + replace_error.message() + ").");
        return false;
    }
    return true;
}

Json diagnostics_json(const std::vector<ProjectUiDiagnostic>& diagnostics) {
    Json output = Json::array();
    for (const auto& diagnostic : diagnostics) {
        output.push_back({{"severity", severity_name(diagnostic.severity)},
            {"code", diagnostic.code}, {"path", diagnostic.path}, {"message", diagnostic.message}});
    }
    return output;
}

std::string observation_for(const std::string_view source_json,
                            const std::filesystem::path& path,
                            const std::uint64_t revision,
                            const std::string_view fingerprint,
                            const bool can_undo, const bool can_redo) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    const auto document = parse_document(source_json, diagnostics);
    const auto serialized = document.is_object() ? canonical_dump(document) : std::string{};
    auto validation = validate_document(document, serialized);
    diagnostics.insert(diagnostics.end(), validation.begin(), validation.end());
    Json roots = Json::array();
    std::size_t node_count = 0U;
    if (document.is_object() && document.contains("nodes") && document.at("nodes").is_array()) {
        node_count = document.at("nodes").size();
        for (const auto& node : document.at("nodes")) {
            if (node.is_object() && parent_id_of(node).empty()) roots.push_back(node.value("id", ""));
        }
    }
    return Json{{"schemaVersion", project_ui_authoring_schema}, {"valid", diagnostics.empty()},
        {"code", diagnostics.empty() ? "ok" : "ui.invalid-document"}, {"revision", revision},
        {"fingerprint", fingerprint.empty() ? fingerprint_of(document) : std::string(fingerprint)},
        {"documentPath", path.generic_string()}, {"sourceBytes", serialized.size()},
        {"nodeCount", node_count}, {"roots", std::move(roots)}, {"canUndo", can_undo},
        {"canRedo", can_redo}, {"diagnostics", diagnostics_json(diagnostics)},
        {"document", document}}.dump();
}

ProjectUiEditReceipt make_receipt(const bool success, const bool changed, const bool persisted,
                                  std::string operation, std::string code, std::string detail,
                                  const std::uint64_t revision, std::string fingerprint,
                                  std::string candidate_fingerprint, std::string document_json,
                                  std::string observation_json, const bool can_undo,
                                  const bool can_redo, std::vector<ProjectUiDiagnostic> diagnostics) {
    return {success, changed, persisted, std::move(operation), std::move(code), std::move(detail),
        revision, std::move(fingerprint), std::move(candidate_fingerprint), std::move(document_json),
        std::move(observation_json), can_undo, can_redo, std::move(diagnostics)};
}

} // namespace

std::string project_ui_resolved_document_json(const std::string_view source_json) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    const auto document = parse_document(source_json, diagnostics);
    if (diagnostics.empty()) {
        const auto validation = validate_document(document, canonical_dump(document));
        diagnostics.insert(diagnostics.end(), validation.begin(), validation.end());
    }
    if (!diagnostics.empty()) {
        return Json{{"schemaVersion", "noemancer.ui-document/0.1"}, {"valid", false},
            {"code", "ui.invalid-document"}, {"documentId", "ui.invalid"},
            {"nodes", Json::array()}, {"diagnostics", diagnostics_json(diagnostics)}}.dump();
    }

    auto resolved_document = document;
    for (std::size_t index = 0U; index < document.at("nodes").size(); ++index) {
        Json resolved_node;
        std::vector<std::string> chain;
        if (!resolve_source_node(document, document.at("nodes").at(index),
                                 resolved_node, chain, diagnostics)) {
            return Json{{"schemaVersion", "noemancer.ui-document/0.1"}, {"valid", false},
                {"code", "ui.component-resolution-failed"},
                {"documentId", document.value("documentId", "ui.invalid")},
                {"nodes", Json::array()}, {"diagnostics", diagnostics_json(diagnostics)}}.dump();
        }
        if (!chain.empty()) resolved_node["componentChain"] = std::move(chain);
        resolved_document["nodes"].at(index) = std::move(resolved_node);
    }
    resolved_document["schemaVersion"] = "noemancer.ui-document/0.1";
    resolved_document["valid"] = true;
    resolved_document["code"] = "ok";
    resolved_document["resolution"] = {
        {"schemaVersion", "noemancer.ui-resolution/0.1"},
        {"sourceFingerprint", fingerprint_of(document)},
        {"merge", "base-to-derived-recursive-object-array-replace-node-overrides"},
        {"componentRefProvenance", true}
    };
    return canonical_dump(resolved_document);
}

std::string ProjectUiEditReceipt::to_json() const {
    const auto document = nlohmann::json::parse(document_json, nullptr, false);
    const auto observation = nlohmann::json::parse(observation_json, nullptr, false);
    return nlohmann::json{{"schemaVersion", project_ui_authoring_schema}, {"success", success},
        {"changed", changed}, {"persisted", persisted}, {"operation", operation},
        {"code", code}, {"detail", detail}, {"revision", revision}, {"fingerprint", fingerprint},
        {"candidateFingerprint", candidate_fingerprint},
        {"document", document.is_discarded() ? nlohmann::json(nullptr) : document},
        {"observation", observation.is_discarded() ? nlohmann::json(nullptr) : observation},
        {"canUndo", can_undo}, {"canRedo", can_redo}, {"diagnostics", diagnostics_json(diagnostics)}}.dump();
}

ProjectUiAuthoringSession::ProjectUiAuthoringSession(std::string source_json,
                                                     std::filesystem::path document_path,
                                                     const std::uint64_t revision)
    : document_path_(absolute_path(document_path)), revision_(revision == 0U ? 1U : revision) {
    std::vector<ProjectUiDiagnostic> parse_diagnostics;
    const auto document = parse_document(source_json, parse_diagnostics);
    if (!parse_diagnostics.empty()) {
        diagnostics_ = std::move(parse_diagnostics);
        source_json_ = std::move(source_json);
        fingerprint_ = fingerprint_of_json(source_json_);
        return;
    }
    source_json_ = canonical_dump(document);
    // A source revision is part of the authored UI document's CAS identity.
    // The caller may still provide an explicit non-default revision when
    // attaching to a live authority, but a normal file load should resume at
    // the revision that was actually persisted.
    if (revision == 1U && document.contains("revision") &&
        document.at("revision").is_number_unsigned() && document.at("revision").get<std::uint64_t>() > 0U) {
        revision_ = document.at("revision").get<std::uint64_t>();
    }
    fingerprint_ = fingerprint_of(document);
    diagnostics_ = validate_document(document, source_json_);
}

ProjectUiAuthoringSession ProjectUiAuthoringSession::from_file(
    std::filesystem::path document_path, const std::uint64_t revision) {
    const auto path = absolute_path(document_path);
    std::vector<ProjectUiDiagnostic> diagnostics;
    const auto source = read_file(path, diagnostics);
    if (!diagnostics.empty()) {
        ProjectUiAuthoringSession result("{}", path, revision);
        result.diagnostics_ = std::move(diagnostics);
        return result;
    }
    return ProjectUiAuthoringSession(std::move(source), path, revision);
}

ProjectUiAuthoringSession ProjectUiAuthoringSession::from_json(
    std::string source_json, std::filesystem::path document_path,
    const std::uint64_t revision) {
    return ProjectUiAuthoringSession(std::move(source_json), std::move(document_path), revision);
}

std::vector<ProjectUiDiagnostic> ProjectUiAuthoringSession::validate() const {
    std::vector<ProjectUiDiagnostic> diagnostics;
    const auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return diagnostics;
    auto validation = validate_document(document, canonical_dump(document));
    return validation;
}

std::string ProjectUiAuthoringSession::observation_json() const {
    return observation_for(source_json_, document_path_, revision_, fingerprint_, can_undo(), can_redo());
}

std::string ProjectUiAuthoringSession::resolved_node_json(const std::string_view node_id) const {
    std::vector<ProjectUiDiagnostic> diagnostics;
    const auto document = parse_document(source_json_, diagnostics);
    if (diagnostics.empty()) {
        const auto source_validation = validate_document(document, canonical_dump(document));
        diagnostics.insert(diagnostics.end(), source_validation.begin(), source_validation.end());
    }
    if (!diagnostics.empty()) {
        return Json{{"schemaVersion", "noemancer.ui-node-resolution/0.1"}, {"valid", false},
            {"code", "ui.invalid-document"}, {"nodeId", std::string(node_id)},
            {"sourceRevision", revision_}, {"diagnostics", diagnostics_json(diagnostics)}}.dump();
    }
    const auto index = node_index(document, node_id);
    if (!index) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.node-not-found", "/nodeId", "The requested source node does not exist.");
        return Json{{"schemaVersion", "noemancer.ui-node-resolution/0.1"}, {"valid", false},
            {"code", "ui.node-not-found"}, {"nodeId", std::string(node_id)},
            {"sourceRevision", revision_}, {"diagnostics", diagnostics_json(diagnostics)}}.dump();
    }
    const auto& source_node = document.at("nodes").at(*index);
    std::vector<std::string> chain;
    Json resolved;
    if (!resolve_source_node(document, source_node, resolved, chain, diagnostics)) {
        return Json{{"schemaVersion", "noemancer.ui-node-resolution/0.1"}, {"valid", false},
            {"code", "ui.component-resolution-failed"}, {"nodeId", std::string(node_id)},
            {"sourceRevision", revision_}, {"diagnostics", diagnostics_json(diagnostics)}}.dump();
    }
    return Json{{"schemaVersion", "noemancer.ui-node-resolution/0.1"}, {"valid", true},
        {"code", "ok"}, {"nodeId", std::string(node_id)}, {"sourceRevision", revision_},
        {"componentChain", std::move(chain)}, {"node", canonicalize(resolved)},
        {"diagnostics", Json::array()}}.dump();
}

ProjectUiEditReceipt ProjectUiAuthoringSession::failure(
    const std::string_view operation, const std::string_view code,
    const std::string_view detail, std::vector<ProjectUiDiagnostic> diagnostics) const {
    return make_receipt(false, false, false, std::string(operation), std::string(code),
        std::string(detail), revision_, fingerprint_, {}, source_json_, observation_json(),
        can_undo(), can_redo(), std::move(diagnostics));
}

ProjectUiEditReceipt ProjectUiAuthoringSession::invalid_request(
    const std::string_view operation, const std::string_view code,
    const std::string_view detail, std::vector<ProjectUiDiagnostic> diagnostics) const {
    return failure(operation, code, detail, std::move(diagnostics));
}

ProjectUiEditReceipt ProjectUiAuthoringSession::success(
    const std::string_view operation, const std::string_view code,
    const std::string_view detail, const bool changed, const bool persisted,
    const std::uint64_t revision, std::string candidate_json,
    std::string candidate_fingerprint) const {
    return make_receipt(true, changed, persisted, std::string(operation), std::string(code),
        std::string(detail), revision, fingerprint_, std::move(candidate_fingerprint),
        std::move(candidate_json), observation_json(), can_undo(), can_redo(), {});
}

ProjectUiEditReceipt ProjectUiAuthoringSession::commit_candidate(
    std::string candidate_json, const ProjectUiEditOptions& options,
    const std::string_view operation, const HistoryDirection direction,
    std::optional<HistoryEntry> history_entry) {
    if (options.expected_revision && *options.expected_revision != revision_) {
        std::vector<ProjectUiDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.revision-conflict", "/revision",
            "Expected revision does not match the current Project UI authoring revision.");
        return failure(operation, "ui.revision-conflict",
            "Refresh the Project UI observation before applying this edit.", std::move(diagnostics));
    }
    std::vector<ProjectUiDiagnostic> parse_diagnostics;
    auto candidate = parse_document(candidate_json, parse_diagnostics);
    if (!parse_diagnostics.empty()) return invalid_request(operation, "ui.invalid-candidate",
        "The candidate Project UI document is not valid JSON.", std::move(parse_diagnostics));
    candidate_json = canonical_dump(candidate);
    auto diagnostics = validate_document(candidate, candidate_json);
    if (!diagnostics.empty()) return invalid_request(operation, "ui.invalid-candidate",
        "The candidate Project UI document violates the source authoring contract.", std::move(diagnostics));
    const auto pre_revision_fingerprint = fingerprint_of(candidate);
    if (pre_revision_fingerprint == fingerprint_) {
        return success(operation, "ui.no-change", "The requested Project UI edit produced no change.",
            false, false, revision_, source_json_, fingerprint_);
    }
    // Every published mutation receives the next monotonic source revision.
    // This is deliberately applied after the no-change check so an idempotent
    // request does not manufacture a new revision.  Dry-run exposes the
    // candidate revision in its receipt but never changes session memory/disk.
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        std::vector<ProjectUiDiagnostic> exhausted;
        add_diagnostic(exhausted, ProjectUiDiagnosticSeverity::error,
            "ui.revision-exhausted", "/revision",
            "The Project UI authoring revision cannot advance further.");
        return failure(operation, "ui.revision-exhausted",
            "The Project UI authoring revision cannot advance further.", std::move(exhausted));
    }
    candidate["revision"] = revision_ + 1U;
    candidate_json = canonical_dump(candidate);
    diagnostics = validate_document(candidate, candidate_json);
    if (!diagnostics.empty()) return invalid_request(operation, "ui.invalid-candidate",
        "The candidate Project UI document violates the source authoring contract.", std::move(diagnostics));
    const auto candidate_fingerprint = fingerprint_of(candidate);
    if (options.dry_run) {
        if (!document_path_.empty()) {
            std::vector<ProjectUiDiagnostic> persistence_diagnostics;
            const auto existing = read_file(document_path_, persistence_diagnostics);
            if (!persistence_diagnostics.empty()) return failure(operation,
                persistence_diagnostics.front().code, persistence_diagnostics.front().message,
                std::move(persistence_diagnostics));
            const auto existing_document = Json::parse(existing, nullptr, false);
            if (existing_document.is_discarded() || fingerprint_of(existing_document) != fingerprint_) {
                add_diagnostic(persistence_diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.persistence-conflict", document_path_.generic_string(),
                    "The source UI document changed externally; refresh before dry-run.");
                return failure(operation, "ui.persistence-conflict",
                    "The source UI document changed externally; refresh before dry-run.",
                    std::move(persistence_diagnostics));
            }
        }
        return success(operation, "ui.edit.dry-run",
            "The Project UI edit was validated; memory and disk were not changed.",
            true, false, revision_, std::move(candidate_json), candidate_fingerprint);
    }
    if (document_path_.empty()) {
        std::vector<ProjectUiDiagnostic> missing;
        add_diagnostic(missing, ProjectUiDiagnosticSeverity::error,
            "ui.document-path-required", "/documentPath",
            "A source UI document path is required before a mutating edit can commit.");
        return failure(operation, "ui.document-path-required",
            "An existing source UI document is required before commit.", std::move(missing));
    }
    std::vector<ProjectUiDiagnostic> persistence_diagnostics;
    if (!persist_candidate(document_path_, fingerprint_, candidate, persistence_diagnostics)) {
        const auto code = persistence_diagnostics.empty() ? "ui.document-commit-failed" :
            persistence_diagnostics.front().code;
        const auto detail = persistence_diagnostics.empty() ? "The Project UI document could not be persisted." :
            persistence_diagnostics.front().message;
        return failure(operation, code, detail, std::move(persistence_diagnostics));
    }

    const HistoryEntry entry = history_entry.value_or(HistoryEntry{source_json_, candidate_json});
    source_json_ = std::move(candidate_json);
    fingerprint_ = candidate_fingerprint;
    ++revision_;
    switch (direction) {
    case HistoryDirection::edit:
        undo_.push_back(entry);
        redo_.clear();
        break;
    case HistoryDirection::undo:
        if (!undo_.empty()) undo_.pop_back();
        redo_.push_back(entry);
        break;
    case HistoryDirection::redo:
        if (!redo_.empty()) redo_.pop_back();
        undo_.push_back(entry);
        break;
    }
    const auto trim = [](auto& history) {
        if (history.size() > history_limit) {
            history.erase(history.begin(), history.begin() +
                static_cast<typename std::remove_reference_t<decltype(history)>::difference_type>(
                    history.size() - history_limit));
        }
    };
    trim(undo_);
    trim(redo_);
    return success(operation, "ui.edit.committed",
        "The Project UI document was atomically persisted and published at the new revision.",
        true, true, revision_, source_json_, fingerprint_);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::add_node(
    ProjectUiAddNodeRequest request, const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    if (!safe_id(request.id)) add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
        "ui.unsafe-node-id", "/request/id", "Node id must be a stable path-safe identifier.");
    if (request.role.empty() || request.role.size() > max_id_bytes) add_diagnostic(diagnostics,
        ProjectUiDiagnosticSeverity::error, "ui.invalid-node-role", "/request/role",
        "Node role must be a bounded non-empty string.");
    Json binding, actions, state, presentation, value;
    const bool parsed = parse_object_value(request.binding_json, "/request/binding", "binding",
        binding, diagnostics) && parse_array_value(request.actions_json, "/request/actions", "actions",
        actions, diagnostics) && parse_object_value(request.state_json, "/request/state", "state",
        state, diagnostics) && parse_object_value(request.presentation_json, "/request/presentation",
        "presentation", presentation, diagnostics) && parse_any_value(request.value_json, "/request/value",
        "value", value, diagnostics);
    if (!request.parent_id.empty() && !safe_id(request.parent_id)) add_diagnostic(diagnostics,
        ProjectUiDiagnosticSeverity::error, "ui.unsafe-parent-id", "/request/parentId",
        "parentId must be a stable path-safe identifier.");
    if (request.component_ref && !safe_id(*request.component_ref)) add_diagnostic(diagnostics,
        ProjectUiDiagnosticSeverity::error, "ui.unsafe-component-id", "/request/componentRef",
        "componentRef must be a stable path-safe identifier.");
    if (!parsed || !diagnostics.empty()) return invalid_request("add_node", "ui.invalid-request",
        "The add_node request is invalid.", std::move(diagnostics));

    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("add_node", "ui.invalid-source", "The current source is invalid.", std::move(diagnostics));
    if (node_index(document, request.id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.duplicate-node-id",
            "/request/id", "A node with this id already exists.");
        return invalid_request("add_node", "ui.duplicate-node-id", "Node id already exists.", std::move(diagnostics));
    }
    if (!request.parent_id.empty() && !node_index(document, request.parent_id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.parent-not-found",
            "/request/parentId", "The requested parent node does not exist.");
        return invalid_request("add_node", "ui.parent-not-found", "The requested parent node does not exist.", std::move(diagnostics));
    }
    if (request.component_ref && !find_component(document, *request.component_ref)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.component-reference-not-found",
            "/request/componentRef", "componentRef must name an existing reusable declaration.");
        return invalid_request("add_node", "ui.component-reference-not-found",
            "The requested component declaration does not exist.", std::move(diagnostics));
    }
    const auto sibling_index = request.sibling_index.value_or(
        direct_children(document, request.parent_id).size());
    const auto insertion = insertion_position(document, request.parent_id, sibling_index);
    if (insertion == std::numeric_limits<std::size_t>::max()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.invalid-sibling-index",
            "/request/siblingIndex", "The requested sibling index is outside the parent range.");
        return invalid_request("add_node", "ui.invalid-sibling-index", "Invalid sibling index.", std::move(diagnostics));
    }
    Json node{{"id", request.id}, {"parentId", request.parent_id.empty() ? Json(nullptr) : Json(request.parent_id)},
        {"role", request.role}, {"label", request.label}, {"state", state.is_null() ? Json::object({{"visible", true}, {"enabled", true}}) : state},
        {"actions", actions.is_null() ? Json::array() : actions}};
    if (request.binding_json) { if (binding.is_null()) {} else node["binding"] = binding; }
    if (request.presentation_json) { if (!presentation.is_null()) node["presentation"] = presentation; }
    if (request.value_json) { if (!value.is_null()) node["value"] = value; }
    if (request.component_ref) node["componentRef"] = *request.component_ref;
    document["nodes"].insert(document["nodes"].begin() + static_cast<Json::difference_type>(insertion), std::move(node));
    refresh_roots(document);
    return commit_candidate(canonical_dump(document), options, "add_node", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::remove_subtree(
    const std::string_view node_id, const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("remove_subtree", "ui.invalid-source", "The current source is invalid.", std::move(diagnostics));
    const auto index = node_index(document, node_id);
    if (!index) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.node-not-found",
            "/request/nodeId", "The requested node does not exist.");
        return invalid_request("remove_subtree", "ui.node-not-found", "The requested node does not exist.", std::move(diagnostics));
    }
    if (parent_id_of(document.at("nodes").at(*index)).empty()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.root-removal-forbidden",
            "/request/nodeId", "The single project UI root cannot be removed without a replacement.");
        return invalid_request("remove_subtree", "ui.root-removal-forbidden", "The project UI root cannot be removed.", std::move(diagnostics));
    }
    const auto removed = subtree_ids(document, node_id);
    const std::unordered_set<std::string> removed_set(removed.begin(), removed.end());
    Json nodes = Json::array();
    for (const auto& node : document["nodes"]) {
        if (!node.is_object() || !removed_set.contains(node.value("id", ""))) nodes.push_back(node);
    }
    document["nodes"] = std::move(nodes);
    refresh_roots(document);
    return commit_candidate(canonical_dump(document), options, "remove_subtree", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::update_node(
    ProjectUiUpdateNodeRequest request, const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    if (!safe_id(request.node_id)) add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
        "ui.unsafe-node-id", "/request/nodeId", "Node id must be a stable path-safe identifier.");
    if (request.role && (request.role->empty() || request.role->size() > max_id_bytes)) add_diagnostic(diagnostics,
        ProjectUiDiagnosticSeverity::error, "ui.invalid-node-role", "/request/role",
        "Node role must be a bounded non-empty string.");
    if (request.component_ref && *request.component_ref != "null" && !safe_id(*request.component_ref)) add_diagnostic(diagnostics,
        ProjectUiDiagnosticSeverity::error, "ui.unsafe-component-id", "/request/componentRef",
        "componentRef must be a stable path-safe identifier or null.");
    Json binding, actions, state, presentation, value;
    const bool parsed = parse_object_value(request.binding_json, "/request/binding", "binding", binding, diagnostics) &&
        parse_array_value(request.actions_json, "/request/actions", "actions", actions, diagnostics) &&
        parse_object_value(request.state_json, "/request/state", "state", state, diagnostics) &&
        parse_object_value(request.presentation_json, "/request/presentation", "presentation", presentation, diagnostics) &&
        parse_any_value(request.value_json, "/request/value", "value", value, diagnostics);
    if (!parsed || !diagnostics.empty()) return invalid_request("update_node", "ui.invalid-request",
        "The update_node request is invalid.", std::move(diagnostics));
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("update_node", "ui.invalid-source", "The current source is invalid.", std::move(diagnostics));
    const auto index = node_index(document, request.node_id);
    if (!index) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.node-not-found",
            "/request/nodeId", "The requested node does not exist.");
        return invalid_request("update_node", "ui.node-not-found", "The requested node does not exist.", std::move(diagnostics));
    }
    if (request.component_ref && *request.component_ref != "null" &&
        !find_component(document, *request.component_ref)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.component-reference-not-found",
            "/request/componentRef", "componentRef must name an existing reusable declaration.");
        return invalid_request("update_node", "ui.component-reference-not-found",
            "The requested component declaration does not exist.", std::move(diagnostics));
    }
    auto& node = document["nodes"].at(*index);
    if (request.label) node["label"] = *request.label;
    if (request.role) node["role"] = *request.role;
    if (request.binding_json) { if (binding.is_null()) node.erase("binding"); else node["binding"] = binding; }
    if (request.actions_json) { if (actions.is_null()) node.erase("actions"); else node["actions"] = actions; }
    if (request.state_json) { if (state.is_null()) node.erase("state"); else node["state"] = state; }
    if (request.presentation_json) { if (presentation.is_null()) node.erase("presentation"); else node["presentation"] = presentation; }
    if (request.value_json) { if (value.is_null()) node.erase("value"); else node["value"] = value; }
    if (request.component_ref) {
        if (*request.component_ref == "null") node.erase("componentRef");
        else node["componentRef"] = *request.component_ref;
    }
    return commit_candidate(canonical_dump(document), options, "update_node", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::reparent(
    const std::string_view node_id, const std::string_view parent_id,
    const ProjectUiEditOptions options) {
    return reparent(node_id, parent_id, std::numeric_limits<std::size_t>::max(), options);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::reparent(
    const std::string_view node_id, const std::string_view parent_id,
    const std::size_t sibling_index, const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("reparent", "ui.invalid-source", "The current source is invalid.", std::move(diagnostics));
    const auto index = node_index(document, node_id);
    if (!index) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.node-not-found", "/request/nodeId", "The requested node does not exist.");
        return invalid_request("reparent", "ui.node-not-found", "The requested node does not exist.", std::move(diagnostics));
    }
    if (parent_id.empty()) {
        if (parent_id_of(document.at("nodes").at(*index)).empty() && sibling_index == std::numeric_limits<std::size_t>::max())
            return commit_candidate(canonical_dump(document), options, "reparent", HistoryDirection::edit);
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.root-count", "/request/parentId", "A project UI source must keep exactly one root.");
        return invalid_request("reparent", "ui.root-count", "A node cannot become a second root.", std::move(diagnostics));
    }
    const auto parent_index = node_index(document, parent_id);
    if (!parent_index) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.parent-not-found", "/request/parentId", "The requested parent node does not exist.");
        return invalid_request("reparent", "ui.parent-not-found", "The requested parent node does not exist.", std::move(diagnostics));
    }
    if (node_id == parent_id || is_descendant(document, parent_id, node_id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.parent-cycle", "/request/parentId", "A node cannot be reparented beneath itself or its subtree.");
        return invalid_request("reparent", "ui.parent-cycle", "The requested reparent would create a cycle.", std::move(diagnostics));
    }
    const auto old_parent = parent_id_of(document.at("nodes").at(*index));
    const auto has_index = sibling_index != std::numeric_limits<std::size_t>::max();
    if (old_parent == parent_id && !has_index) return commit_candidate(canonical_dump(document), options, "reparent", HistoryDirection::edit);
    const auto moving_ids = subtree_ids(document, node_id);
    const std::unordered_set<std::string> moving_set(moving_ids.begin(), moving_ids.end());
    Json moving = Json::array();
    Json remaining = Json::array();
    for (const auto& node : document["nodes"]) {
        if (moving_set.contains(node.value("id", ""))) moving.push_back(node);
        else remaining.push_back(node);
    }
    document["nodes"] = std::move(remaining);
    const auto target_index = has_index ? sibling_index : direct_children(document, parent_id).size();
    const auto insertion = insertion_position(document, parent_id, target_index);
    if (insertion == std::numeric_limits<std::size_t>::max()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.invalid-sibling-index", "/request/siblingIndex", "The requested sibling index is outside the parent range.");
        return invalid_request("reparent", "ui.invalid-sibling-index", "Invalid sibling index.", std::move(diagnostics));
    }
    if (!moving.empty()) moving.at(0)["parentId"] = parent_id;
    document["nodes"].insert(document["nodes"].begin() + static_cast<Json::difference_type>(insertion), moving.begin(), moving.end());
    refresh_roots(document);
    return commit_candidate(canonical_dump(document), options, "reparent", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::reorder(
    const std::string_view node_id, const std::size_t sibling_index,
    const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("reorder", "ui.invalid-source", "The current source is invalid.", std::move(diagnostics));
    const auto index = node_index(document, node_id);
    if (!index) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.node-not-found", "/request/nodeId", "The requested node does not exist.");
        return invalid_request("reorder", "ui.node-not-found", "The requested node does not exist.", std::move(diagnostics));
    }
    const auto parent = parent_id_of(document.at("nodes").at(*index));
    const auto children = direct_children(document, parent);
    const auto current = std::ranges::find(children, node_id);
    if (current == children.end() || sibling_index >= children.size()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.invalid-sibling-index", "/request/siblingIndex", "The requested sibling index is outside the parent range.");
        return invalid_request("reorder", "ui.invalid-sibling-index", "Invalid sibling index.", std::move(diagnostics));
    }
    if (static_cast<std::size_t>(current - children.begin()) == sibling_index)
        return commit_candidate(canonical_dump(document), options, "reorder", HistoryDirection::edit);
    const auto moving_ids = subtree_ids(document, node_id);
    const std::unordered_set<std::string> moving_set(moving_ids.begin(), moving_ids.end());
    Json moving = Json::array();
    Json remaining = Json::array();
    for (const auto& node : document["nodes"]) {
        if (moving_set.contains(node.value("id", ""))) moving.push_back(node);
        else remaining.push_back(node);
    }
    document["nodes"] = std::move(remaining);
    const auto remaining_children = direct_children(document, parent);
    const auto effective_index = std::min(sibling_index, remaining_children.size());
    const auto insertion = insertion_position(document, parent, effective_index);
    if (insertion == std::numeric_limits<std::size_t>::max()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.invalid-sibling-index", "/request/siblingIndex", "The requested sibling index is outside the parent range.");
        return invalid_request("reorder", "ui.invalid-sibling-index", "Invalid sibling index.", std::move(diagnostics));
    }
    document["nodes"].insert(document["nodes"].begin() + static_cast<Json::difference_type>(insertion), moving.begin(), moving.end());
    refresh_roots(document);
    return commit_candidate(canonical_dump(document), options, "reorder", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::update_design_tokens(
    std::string design_tokens_json, const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    const auto tokens = Json::parse(design_tokens_json, nullptr, false);
    if (tokens.is_discarded() || !tokens.is_object()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.invalid-design-tokens", "/designTokens", "designTokens must be a JSON object.");
        return invalid_request("update_design_tokens", "ui.invalid-design-tokens", "Invalid design token object.", std::move(diagnostics));
    }
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("update_design_tokens", "ui.invalid-source", "The current source is invalid.", std::move(diagnostics));
    document["designTokens"] = tokens;
    return commit_candidate(canonical_dump(document), options, "update_design_tokens", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::add_declaration(
    ProjectUiAddDeclarationRequest request, const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    if (!safe_id(request.id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.unsafe-component-id", "/request/id",
            "Component id must be a stable path-safe identifier.");
    }
    auto declaration = Json::parse(request.declaration_json, nullptr, false);
    if (declaration.is_discarded() || !declaration.is_object()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-component", "/request/declaration",
            "A reusable declaration must be a JSON object.");
    } else if (declaration.contains("id") &&
               (!declaration.at("id").is_string() || declaration.at("id").get<std::string>() != request.id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-id-mismatch", "/request/declaration/id",
            "The declaration id must be absent or equal to the request id.");
    }
    if (!diagnostics.empty()) return invalid_request("add_declaration", "ui.invalid-request",
        "The reusable declaration request is invalid.", std::move(diagnostics));
    declaration["id"] = request.id;
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("add_declaration", "ui.invalid-source",
        "The current source is invalid.", std::move(diagnostics));
    if (!document.contains("components")) document["components"] = Json::array();
    if (!document.at("components").is_array()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-components", "/components", "components must be an array when present.");
        return invalid_request("add_declaration", "ui.invalid-components",
            "The current components collection is invalid.", std::move(diagnostics));
    }
    if (find_component(document, request.id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.duplicate-component-id", "/request/id", "A declaration with this id already exists.");
        return invalid_request("add_declaration", "ui.duplicate-component-id",
            "A declaration with this id already exists.", std::move(diagnostics));
    }
    document["components"].push_back(std::move(declaration));
    return commit_candidate(canonical_dump(document), options, "add_declaration", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::update_declaration(
    const std::string_view declaration_id, std::string declaration_json,
    const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    if (!safe_id(declaration_id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.unsafe-component-id", "/request/id",
            "Component id must be a stable path-safe identifier.");
        return invalid_request("update_declaration", "ui.unsafe-component-id",
            "The declaration id is not safe.", std::move(diagnostics));
    }
    auto declaration = Json::parse(declaration_json, nullptr, false);
    if (declaration.is_discarded() || !declaration.is_object()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.invalid-component", "/request/declaration",
            "A reusable declaration must be a JSON object.");
        return invalid_request("update_declaration", "ui.invalid-component",
            "The reusable declaration is invalid.", std::move(diagnostics));
    }
    if (declaration.contains("id") &&
        (!declaration.at("id").is_string() || declaration.at("id").get<std::string>() != declaration_id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-id-mismatch", "/request/declaration/id",
            "The declaration id must be absent or equal to the target id.");
        return invalid_request("update_declaration", "ui.component-id-mismatch",
            "The declaration id does not match the target.", std::move(diagnostics));
    }
    declaration["id"] = std::string(declaration_id);
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("update_declaration", "ui.invalid-source",
        "The current source is invalid.", std::move(diagnostics));
    if (!document.contains("components") || !document.at("components").is_array()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-not-found", "/request/id", "The target declaration does not exist.");
        return invalid_request("update_declaration", "ui.component-not-found",
            "The target declaration does not exist.", std::move(diagnostics));
    }
    const auto found = std::ranges::find_if(document["components"], [&](const auto& item) {
        return item.is_object() && item.value("id", "") == declaration_id;
    });
    if (found == document["components"].end()) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-not-found", "/request/id", "The target declaration does not exist.");
        return invalid_request("update_declaration", "ui.component-not-found",
            "The target declaration does not exist.", std::move(diagnostics));
    }
    *found = std::move(declaration);
    return commit_candidate(canonical_dump(document), options, "update_declaration", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::remove_declaration(
    const std::string_view declaration_id, const ProjectUiEditOptions options) {
    std::vector<ProjectUiDiagnostic> diagnostics;
    auto document = parse_document(source_json_, diagnostics);
    if (!diagnostics.empty()) return failure("remove_declaration", "ui.invalid-source",
        "The current source is invalid.", std::move(diagnostics));
    if (!find_component(document, declaration_id)) {
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
            "ui.component-not-found", "/request/id", "The target declaration does not exist.");
        return invalid_request("remove_declaration", "ui.component-not-found",
            "The target declaration does not exist.", std::move(diagnostics));
    }
    if (document.contains("nodes") && document.at("nodes").is_array()) {
        for (std::size_t index = 0U; index < document.at("nodes").size(); ++index) {
            const auto& node = document.at("nodes").at(index);
            if (node.is_object() && node.value("componentRef", "") == declaration_id) {
                add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                    "ui.component-in-use", "/nodes/" + std::to_string(index) + "/componentRef",
                    "A declaration cannot be removed while a node references it.");
                return invalid_request("remove_declaration", "ui.component-in-use",
                    "The declaration is still referenced by a node.", std::move(diagnostics));
            }
        }
    }
    for (std::size_t index = 0U; index < document.at("components").size(); ++index) {
        const auto& component = document.at("components").at(index);
        if (component.is_object() && component.value("extends", "") == declaration_id) {
            add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error,
                "ui.component-in-use", "/components/" + std::to_string(index) + "/extends",
                "A declaration cannot be removed while another declaration extends it.");
            return invalid_request("remove_declaration", "ui.component-in-use",
                "The declaration is still extended by another declaration.", std::move(diagnostics));
        }
    }
    auto& components = document["components"];
    components.erase(std::ranges::find_if(components, [&](const auto& item) {
        return item.is_object() && item.value("id", "") == declaration_id;
    }));
    return commit_candidate(canonical_dump(document), options, "remove_declaration", HistoryDirection::edit);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::undo(const ProjectUiEditOptions options) {
    if (options.expected_revision && *options.expected_revision != revision_) {
        std::vector<ProjectUiDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.revision-conflict", "/revision", "Expected revision does not match the current Project UI authoring revision.");
        return failure("undo", "ui.revision-conflict", "Refresh before undo.", std::move(diagnostics));
    }
    if (undo_.empty()) {
        std::vector<ProjectUiDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.undo-empty", "/history", "There is no committed Project UI edit to undo.");
        return failure("undo", "ui.undo-empty", "There is no committed Project UI edit to undo.", std::move(diagnostics));
    }
    const auto entry = undo_.back();
    return commit_candidate(entry.before, options, "undo", HistoryDirection::undo, entry);
}

ProjectUiEditReceipt ProjectUiAuthoringSession::redo(const ProjectUiEditOptions options) {
    if (options.expected_revision && *options.expected_revision != revision_) {
        std::vector<ProjectUiDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.revision-conflict", "/revision", "Expected revision does not match the current Project UI authoring revision.");
        return failure("redo", "ui.revision-conflict", "Refresh before redo.", std::move(diagnostics));
    }
    if (redo_.empty()) {
        std::vector<ProjectUiDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectUiDiagnosticSeverity::error, "ui.redo-empty", "/history", "There is no undone Project UI edit to redo.");
        return failure("redo", "ui.redo-empty", "There is no undone Project UI edit to redo.", std::move(diagnostics));
    }
    const auto entry = redo_.back();
    return commit_candidate(entry.after, options, "redo", HistoryDirection::redo, entry);
}

} // namespace noemancer
