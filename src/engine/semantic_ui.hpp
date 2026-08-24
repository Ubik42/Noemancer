#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct SemanticUiQuery final {
    std::vector<std::string> node_ids;
    std::vector<std::string> roles;
    std::size_t depth{2};
    std::size_t byte_budget{16 * 1024};
    std::size_t cursor{};
    bool include_values{true};
};

struct SemanticUiDeltaQuery final {
    std::size_t byte_budget{8 * 1024};
    std::size_t cursor{};
    bool include_values{true};
    std::string base_fingerprint;
};

[[nodiscard]] std::string semantic_ui_document_from_inspector(
    std::string_view inspector_document_json,
    std::string_view locale = "en-US");
[[nodiscard]] std::string semantic_ui_game_hud_document(
    std::string_view ability_state_json,
    std::string_view entity_id,
    std::string_view locale = "en-US");
[[nodiscard]] std::string semantic_ui_project_runtime_document(
    std::string_view project_document_json,
    std::string_view scripting_state_json,
    std::string_view input_state_json,
    std::string_view gameplay_state_json,
    std::string_view locale = "en-US");
[[nodiscard]] std::string semantic_ui_query_json(
    std::string_view semantic_ui_document_json,
    const SemanticUiQuery& query);
[[nodiscard]] std::string semantic_ui_delta_json(
    std::string_view semantic_ui_document_json,
    std::string_view semantic_world_delta_json,
    std::string_view entity_id,
    const SemanticUiDeltaQuery& query = {});
[[nodiscard]] std::string semantic_ui_validation_json(std::string_view semantic_ui_document_json);
[[nodiscard]] std::string semantic_ui_resource_status_json(std::string_view locale = "en-US");

} // namespace noemancer
