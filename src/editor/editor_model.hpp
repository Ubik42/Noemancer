#pragma once

#include "engine/asset_registry.hpp"
#include "engine/asset_job_queue.hpp"
#include "engine/asset_repair.hpp"
#include "engine/asset_thumbnail.hpp"
#include "engine/asset_workflow.hpp"
#include "engine/animation_graph_patch.hpp"
#include "engine/world.hpp"

#include <cstddef>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct EditorObject final {
    std::string id;
    std::string name;
    std::string kind;
    std::string parent_id;
    SourceAnchor source;
    std::optional<Transform> transform;
    std::uint64_t revision{};
};

struct EditorAsset final {
    std::string id;
    std::string name;
    std::string kind;
    std::string source;
    std::string import_state;
    std::string content_hash;
    std::string license;
    bool available{};
    std::string thumbnail_uri;
    std::string thumbnail_strategy;
    bool thumbnail_cached{};
};

struct EditorPanel final {
    std::string id;
    std::string title;
    std::string region;
};

struct EditorViewportCamera final {
    Transform transform;
    Camera camera;
};

struct InspectorProperty final {
    std::string id;
    std::string component;
    std::string property;
    std::string label;
    std::string value_type;
    std::string control;
    std::string value_json;
    std::string unit;
    double minimum{};
    double maximum{1.0};
    double step{0.05};
    bool has_minimum{};
    bool has_maximum{};
    bool editable{};
    std::vector<std::string> options;
};

struct InspectorSection final {
    std::string id;
    std::string component;
    std::string label;
    bool default_expanded{};
    std::vector<InspectorProperty> properties;
};

struct EditorSceneAction final {
    bool success{};
    std::string code;
    std::string detail;
    std::string entity_id;
    std::uint64_t revision{};
};

class EditorModel final {
public:
    EditorModel(World& world, AssetRegistry& assets);

