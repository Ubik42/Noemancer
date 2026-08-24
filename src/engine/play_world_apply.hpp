#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct PlayWorldChange final {
    std::string entity_id;
    std::string field;
    std::string before_json;
    std::string after_json;
    // Stable within the same scene identity and change path.  It is derived
    // from operation/entity/field, never from vector position or JSON order.
    std::string change_id;
    std::string operation;
};

struct PlayWorldApplyPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint64_t base_revision{};
    std::string base_scene_json;
    std::string candidate_scene_json;
    std::vector<PlayWorldChange> changes;

    [[nodiscard]] std::string to_json() const;
};

// A selection is a plain-data, reviewable projection of a complete apply
// diff.  It intentionally carries the same base revision so the host can
// apply it through the normal revision/transaction boundary.
struct PlayWorldSelectionPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::uint64_t base_revision{};
    std::string base_scene_json;
    std::string candidate_scene_json;
    std::vector<std::string> selected_change_ids;
    std::vector<PlayWorldChange> changes;

    [[nodiscard]] std::string to_json() const;
};

// Builds a deterministic, reviewable plan. Only authorable scene fields are
// present in runtime_scene_json; transient physics/renderer state never enters
// this boundary.
[[nodiscard]] PlayWorldApplyPlan plan_play_world_apply(
    std::string_view base_scene_json,
    std::string_view runtime_scene_json,
    std::uint64_t base_revision);

// Rebuilds a canonical candidate from the complete diff and an explicit set
// of stable change IDs.  Empty selection means "apply nothing".  The
// complete plan remains the source of truth for before/after values; this is
// not a JSON-Patch surface and does not expose third-party patch types.
[[nodiscard]] PlayWorldSelectionPlan plan_play_world_apply_selection(
    const PlayWorldApplyPlan& complete_plan,
    const std::vector<std::string>& selected_change_ids);

} // namespace noemancer
