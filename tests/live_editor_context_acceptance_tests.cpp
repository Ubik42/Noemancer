#include "editor/editor_ui.hpp"
#include "engine/command_registry.hpp"
#include "engine/process_diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;

bool require(const bool condition, const std::string_view message, int& failures) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
    return false;
}

Json parse(const std::string_view text, const std::string_view label, int& failures) {
    const auto value = Json::parse(text, nullptr, false);
    require(!value.is_discarded(), std::string(label) + " is not valid JSON", failures);
    return value.is_discarded() ? Json::object() : value;
}

bool contains(const std::vector<std::string>& values, const std::string_view expected) {
    for (const auto& value : values) {
        if (value == expected) return true;
    }
    return false;
}

Json find_tool(const Json& manifest, const std::string_view name) {
    if (!manifest.is_object() || !manifest.contains("tools") || !manifest.at("tools").is_array()) {
        return Json::object();
    }
    for (const auto& tool : manifest.at("tools")) {
        if (tool.is_object() && tool.value("name", std::string{}) == name) return tool;
    }
    return Json::object();
}

bool has_chrome_region(const Json& chrome, const std::string_view id) {
    if (!chrome.is_object() || !chrome.contains("regions") || !chrome.at("regions").is_array()) return false;
    for (const auto& region : chrome.at("regions")) {
        if (region.is_object() && region.value("id", std::string{}) == id) return true;
    }
    return false;
}

} // namespace