    [[nodiscard]] const std::vector<EditorObject>& objects() const noexcept;
    [[nodiscard]] const std::vector<EditorAsset>& assets() const noexcept;
    [[nodiscard]] const std::vector<EditorPanel>& panels() const noexcept;
    [[nodiscard]] const std::vector<InspectorSection>& inspector_sections() const noexcept;
    [[nodiscard]] std::string inspector_semantic_ui_document_json(std::string_view locale="en-US") const;
    [[nodiscard]] std::size_t selected_object_index() const noexcept;
    [[nodiscard]] const EditorObject& selected_object() const;
    [[nodiscard]] const std::vector<std::string>& selected_object_ids() const noexcept;
    [[nodiscard]] bool is_object_selected(std::string_view entity_id) const noexcept;
    [[nodiscard]] std::optional<EditorViewportCamera> viewport_camera() const;
    void select_object(std::size_t index, bool additive = false);
    [[nodiscard]] bool select_object(std::string_view entity_id, bool additive = false);
    void select_asset(std::size_t index) noexcept;
    [[nodiscard]] bool select_asset(std::string_view asset_id) noexcept;
    [[nodiscard]] std::size_t selected_asset_index() const noexcept;
    [[nodiscard]] const EditorAsset* selected_asset() const noexcept;
    [[nodiscard]] std::string selected_asset_inspection_json() const;
    [[nodiscard]] std::string selected_animation_graph_authoring_json() const;
    [[nodiscard]] std::string apply_selected_animation_graph_patch(
        const std::vector<AnimationGraphPatchOperation>& operations,
        std::string_view expected_fingerprint, bool dry_run);
    [[nodiscard]] std::string selected_tilemap_authoring_json() const;
    [[nodiscard]] std::string apply_selected_tilemap_stroke(std::string_view layer_id,
        const std::vector<TilemapCellEdit>& edits,std::string_view expected_fingerprint,bool dry_run);
    [[nodiscard]] std::string apply_selected_tilemap_region(std::string_view shape,std::string_view layer_id,
        std::array<std::int32_t,2> first,std::optional<std::array<std::int32_t,2>> second,
        std::optional<std::string> tile_id,bool flip_x,bool flip_y,std::string_view expected_fingerprint,bool dry_run);
    [[nodiscard]] std::string apply_selected_tile_palette_autotile(std::string_view tile_id,std::string_view autotile_group,
        const std::vector<TileAutotileVariant>& variants,std::string_view expected_fingerprint,bool dry_run);
    [[nodiscard]] std::string asset_registry_status_json() const;
    [[nodiscard]] std::string scripting_status_json() const;
    [[nodiscard]] std::string compile_scripts_json(std::string_view configuration = "Debug");
    [[nodiscard]] EditorSceneAction refresh_assets();
    [[nodiscard]] EditorSceneAction import_selected_asset();
    [[nodiscard]] EditorSceneAction inspect_selected_asset();
    [[nodiscard]] EditorSceneAction generate_selected_asset_thumbnail();
    [[nodiscard]] EditorSceneAction cook_selected_asset(std::string_view target_profile = "windows-x64-debug");
    [[nodiscard]] std::string active_asset_job_json() const;
    [[nodiscard]] std::string selected_asset_repair_json() const;
    [[nodiscard]] EditorSceneAction execute_selected_asset_repair(std::string_view action_id);
    [[nodiscard]] std::optional<EditorSceneAction> reconcile_active_asset_job();
    [[nodiscard]] EditorSceneAction cancel_active_asset_job();
    [[nodiscard]] EditorSceneAction retry_active_asset_job();
    void refresh();
    void reset_for_loaded_project();
    [[nodiscard]] TransformUpdateResult set_selected_transform(Transform transform);
    [[nodiscard]] ActionReceipt set_selected_property(std::string_view property, std::string_view value_json);
    [[nodiscard]] ActionReceipt undo();
    [[nodiscard]] ActionReceipt redo();
    [[nodiscard]] EditorSceneAction create_empty_entity(std::string_view display_name = "Empty Entity",
                                                        std::string_view parent_entity_id = {});
    [[nodiscard]] EditorSceneAction duplicate_selected();
    [[nodiscard]] EditorSceneAction copy_selected();
    [[nodiscard]] EditorSceneAction paste_copied();
    [[nodiscard]] bool can_paste() const noexcept;
    [[nodiscard]] EditorSceneAction rename_selected(std::string_view display_name);
    [[nodiscard]] EditorSceneAction reparent_entity(std::string_view entity_id, std::string_view parent_entity_id);
    [[nodiscard]] EditorSceneAction delete_selected(bool recursive);
    [[nodiscard]] EditorSceneAction add_component(std::string_view component);
    [[nodiscard]] EditorSceneAction remove_component(std::string_view component);
    [[nodiscard]] EditorSceneAction new_scene(std::string_view display_name = "Untitled Scene");
    [[nodiscard]] EditorSceneAction save_scene();
    [[nodiscard]] EditorSceneAction save_scene_as(std::string_view source_path, bool overwrite = false);
    [[nodiscard]] EditorSceneAction open_scene(std::string_view source_path);
    [[nodiscard]] std::string scene_recovery_candidates_json(std::string_view project_root) const;
    [[nodiscard]] EditorSceneAction recover_scene(std::string_view project_root,
                                                  std::string_view recovery_relative_path);
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] bool can_save_scene() const;
    [[nodiscard]] bool scene_dirty() const;
    [[nodiscard]] const std::string& scene_source() const noexcept;
    [[nodiscard]] std::uint64_t world_revision() const noexcept;
    void set_focused_panel(std::string_view panel_id) noexcept;
    [[nodiscard]] const std::string& focused_panel() const noexcept;
    [[nodiscard]] std::string focused_observation_json() const;
    [[nodiscard]] std::string semantic_snapshot_json() const;

private:
    enum class EditDomain { scene,asset_source };
    void record_edit(EditDomain domain);
    void refresh_inspector();
    World& world_;
    AssetRegistry& asset_registry_;
    std::vector<EditorObject> objects_;
    std::vector<EditorAsset> assets_;
    std::vector<EditorPanel> panels_;
    std::vector<InspectorSection> inspector_sections_;
    std::string selected_object_id_;
    std::vector<std::string> selected_object_ids_;
    std::string scene_clipboard_json_;
    std::string selected_asset_id_;
    std::string focused_panel_id_{"editor.panel.scene"};
    std::string observed_scene_source_;
    std::string saved_scene_json_;
    std::vector<EditDomain> edit_undo_timeline_;
    std::vector<EditDomain> edit_redo_timeline_;
    std::uint64_t generated_entity_sequence_{1};
    AssetJobQueue asset_jobs_{{.worker_count=1,.max_queued_jobs=16}};
    std::string active_asset_job_id_;
    AssetJobKind active_asset_job_kind_{AssetJobKind::inspect};
    std::uint64_t active_asset_job_base_revision_{};
    bool active_asset_job_reconciled_{};
};

} // namespace noemancer