int main() {
    noemancer::configure_process_diagnostics("test.live-editor-context-acceptance");

    noemancer::World world;
    if (!world.load_scene(noemancer::make_bootstrap_scene_document()).success) {
        std::cerr << "Bootstrap scene did not load for live Editor context acceptance\n";
        return 1;
    }
    noemancer::AssetRegistry assets;
    noemancer::EditorUi editor(world, assets);
    editor.set_project_context(noemancer::EditorProjectContext{
        .project_id = "project.live-editor-context",
        .name = "Live Editor Context Fixture",
        .root = "engine://",
        .startup_scene = "scenes/bootstrap.scene.json"});

    int failures = 0;
    const auto scene_before = world.canonical_scene_json();
    const auto world_revision_before = world.revision();
    const auto entity_count_before = world.entity_count();
    const auto initial = editor.editor_context_snapshot();

    require(initial.schema_version == "noemancer.editor-context/0.1",
        "Editor context uses the stable plain-data schema", failures);
    require(initial.revision > 0U && initial.world_revision == world_revision_before,
        "Editor context exposes a non-zero revision bound to the existing World", failures);
    require(initial.authority == "edit-world" && initial.simulation_state == "edit" && initial.writable,
        "Initial context identifies the writable Edit World authority", failures);
    require(initial.project_id == "project.live-editor-context" &&
        initial.project_name == "Live Editor Context Fixture" &&
        initial.scene_source == "asset://scenes/bootstrap.scene.json",
        "Context observe includes the active project and scene identity", failures);
    require(contains(initial.selected_entity_ids, "entity.demo-cube") &&
        initial.primary_selected_entity_id == "entity.demo-cube" &&
        initial.focused_panel_id == "editor.panel.scene",
        "Context observe includes the deterministic Outliner/Inspector selection and focus", failures);

    const auto observed_json = parse(editor.editor_context_snapshot_json(),
        "editor context observation", failures);
    require(observed_json.value("schemaVersion", std::string{}) == "noemancer.editor-context/0.1" &&
        observed_json.value("revision", 0ULL) == initial.revision &&
        observed_json.value("worldRevision", 0ULL) == world_revision_before,
        "JSON observation preserves the typed context identity and revision", failures);
    require(observed_json.value("selection", Json::object()).is_object() &&
        observed_json.at("selection").contains("entityIds") &&
        observed_json.at("selection").contains("primaryEntityId") &&
        observed_json.value("focus", Json::object()).is_object() &&
        observed_json.at("focus").contains("panelId") &&
        observed_json.at("focus").contains("activeTabId"),
        "JSON observation exposes stable selection/focus fields rather than UI handles", failures);

    // The existing full semantic snapshot remains the source of Editor chrome
    // truth.  The live adapter must project this same state, not create a
    // parallel hierarchy or Inspector database.
    const auto semantic = parse(editor.semantic_snapshot_json(), "Editor semantic snapshot", failures);
    const auto chrome = semantic.value("editorChrome", Json::object());
    require(chrome.value("schemaVersion", std::string{}) == "noemancer.editor-chrome/0.1" &&
        has_chrome_region(chrome, "editor.scene-view") &&
        has_chrome_region(chrome, "editor.world-outliner") &&
        has_chrome_region(chrome, "editor.inspector"),
        "Editor context is backed by the existing scene/outliner/Inspector chrome projection", failures);
    if (chrome.is_object() && chrome.contains("regions") && chrome.at("regions").is_array()) {
        for (const auto& region : chrome.at("regions")) {
            if (!region.is_object()) continue;
            if (region.value("id", std::string{}) == "editor.world-outliner") {
                require(region.value("selectedEntityIds", Json::array()).is_array() &&
                    region.at("selectedEntityIds").size() == 1U &&
                    region.at("selectedEntityIds").front() == "entity.demo-cube",
                    "Outliner chrome exposes the same initial selection", failures);
            }
            if (region.value("id", std::string{}) == "editor.inspector") {
                require(region.value("selectedEntityId", Json(nullptr)) == "entity.demo-cube",
                    "Inspector chrome exposes the same initial selection", failures);
            }
        }
    }

    // CommandRegistry is still the wire-level manifest authority.  The live
    // runtime adapter must route these two commands to this already-owned
    // EditorUi instance; it must not construct a detached serve World.
    noemancer::CommandRegistry commands(world, assets);
    const auto manifest = parse(commands.manifest_json(), "CommandRegistry manifest", failures);
    require(manifest.value("protocolVersion", std::string{}) == "0.2" &&
        manifest.value("tools", Json::array()).is_array(),
        "Live Editor context commands use the existing CommandRegistry manifest envelope", failures);
    const auto observe_tool = find_tool(manifest, "editor.context.observe");
    const auto intent_tool = find_tool(manifest, "editor.context.intent");
    require(!observe_tool.empty() && !intent_tool.empty(),
        "Manifest registers editor.context.observe and editor.context.intent", failures);
    if (!observe_tool.empty()) {
        require(observe_tool.value("access", std::string{}) == "read" &&
            observe_tool.value("idempotent", false) &&
            observe_tool.value("runtimeState", std::string{}) == "attached" &&
            observe_tool.value("taskKind", std::string{}) == "immediate" &&
            observe_tool.value("inputSchema", Json(nullptr)).is_object() &&
            observe_tool.value("outputSchema", Json(nullptr)).is_object(),
            "Observe command advertises bounded read/attached structured metadata", failures);
    }
    if (!intent_tool.empty()) {
        require(intent_tool.value("access", std::string{}) == "write" &&
            intent_tool.value("supportsDryRun", false) &&
            intent_tool.value("runtimeState", std::string{}) == "attached" &&
            intent_tool.value("taskKind", std::string{}) == "immediate" &&
            intent_tool.value("inputSchema", Json(nullptr)).is_object() &&
            intent_tool.value("outputSchema", Json(nullptr)).is_object(),
            "Intent command advertises revision-bound structured write metadata", failures);
    }

    noemancer::EditorUiContextIntent preview_intent;
    preview_intent.expected_revision = initial.revision;
    preview_intent.dry_run = true;
    preview_intent.selected_entity_ids = std::vector<std::string>{"entity.test-alien"};
    preview_intent.primary_selected_entity_id = "entity.test-alien";
    const auto previewed = editor.apply_editor_context_intent(preview_intent);
    require(previewed.success && previewed.code == "editor.context-dry-run" &&
        previewed.revision_after == initial.revision &&
        editor.editor_context_snapshot().primary_selected_entity_id == initial.primary_selected_entity_id,
        "Dry-run validates a context intent without changing visible selection", failures);

    // Selection is an Editor projection operation.  It must update the one
    // visible context while leaving the authoritative World document alone.
    noemancer::EditorUiContextIntent select_intent;
    select_intent.expected_revision = initial.revision;
    select_intent.selected_entity_ids = std::vector<std::string>{"entity.test-alien"};
    select_intent.primary_selected_entity_id = "entity.test-alien";
    const auto selected = editor.apply_editor_context_intent(select_intent);
    require(selected.success && selected.code == "ok" && selected.revision_after > initial.revision,
        "Revision-bound entity selection commits through the existing Editor model", failures);
    require(world.revision() == world_revision_before && world.entity_count() == entity_count_before &&
        world.canonical_scene_json() == scene_before,
        "Entity selection does not mutate the World or create a second scene model", failures);
    const auto after_selection = editor.editor_context_snapshot();
    require(contains(after_selection.selected_entity_ids, "entity.test-alien") &&
        after_selection.primary_selected_entity_id == "entity.test-alien" &&
        after_selection.revision == selected.revision_after,
        "Selection receipt and observation remain revision-consistent", failures);

    noemancer::EditorUiContextIntent focus_intent;
    focus_intent.expected_revision = after_selection.revision;
    focus_intent.focused_panel_id = "editor.panel.inspector";
    const auto focused = editor.apply_editor_context_intent(focus_intent);
    require(focused.success && focused.code == "ok" && focused.revision_after > after_selection.revision,
        "Revision-bound Inspector focus commits through the existing Editor model", failures);
    require(world.revision() == world_revision_before && world.entity_count() == entity_count_before &&
        world.canonical_scene_json() == scene_before,
        "Inspector focus does not mutate the World", failures);
    const auto after_focus = editor.editor_context_snapshot();
    require(after_focus.focused_panel_id == "editor.panel.inspector" &&
        after_focus.revision == focused.revision_after,
        "Focus receipt and observation remain revision-consistent", failures);

    // A plan captured against the old context must fail after another client
    // advances the context, and the stale request must not partially select or
    // focus anything.  This is the same CAS boundary used by the live command.
    noemancer::EditorUiContextIntent stale_intent;
    stale_intent.expected_revision = initial.revision;
    stale_intent.selected_entity_ids = std::vector<std::string>{"entity.demo-cube"};
    stale_intent.primary_selected_entity_id = "entity.demo-cube";
    const auto stale = editor.apply_editor_context_intent(stale_intent);
    require(!stale.success && stale.code == "editor.context-conflict",
        "Stale context intent is rejected with a stable revision-conflict code", failures);
    const auto after_stale = editor.editor_context_snapshot();
    require(after_stale.revision == after_focus.revision &&
        after_stale.primary_selected_entity_id == after_focus.primary_selected_entity_id &&
        after_stale.focused_panel_id == after_focus.focused_panel_id &&
        world.revision() == world_revision_before &&
        world.canonical_scene_json() == scene_before,
        "Rejected stale intent leaves the Editor projection and World unchanged", failures);

    // The JSON adapter is the boundary consumed by the live transport.  Keep
    // this check deliberately small: the typed API above owns validation and
    // the JSON wrapper must return the same structured receipt, not a message.
    const auto json_request = Json{
        {"expectedRevision", after_focus.revision},
        {"focus", {{"panelId", "editor.panel.assets"}}}}.dump();
    const auto json_receipt = parse(editor.apply_editor_context_intent_json(json_request),
        "JSON context intent receipt", failures);
    require(json_receipt.value("success", false) && json_receipt.value("code", std::string{}) == "ok" &&
        json_receipt.value("revisionAfter", 0ULL) > after_focus.revision,
        "JSON context intent returns a structured revisioned receipt", failures);
    require(world.revision() == world_revision_before && world.entity_count() == entity_count_before &&
        world.canonical_scene_json() == scene_before,
        "JSON selection/focus intent remains UI-only with respect to the World", failures);

    commands.attach_editor_context(
        [&editor] { return editor.editor_context_snapshot_json(); },
        [&editor](const std::string_view request) {
            return editor.apply_editor_context_intent_json(request);
        });
    const auto wire_observation = commands.invoke("editor.context.observe", "{}");
    const auto wire_observation_envelope = parse(
        wire_observation.output_json, "live command context observation", failures);
    require(wire_observation.exit_code == 0 &&
        wire_observation_envelope.value("ok", false) &&
        wire_observation_envelope.value("result", Json::object())
            .value("revision", 0ULL) == editor.editor_context_revision(),
        "CommandRegistry routes observation to the same EditorUi authority", failures);

    const auto wire_revision = editor.editor_context_revision();
    const auto wire_intent = commands.invoke("editor.context.intent", Json{
        {"expectedRevision", wire_revision},
        {"focus", {{"panelId", "editor.panel.inspector"}}}}.dump());
    const auto wire_intent_envelope = parse(
        wire_intent.output_json, "live command context intent", failures);
    require(wire_intent.exit_code == 0 && wire_intent_envelope.value("ok", false) &&
        wire_intent_envelope.value("result", Json::object()).value("success", false) &&
        editor.editor_context_snapshot().focused_panel_id == "editor.panel.inspector",
        "CommandRegistry routes revision-bound intent to the visible EditorUi authority", failures);
    require(world.revision() == world_revision_before && world.entity_count() == entity_count_before &&
        world.canonical_scene_json() == scene_before,
        "Live command routing does not create or mutate a second World", failures);

    if (failures != 0) {
        std::cerr << "Live Editor context acceptance failed with " << failures << " assertion(s).\n";
        return 2;
    }
    std::cout << "Live Editor context acceptance passed: one EditorUi authority, revision-bound selection/focus, and unchanged World.\n";
    return 0;
}
