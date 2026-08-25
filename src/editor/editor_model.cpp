#include "editor/editor_model.hpp"
#include "editor/animation_graph_canvas.hpp"
#include "engine/command_registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string escape_json(const std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output += character; break;
        }
    }
    return output;
}

std::string receipt_detail(const Json& receipt, const std::string_view fallback) {
    auto detail = receipt.value("detail",std::string(fallback));
    if (!receipt.value("success",false) && receipt.contains("errors") && receipt.at("errors").is_array()) {
        std::size_t count{};
        for (const auto& error : receipt.at("errors")) {
            if (count++ == 3) break;
            detail += " ";
            detail += error.value("message","Validation failed.");
        }
    }
    return detail;
}

std::optional<std::filesystem::path> thumbnail_cache_path(
    const std::filesystem::path& generated_root,const std::string_view uri) {
    constexpr std::string_view prefix="cache://thumbnails/";
    if(!uri.starts_with(prefix))return std::nullopt;
    const auto name=uri.substr(prefix.size());
    if(name.empty()||name.find('/')!=std::string_view::npos||name.find('\\')!=std::string_view::npos||
       name.find("..")!=std::string_view::npos)return std::nullopt;
    const auto candidate=(generated_root/"thumbnail-cache"/std::string(name)).lexically_normal();
    const auto root=(generated_root/"thumbnail-cache").lexically_normal();
    auto relative=candidate.lexically_relative(root);
    if(relative.empty()||(!relative.empty()&&*relative.begin()==std::filesystem::path("..")))return std::nullopt;
    return candidate;
}

ThumbnailSourceReadResult read_thumbnail_source(
    const std::filesystem::path& source,const std::size_t maximum_bytes) {
    std::error_code error;
    if(!std::filesystem::is_regular_file(source,error))
        return {.code="thumbnail.source-unavailable",.detail="The thumbnail source is unavailable."};
    const auto bytes=std::filesystem::file_size(source,error);
    if(error||bytes==0U||bytes>maximum_bytes)
        return {.code="thumbnail.source-too-large",.detail="The thumbnail source is empty or exceeds the source budget."};
    std::ifstream input(source,std::ios::binary);
    if(!input)return {.code="thumbnail.source-read-failed",.detail="The thumbnail source could not be opened."};
    ThumbnailSourceReadResult result{.success=true,.code="ok",.detail="Thumbnail source read."};
    result.bytes.resize(static_cast<std::size_t>(bytes));
    input.read(reinterpret_cast<char*>(result.bytes.data()),static_cast<std::streamsize>(result.bytes.size()));
    if(!input)return {.code="thumbnail.source-read-failed",.detail="The thumbnail source could not be read."};
    return result;
}

bool write_thumbnail_artifact(const std::filesystem::path& generated_root,const std::string_view uri,
    const std::span<const std::uint8_t> bytes,std::string& code,std::string& detail) {
    const auto destination=thumbnail_cache_path(generated_root,uri);
    if(!destination){code="thumbnail.artifact-uri-unsafe";detail="The thumbnail cache URI is unsafe.";return false;}
    std::error_code error;
    std::filesystem::create_directories(destination->parent_path(),error);
    if(error){code="thumbnail.artifact-directory-failed";detail="The thumbnail cache directory could not be created.";return false;}
    if(std::filesystem::is_regular_file(*destination,error)){code="ok";detail="Thumbnail cache hit.";return true;}
    auto temporary=*destination;temporary += ".tmp";
    {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output){code="thumbnail.artifact-write-failed";detail="The thumbnail staging artifact could not be opened.";return false;}
        output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!output){output.close();std::filesystem::remove(temporary,error);code="thumbnail.artifact-write-failed";detail="The thumbnail artifact could not be written.";return false;}
    }
    std::filesystem::rename(temporary,*destination,error);
    if(error){std::filesystem::remove(temporary,error);code="thumbnail.artifact-commit-failed";detail="The thumbnail artifact could not be committed.";return false;}
    code="ok";detail="Thumbnail artifact committed.";return true;
}

std::optional<std::filesystem::path> project_member(
    const std::filesystem::path& project_root,const std::filesystem::path& relative) {
    if(project_root.empty()||relative.empty()||relative.is_absolute())return std::nullopt;
    std::error_code error;const auto root=std::filesystem::weakly_canonical(project_root,error);
    if(error||!std::filesystem::is_directory(root,error))return std::nullopt;
    const auto candidate=std::filesystem::weakly_canonical(root/relative,error);if(error)return std::nullopt;
    const auto inside=candidate.lexically_relative(root);
    if(inside.empty()||inside.begin()->string()=="..")return std::nullopt;
    return candidate;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);if(!input)return {};
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}

} // namespace

EditorModel::EditorModel(World& world, AssetRegistry& assets)
    : world_(world), asset_registry_(assets), panels_{
          {"editor.panel.scene", "Scene View", "center"},
          {"editor.panel.outliner", "World Outliner", "left"},
          {"editor.panel.inspector", "Inspector", "right"},
          {"editor.panel.animation-graph", "Animation Graph", "center"},
          {"editor.panel.assets", "Asset Browser", "bottom"},
          {"editor.panel.console", "Console", "bottom"},
          {"editor.panel.agent-context", "Agent Context", "bottom"}
      } {
    refresh();
}

const std::vector<EditorObject>& EditorModel::objects() const noexcept {
    return objects_;
}

const std::vector<EditorAsset>& EditorModel::assets() const noexcept {
    return assets_;
}

const std::vector<EditorPanel>& EditorModel::panels() const noexcept {
    return panels_;
}

const std::vector<InspectorSection>& EditorModel::inspector_sections() const noexcept { return inspector_sections_; }
std::string EditorModel::inspector_semantic_ui_document_json(const std::string_view locale) const {
    return world_.semantic_ui_document_json(selected_object_id_,locale);
}

std::string EditorModel::outliner_semantic_ui_document_json(
    const EditorOutlinerSemanticOptions options) const {
    return outliner_semantic_ui_document_json(EditorOutlinerAuthorityView{
        .authority = "edit-world",
        .simulation_state = "edit",
        .writable = true,
        .world_revision = world_.revision(),
        .objects = objects_,
        .selected_object_ids = selected_object_ids_,
        .primary_selected_object_id = selected_object_id_}, options);
}

std::string EditorModel::outliner_semantic_ui_document_json(
    const EditorOutlinerAuthorityView& authority,
    const EditorOutlinerSemanticOptions options) const {
    const auto entity_limit = std::min<std::size_t>(options.entity_limit, 4096U);
    const auto selection_limit = std::min<std::size_t>(options.selection_limit, 256U);

    std::vector<const EditorObject*> ordered;
    ordered.reserve(authority.objects.size());
    for (const auto& object : authority.objects) ordered.push_back(&object);
    std::ranges::sort(ordered, {}, [](const EditorObject* object) { return object->id; });

    std::unordered_map<std::string_view, const EditorObject*> by_id;
    by_id.reserve(ordered.size());
    for (const auto* object : ordered) by_id.try_emplace(object->id, object);
    std::unordered_map<std::string_view, std::vector<const EditorObject*>> children;
    std::vector<const EditorObject*> roots;
    for (const auto* object : ordered) {
        if (object->parent_id.empty() || object->parent_id == object->id || !by_id.contains(object->parent_id))
            roots.push_back(object);
        else
            children[object->parent_id].push_back(object);
    }

    std::vector<const EditorObject*> hierarchy;
    hierarchy.reserve(std::min(entity_limit, ordered.size()));
    std::unordered_set<std::string_view> visited;
    visited.reserve(ordered.size());
    const auto append_tree = [&](const auto& self, const EditorObject* object) -> void {
        if (hierarchy.size() >= entity_limit || !visited.insert(object->id).second) return;
        hierarchy.push_back(object);
        const auto found = children.find(object->id);
        if (found == children.end()) return;
        for (const auto* child : found->second) self(self, child);
    };
    for (const auto* root : roots) append_tree(append_tree, root);
    // Malformed cyclic input is still represented deterministically as
    // top-level entries rather than disappearing from the semantic surface.
    for (const auto* object : ordered) append_tree(append_tree, object);

    std::vector<std::string> selected;
    selected.reserve(std::min(selection_limit, authority.selected_object_ids.size()));
    for (const auto& id : authority.selected_object_ids) {
        if (selected.size() >= selection_limit) break;
        if (by_id.contains(id) && std::ranges::find(selected, id) == selected.end()) selected.push_back(id);
    }
    std::ranges::sort(selected);
    const auto selected_set = std::unordered_set<std::string>(selected.begin(), selected.end());
    const auto primary = by_id.contains(authority.primary_selected_object_id) ?
        std::string(authority.primary_selected_object_id) : std::string{};

    const auto action = [](const std::string_view id, const std::string_view handler,
                           Json binding, const bool enabled, const bool busy = false) {
        const auto label = id == "outliner.create-empty" ? "Create" :
            id == "outliner.paste" ? "Paste" : id == "outliner.rename" ? "Rename" :
            id == "outliner.copy" ? "Copy" : id == "outliner.duplicate" ? "Duplicate" :
            id == "outliner.reparent" ? "Reparent" : id == "outliner.delete" ? "Delete" :
            id == "outliner.select" ? "Select" : id;
        return Json{{"id", id}, {"label", label}, {"dispatch", "existing-editor-model"}, {"handler", handler},
            {"binding", std::move(binding)}, {"state", {{"enabled", enabled}, {"busy", busy}}}};
    };
    Json panel_actions = Json::array();
    if (authority.writable) {
        panel_actions.push_back(action("outliner.create-empty", "EditorModel.create_empty_entity",
            {{"kind", "editor-entity-create"}, {"parentEntityId", nullptr},
                {"sourceRevision", authority.world_revision}}, true));
        panel_actions.push_back(action("outliner.paste", "EditorModel.paste_copied",
            {{"kind", "editor-entity-paste"}, {"sourceRevision", authority.world_revision}}, can_paste()));
    }
    Json nodes = Json::array({Json{
        {"id", "editor.panel.outliner"}, {"parentId", nullptr}, {"role", "tree"},
        {"label", "World Outliner"},
        {"presentation", {{"inlineActionIds", authority.writable ?
            Json::array({"outliner.create-empty", "outliner.paste"}) : Json::array()}}},
        {"state", {{"visible", true}, {"enabled", true}, {"editable", authority.writable}}},
        {"actions", std::move(panel_actions)}}});
    for (const auto* object : hierarchy) {
        const auto parent_is_included = !object->parent_id.empty() && visited.contains(object->parent_id);
        const auto selected_entity = selected_set.contains(object->id);
        const auto entity_binding = [&](const std::string_view kind, const std::string_view operation) {
            return Json{{"kind", kind}, {"operation", operation}, {"entityId", object->id},
                {"sourceRevision", authority.world_revision}, {"entityRevision", object->revision}};
        };
        Json actions = Json::array({action("outliner.select", "EditorModel.select_object",
            entity_binding("editor-entity-selection", "select"), true)});
        if (authority.writable && selected_entity) {
            auto rename_action = action("outliner.rename", "EditorModel.rename_selected",
                entity_binding("editor-entity-action", "rename"), true);
            rename_action["input"] = {{"field", "displayName"}, {"control", "text"},
                {"value", object->name}, {"placeholder", "Display name"}, {"maxLength", 128U}};
            actions.push_back(std::move(rename_action));
            actions.push_back(action("outliner.copy", "EditorModel.copy_selected",
                entity_binding("editor-entity-action", "copy"), true));
            actions.push_back(action("outliner.duplicate", "EditorModel.duplicate_selected",
                entity_binding("editor-entity-action", "duplicate"), true));
            auto reparent_action = action("outliner.reparent", "EditorModel.reparent_entity",
                entity_binding("editor-entity-action", "reparent"), true);
            Json parent_options = Json::array({Json{{"value", ""}, {"label", "Scene Root"}}});
            for (const auto* candidate : hierarchy) {
                if (parent_options.size() >= 256U) break;
                if (candidate->id == object->id) continue;
                auto ancestor_id = candidate->parent_id;
                auto would_create_cycle = false;
                while (!ancestor_id.empty()) {
                    if (ancestor_id == object->id) { would_create_cycle = true; break; }
                    const auto ancestor = by_id.find(ancestor_id);
                    if (ancestor == by_id.end()) break;
                    ancestor_id = ancestor->second->parent_id;
                }
                if (would_create_cycle) continue;
                parent_options.push_back({{"value", candidate->id}, {"label", candidate->name}});
            }
            reparent_action["input"] = {{"field", "parentEntityId"}, {"control", "combo"},
                {"value", object->parent_id}, {"options", std::move(parent_options)}};
            actions.push_back(std::move(reparent_action));
            auto delete_action = action("outliner.delete", "EditorModel.delete_selected",
                entity_binding("editor-entity-action", "delete"), true);
            delete_action["confirmation"] = {{"field", "confirmed"},
                {"label", "Confirm recursive delete"}, {"required", true}};
            actions.push_back(std::move(delete_action));
        }
        nodes.push_back({
            {"id", "editor.outliner.entity." + object->id},
            {"parentId", parent_is_included ? Json("editor.outliner.entity." + object->parent_id) :
                Json("editor.panel.outliner")},
            {"role", "treeitem"}, {"label", object->name},
            {"presentation", {{"inlineActionIds", authority.writable && selected_entity ?
                Json::array({"outliner.rename", "outliner.copy", "outliner.duplicate",
                    "outliner.reparent", "outliner.delete"}) : Json::array()}}},
            {"entity", {{"id", object->id}, {"parentId", object->parent_id.empty() ? Json(nullptr) : Json(object->parent_id)},
                {"type", object->kind}, {"revision", object->revision}}},
            {"state", {{"visible", true}, {"enabled", true}, {"editable", authority.writable},
                {"selected", selected_entity}, {"primarySelected", object->id == primary},
                {"expanded", children.contains(object->id)}}},
            {"status", {{"authority", authority.authority}, {"simulationState", authority.simulation_state},
                {"writable", authority.writable}}},
            {"actions", std::move(actions)}});
    }

    return Json{
        {"schemaVersion", "noemancer.ui-document/0.1"}, {"documentId", "editor.world-outliner"},
        {"valid", true}, {"code", "ok"}, {"revision", authority.world_revision},
        {"authority", authority.authority}, {"simulationState", authority.simulation_state},
        {"writable", authority.writable},
        {"selection", {{"primaryEntityId", primary.empty() ? Json(nullptr) : Json(primary)},
            {"entityIds", selected}, {"total", authority.selected_object_ids.size()},
            {"limit", selection_limit}, {"truncated", selected.size() < authority.selected_object_ids.size()}}},
        {"entities", {{"total", ordered.size()}, {"included", hierarchy.size()},
            {"limit", entity_limit}, {"truncated", hierarchy.size() < ordered.size()}}},
        {"nodes", std::move(nodes)}}.dump();
}

std::string EditorModel::asset_browser_semantic_ui_document_json(
    const EditorAssetBrowserSemanticQuery request) const {
    constexpr std::size_t hard_page_limit = 256U;
    constexpr std::size_t hard_query_bytes = 256U;
    const auto query_bytes = std::min(request.query.size(), hard_query_bytes);
    auto query = std::string(request.query.substr(0U, query_bytes));
    std::ranges::transform(query, query.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const auto matches_query = [&query](const EditorAsset& asset) {
        if (query.empty()) return true;
        const auto contains = [&query](std::string value) {
            std::ranges::transform(value, value.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value.find(query) != std::string::npos;
        };
        return contains(asset.id) || contains(asset.name) || contains(asset.kind) ||
            contains(asset.source) || contains(asset.import_state) || contains(asset.license);
    };

    std::vector<const EditorAsset*> matched;
    matched.reserve(assets_.size());
    for (const auto& asset : assets_) if (matches_query(asset)) matched.push_back(&asset);
    std::ranges::sort(matched, {}, [](const EditorAsset* asset) { return asset->id; });
    const auto cursor = std::min(request.cursor, matched.size());
    const auto page_limit = std::min(request.page_limit, hard_page_limit);
    const auto returned = std::min(page_limit, matched.size() - cursor);
    const auto page_end = cursor + returned;
    const auto has_more = page_end < matched.size();

    Json panel_actions = Json::array();
    if (cursor > 0U) panel_actions.push_back({
        {"id", "asset-browser.previous-page"}, {"label", "Previous Page"},
        {"dispatch", "editor-ui-ephemeral"}, {"handler", "EditorUi.set_asset_browser_cursor"},
        {"binding", {{"kind", "editor-asset-browser-page"}, {"direction", "previous"},
            {"cursor", cursor > page_limit ? cursor - page_limit : 0U}, {"pageLimit", page_limit}}}});
    if (has_more) panel_actions.push_back({
        {"id", "asset-browser.next-page"}, {"label", "Next Page"},
        {"dispatch", "editor-ui-ephemeral"}, {"handler", "EditorUi.set_asset_browser_cursor"},
        {"binding", {{"kind", "editor-asset-browser-page"}, {"direction", "next"},
            {"cursor", page_end}, {"pageLimit", page_limit}}}});
    Json nodes = Json::array({Json{
        {"id", "editor.panel.assets"}, {"parentId", nullptr}, {"role", "grid"},
        {"label", "Asset Browser"},
        {"state", {{"visible", true}, {"enabled", true}, {"editable", false}}},
        {"binding", {{"kind", "editor-asset-browser-query"}, {"query", query}}},
        {"actions", std::move(panel_actions)}}});
    const auto active_job = Json::parse(active_asset_job_json(), nullptr, false);
    const auto job_state = active_job.is_object() ? active_job.value("state", std::string{}) : std::string{};
    const auto asset_job_busy = active_job.is_object() && active_job.value("valid", false) &&
        (job_state == "queued" || job_state == "running");
    const auto source_revision = asset_registry_.revision();
    const auto handler_action = [source_revision, asset_job_busy](
                                   const std::string_view id, const std::string_view label,
                                   const std::string_view operation, const std::string_view handler,
                                   const std::string& asset_id) {
        return Json{{"id", id}, {"label", label}, {"dispatch", "existing-editor-model"},
            {"handler", handler}, {"requiresSelectedAssetId", asset_id},
            {"binding", {{"kind", "editor-asset-action"}, {"operation", operation},
                {"assetId", asset_id}, {"sourceRevision", source_revision}}},
            {"state", {{"enabled", !asset_job_busy}, {"busy", asset_job_busy}}}};
    };
    for (auto index = cursor; index < page_end; ++index) {
        const auto& asset = *matched[index];
        Json actions = Json::array();
        const auto selected_asset = asset.id == selected_asset_id_;
        actions.push_back({
            {"id", "asset.select"}, {"label", "Select"},
            {"dispatch", "existing-editor-model"}, {"handler", "EditorModel.select_asset"},
            {"binding", {{"kind", "editor-asset-selection"}, {"assetId", asset.id},
                {"sourceRevision", source_revision}}},
            {"state", {{"enabled", true}, {"busy", false}}}});
        if (selected_asset) {
            actions.push_back(handler_action("asset.import", "Import", "import",
                "EditorModel.import_selected_asset", asset.id));
            actions.push_back(handler_action("asset.inspect", "Inspect", "inspect",
                "EditorModel.inspect_selected_asset", asset.id));
            actions.push_back(handler_action("asset.build-preview", "Build Preview", "build-preview",
                "EditorModel.generate_selected_asset_thumbnail", asset.id));
            actions.push_back(handler_action("asset.cook", "Cook", "cook",
                "EditorModel.cook_selected_asset", asset.id));
        }
        Json presentation{{"kind", "asset-card"}, {"primaryActionId", "asset.select"},
            {"inlineActionIds", selected_asset ?
                Json::array({"asset.import", "asset.inspect", "asset.build-preview", "asset.cook"}) : Json::array()}};
        if (!asset.thumbnail_uri.empty()) presentation["imageSource"] = asset.thumbnail_uri;
        nodes.push_back({
            {"id", "editor.asset." + asset.id}, {"parentId", "editor.panel.assets"},
            {"role", "griditem"}, {"label", asset.name},
            {"presentation", std::move(presentation)},
            {"asset", {{"id", asset.id}, {"kind", asset.kind}, {"source", asset.source},
                {"available", asset.available}, {"importState", asset.import_state},
                {"license", asset.license},
                {"thumbnail", {{"uri", asset.thumbnail_uri}, {"strategy", asset.thumbnail_strategy},
                    {"cached", asset.thumbnail_cached}}}}},
            {"state", {{"visible", true}, {"enabled", true}, {"editable", false},
                {"selected", selected_asset}, {"busy", selected_asset && asset_job_busy}}},
            {"actions", std::move(actions)}});
    }

    return Json{
        {"schemaVersion", "noemancer.ui-document/0.1"},
        {"documentId", "editor.asset-browser"}, {"valid", true}, {"code", "ok"},
        {"revision", asset_registry_.revision()},
        {"selection", {{"assetId", selected_asset_id_.empty() ? Json(nullptr) : Json(selected_asset_id_)}}},
        {"query", {{"text", query}, {"sourceBytes", request.query.size()},
            {"byteLimit", hard_query_bytes}, {"truncated", request.query.size() > query_bytes}}},
        {"page", {{"total", assets_.size()}, {"matched", matched.size()}, {"returned", returned},
            {"cursor", cursor}, {"requestedCursor", request.cursor},
            {"limit", page_limit}, {"requestedLimit", request.page_limit}, {"hardLimit", hard_page_limit},
            {"nextCursor", has_more ? Json(page_end) : Json(nullptr)}, {"truncated", has_more}}},
        {"nodes", std::move(nodes)}}.dump();
}

std::size_t EditorModel::selected_object_index() const noexcept {
    for (std::size_t index = 0; index < objects_.size(); ++index) {
        if (objects_[index].id == selected_object_id_) return index;
    }
    return objects_.size();
}

const EditorObject& EditorModel::selected_object() const {
    const auto index = selected_object_index();
    if (index >= objects_.size()) {
        throw std::out_of_range("Editor selection is outside the object list");
    }
    return objects_.at(index);
}

const std::vector<std::string>& EditorModel::selected_object_ids() const noexcept { return selected_object_ids_; }
bool EditorModel::is_object_selected(const std::string_view entity_id) const noexcept {
    return std::ranges::find(selected_object_ids_,entity_id)!=selected_object_ids_.end();
}

std::optional<EditorViewportCamera> EditorModel::viewport_camera() const {
    for (const auto& entity:world_.entity_views())
        if (entity.camera&&entity.camera->primary&&entity.transform) return EditorViewportCamera{*entity.transform,*entity.camera};
    return std::nullopt;
}

void EditorModel::select_object(const std::size_t index,const bool additive) {
    if (index < objects_.size()) {
        selected_object_id_ = objects_[index].id;
        if(!additive) selected_object_ids_={selected_object_id_};
        else if(const auto found=std::ranges::find(selected_object_ids_,selected_object_id_);found!=selected_object_ids_.end()) {
            if(selected_object_ids_.size()>1) {selected_object_ids_.erase(found);selected_object_id_=selected_object_ids_.back();}
        } else selected_object_ids_.push_back(selected_object_id_);
        refresh_inspector();
    }
}

bool EditorModel::select_object(const std::string_view entity_id,const bool additive) {
    const auto found = std::ranges::find(objects_, entity_id, &EditorObject::id);
    if (found == objects_.end()) return false;
    select_object(static_cast<std::size_t>(std::distance(objects_.begin(),found)),additive);
    return true;
}

void EditorModel::select_asset(const std::size_t index) noexcept {
    if (index < assets_.size()) selected_asset_id_ = assets_[index].id;
}

bool EditorModel::select_asset(const std::string_view asset_id) noexcept {
    const auto found=std::ranges::find(assets_,asset_id,&EditorAsset::id);
    if(found==assets_.end())return false;
    selected_asset_id_=found->id;return true;
}

std::size_t EditorModel::selected_asset_index() const noexcept {
    for (std::size_t index = 0; index < assets_.size(); ++index) {
        if (assets_[index].id == selected_asset_id_) return index;
    }
    return assets_.size();
}

const EditorAsset* EditorModel::selected_asset() const noexcept {
    const auto index = selected_asset_index();
    return index < assets_.size() ? &assets_[index] : nullptr;
}

std::string EditorModel::selected_asset_inspection_json() const {
    const auto* asset = selected_asset();
    return asset == nullptr ? std::string{"{}"} : asset_registry_.inspect_json(asset->id);
}

std::string EditorModel::selected_animation_graph_authoring_json() const {
    Json result={{"schemaVersion","noemancer.animation-graph-authoring/0.2"},{"valid",false},
        {"code","editor.animation-graph-not-selected"},{"asset",nullptr},{"fingerprint",nullptr},
        {"document",nullptr},{"canvas",nullptr},{"tool",{{"patchCommand","animation.graph.patch"},
            {"operations",{"createNode","deleteNode","connectBlend1DChild","disconnectBlend1DChild",
                "setNodePosition","setLayerWeight","setLayerWeightParameter","setMaskJointWeight"}},
            {"transaction","preview-fingerprint-then-atomic-commit"},{"undoCommand","asset.source.undo"},
            {"redoCommand","asset.source.redo"},{"maximumOperations",animation_graph_patch_max_operations}}}};
    const auto* asset=selected_asset();
    if(asset==nullptr)return result.dump();
    if(asset->kind!="AnimationGraph"&&!asset->source.ends_with(".animation-graph.json"))
        return result.dump();
    const auto inspection=Json::parse(asset_registry_.inspect_json(asset->id),nullptr,false);
    const auto metadata=inspection.is_object()?inspection.value("importedMetadata",Json(nullptr)):Json(nullptr);
    if(!inspection.is_object()||!inspection.value("valid",false)||!metadata.is_object()||
       metadata.value("format",std::string{})!=animation_graph_schema||
       !metadata.contains("document")||!metadata.at("document").is_object())return result.dump();
    const auto parsed=AnimationGraphCodec::parse_json(metadata.at("document").dump());
    if(!parsed){result["code"]=parsed.code;result["detail"]=parsed.detail;return result.dump();}
    if(parsed.document->asset_id!=asset->id) {
        result["code"]="editor.animation-graph-identity-mismatch";
        result["detail"]="The Registry asset ID and Animation Graph document assetId must match.";
        return result.dump();
    }
    AnimationGraphCanvasModel canvas(*parsed.document);
    result["valid"]=true;result["code"]="ok";
    result["asset"]={{"id",asset->id},{"name",asset->name},{"source",asset->source}};
    result["fingerprint"]=AnimationGraphPatch::fingerprint(*parsed.document);
    result["document"]=Json::parse(AnimationGraphCodec::write_canonical_json(*parsed.document));
    result["canvas"]=Json::parse(canvas.projection_json());
    return result.dump();
}

std::string EditorModel::apply_selected_animation_graph_patch(
    const std::vector<AnimationGraphPatchOperation>& operations,
    const std::string_view expected_fingerprint,const bool dry_run) {
    const auto* asset=selected_asset();
    if(asset==nullptr)return Json{{"success",false},{"code","editor.animation-graph-not-selected"}}.dump();
    Json serialized=Json::array();
    for(const auto& operation:operations) {
        Json value={{"operation",operation.operation}};
        if(operation.operation=="setNodePosition")value.update({{"nodeId",operation.node_id},{"x",operation.x},{"y",operation.y}});
        else if(operation.operation=="setLayerWeight")value.update({{"layerId",operation.layer_id},{"weight",operation.weight}});
        else if(operation.operation=="setLayerWeightParameter")value.update({{"layerId",operation.layer_id},{"parameter",operation.parameter}});
        else if(operation.operation=="setMaskJointWeight")value.update({{"maskId",operation.mask_id},{"jointName",operation.joint_name},{"weight",operation.weight}});
        else if(operation.operation=="createNode") {
            value.update({{"nodeId",operation.node_id},{"kind",operation.node_kind}});
            if(operation.node_kind=="clip")value.update({{"clipAsset",operation.clip_asset},{"looping",operation.looping}});
            else if(operation.node_kind=="state-machine")value["stateMachineAsset"]=operation.state_machine_asset;
            else if(operation.node_kind=="blend-1d") {
                value["parameter"]=operation.parameter;value["children"]=Json::array();
                for(const auto& child:operation.children)value["children"].push_back({{"nodeId",child.node_id},{"threshold",child.threshold}});
            }
        } else if(operation.operation=="deleteNode")value["nodeId"]=operation.node_id;
        else if(operation.operation=="connectBlend1DChild")value.update({{"blendNodeId",operation.node_id},
            {"childNodeId",operation.child_node_id},{"threshold",operation.threshold}});
        else if(operation.operation=="disconnectBlend1DChild")value.update({{"blendNodeId",operation.node_id},
            {"childNodeId",operation.child_node_id}});
        serialized.push_back(std::move(value));
    }
    CommandRegistry commands(world_,asset_registry_);
    const auto invocation=commands.invoke("animation.graph.patch",Json{{"assetId",asset->id},{"manager","editor.animation-graph"},
        {"expectedFingerprint",expected_fingerprint},{"dryRun",dry_run},{"operations",std::move(serialized)}}.dump());
    const auto envelope=Json::parse(invocation.output_json,nullptr,false);
    const auto output=envelope.is_object()?envelope.value("result",Json::object()):Json::object();
    const auto source_receipt=output.value("sourceReceipt",Json(nullptr));
    if(invocation.exit_code==0&&!dry_run&&output.value("success",false)&&source_receipt.is_object()&&
       source_receipt.value("success",false)) {record_edit(EditDomain::asset_source);refresh();}
    return invocation.output_json;
}

std::string EditorModel::selected_tilemap_authoring_json() const {
    const auto* asset=selected_asset();
    Json result={{"schemaVersion","noemancer.tilemap-authoring-document/0.1"},{"valid",false},{"code","editor.tilemap-not-selected"},
        {"asset",nullptr},{"fingerprint",nullptr},{"map",nullptr},{"layers",Json::array()},{"palette",nullptr},{"bindings",Json::array()},
        {"tool",{{"command","asset.tilemap.stroke"},{"modes",{"paint","erase"}},{"maximumCellsPerStroke",4096},
            {"transaction","preview-fingerprint-then-atomic-commit"},{"regionCommand","asset.tilemap.region"},
            {"regionShapes",{"rectangle","flood"}},{"emptyFloodPolicy","reject-unbounded"},
            {"paletteCommand","asset.tile-palette.autotile"},{"autotileNeighborBits",{{"north",1},{"east",2},{"south",4},{"west",8}}},
            {"undoCommand","asset.source.undo"},{"redoCommand","asset.source.redo"}}}};
    if(asset==nullptr)return result.dump();
    if(asset->kind!="Tilemap"&&!asset->source.ends_with(".tilemap.json"))
        return result.dump();
    const auto inspection=Json::parse(asset_registry_.inspect_json(asset->id),nullptr,false);
    const auto imported_metadata=inspection.is_object()
        ? inspection.value("importedMetadata",Json(nullptr))
        : Json(nullptr);
    if(!inspection.is_object()||!inspection.value("valid",false)||!imported_metadata.is_object()||
       imported_metadata.value("format",std::string{})!="noemancer.tilemap/0.1"||
       !imported_metadata.contains("document")||!imported_metadata.at("document").is_object())return result.dump();
    const auto& document=imported_metadata.at("document");
    const auto parsed=TilemapAssetCodec::parse_tilemap_json(document.dump());if(!parsed)return result.dump();
    const auto palette_id=parsed.document->palette_asset;const auto* palette_asset=asset_registry_.find(palette_id);
    if(palette_asset==nullptr)return result.dump();
    const auto palette_inspection=Json::parse(asset_registry_.inspect_json(palette_id),nullptr,false);
    const auto palette_metadata=palette_inspection.is_object()
        ? palette_inspection.value("importedMetadata",Json(nullptr))
        : Json(nullptr);
    if(!palette_inspection.is_object()||!palette_inspection.value("valid",false)||!palette_metadata.is_object()||
       !palette_metadata.contains("document")||!palette_metadata.at("document").is_object())return result.dump();
    const auto parsed_palette=TilemapAssetCodec::parse_palette_json(palette_metadata.at("document").dump());
    if(!parsed_palette)return result.dump();
    const auto render_payload=palette_inspection.value("renderPayload",Json(nullptr));
    if(!render_payload.is_object()||!render_payload.contains("spriteAsset"))return result.dump();
    result["valid"]=true;result["code"]="ok";result["asset"]={{"id",asset->id},{"name",asset->name},{"source",asset->source}};
    result["fingerprint"]=TilemapAssetCodec::tilemap_fingerprint(*parsed.document);
    result["map"]={{"cellSize",{parsed.document->cell_width,parsed.document->cell_height}},
        {"chunkSize",{parsed.document->chunk_width,parsed.document->chunk_height}}};
    result["layers"]=document.at("layers");result["palette"]={{"assetId",palette_id},
        {"fingerprint",TilemapAssetCodec::palette_fingerprint(*parsed_palette.document)},
        {"spriteAsset",render_payload.at("spriteAsset")},
        {"tiles",palette_metadata.at("document").at("tiles")}};
    for(const auto& entity:world_.entity_views())if(entity.tilemap_renderer&&entity.tilemap_renderer->tilemap_asset==asset->id&&entity.transform)
        result["bindings"].push_back({{"entityId",entity.id},{"position",{entity.transform->x,entity.transform->y,entity.transform->z}},
            {"scale",{entity.transform->scale_x,entity.transform->scale_y,entity.transform->scale_z}},
            {"rotation",{entity.transform->rotation_x,entity.transform->rotation_y,entity.transform->rotation_z,entity.transform->rotation_w}}});
    return result.dump();
}

std::string EditorModel::apply_selected_tilemap_stroke(const std::string_view layer_id,const std::vector<TilemapCellEdit>& edits,
    const std::string_view expected_fingerprint,const bool dry_run) {
    const auto* asset=selected_asset();if(asset==nullptr)return Json{{"success",false},{"code","editor.tilemap-not-selected"}}.dump();
    Json values=Json::array();for(const auto& edit:edits)values.push_back({{"x",edit.x},{"y",edit.y},
        {"operation",edit.tile_id?"paint":"erase"},{"tileId",edit.tile_id?Json(*edit.tile_id):Json(nullptr)},
        {"flipX",edit.flip_x},{"flipY",edit.flip_y}});
    CommandRegistry commands(world_,asset_registry_);const auto invocation=commands.invoke("asset.tilemap.stroke",Json{{"assetId",asset->id},
        {"layerId",layer_id},{"edits",std::move(values)},{"manager","editor.tile-brush"},
        {"expectedFingerprint",expected_fingerprint},{"dryRun",dry_run}}.dump());
    const auto envelope=Json::parse(invocation.output_json,nullptr,false);const auto output=envelope.value("result",Json::object());
    if(invocation.exit_code==0&&!dry_run&&output.is_object()&&output.value("success",false)) {record_edit(EditDomain::asset_source);refresh();}
    return invocation.output_json;
}

std::string EditorModel::apply_selected_tilemap_region(const std::string_view shape,const std::string_view layer_id,
    const std::array<std::int32_t,2> first,const std::optional<std::array<std::int32_t,2>> second,
    std::optional<std::string> tile_id,const bool flip_x,const bool flip_y,const std::string_view expected_fingerprint,const bool dry_run) {
    const auto* asset=selected_asset();if(asset==nullptr)return Json{{"success",false},{"code","editor.tilemap-not-selected"}}.dump();
    Json arguments={{"assetId",asset->id},{"layerId",layer_id},{"shape",shape},{"first",{{"x",first[0]},{"y",first[1]}}},
        {"operation",tile_id?"paint":"erase"},{"tileId",tile_id.value_or("")},{"flipX",flip_x},{"flipY",flip_y},
        {"manager","editor.tile-region"},{"expectedFingerprint",expected_fingerprint},{"includeEdits",true},{"dryRun",dry_run}};
    if(second)arguments["second"]={{"x",second->at(0)},{"y",second->at(1)}};
    CommandRegistry commands(world_,asset_registry_);const auto invocation=commands.invoke("asset.tilemap.region",arguments.dump());
    const auto envelope=Json::parse(invocation.output_json,nullptr,false);const auto output=envelope.value("result",Json::object());
    if(invocation.exit_code==0&&!dry_run&&output.is_object()&&output.value("success",false)){record_edit(EditDomain::asset_source);refresh();}
    return invocation.output_json;
}

std::string EditorModel::apply_selected_tile_palette_autotile(const std::string_view tile_id,const std::string_view autotile_group,
    const std::vector<TileAutotileVariant>& variants,const std::string_view expected_fingerprint,const bool dry_run) {
    const auto authoring=Json::parse(selected_tilemap_authoring_json(),nullptr,false);
    if(!authoring.is_object()||!authoring.value("valid",false))return Json{{"success",false},{"code","editor.tilemap-not-selected"}}.dump();
    Json values=Json::array();for(const auto& variant:variants)values.push_back({{"mask",variant.neighbor_mask},{"frame",variant.frame_id}});
    CommandRegistry commands(world_,asset_registry_);const auto invocation=commands.invoke("asset.tile-palette.autotile",Json{
        {"assetId",authoring.at("palette").at("assetId")},{"tileId",tile_id},{"autotileGroup",autotile_group},{"variants",std::move(values)},
        {"manager","editor.tile-palette"},{"expectedFingerprint",expected_fingerprint},{"dryRun",dry_run}}.dump());
    const auto envelope=Json::parse(invocation.output_json,nullptr,false);const auto output=envelope.value("result",Json::object());
    if(invocation.exit_code==0&&!dry_run&&output.is_object()&&output.value("success",false)){record_edit(EditDomain::asset_source);refresh();}
    return invocation.output_json;
}

std::string EditorModel::asset_registry_status_json() const { return asset_registry_.registry_json(); }

std::string EditorModel::scripting_status_json() const {
    auto project=Json::parse(world_.scripting_project_observation_json(),nullptr,false);
    auto host=Json::parse(world_.scripting_observation_json(),nullptr,false);
    if(project.is_discarded())project=nullptr;
    if(host.is_discarded())host=nullptr;
    auto types=Json::parse(world_.scripting_project_types_json(),nullptr,false);
    if(types.is_discarded())types=nullptr;
    auto debug_attach=Json::parse(world_.scripting_debug_attach_json(),nullptr,false);
    if(debug_attach.is_discarded())debug_attach=nullptr;
    auto debug_session=Json::parse(world_.scripting_debug_session_status_json(),nullptr,false);
    if(debug_session.is_discarded())debug_session=nullptr;
    return Json{{"schemaVersion","noemancer.editor-scripting-status/0.1"},{"project",std::move(project)},
        {"host",std::move(host)},{"typeCatalog",std::move(types)},{"debugAttach",std::move(debug_attach)},
        {"debugSession",std::move(debug_session)}}.dump();
}

std::string EditorModel::compile_scripts_json(const std::string_view configuration) {
    return world_.scripting_project_compile_json(configuration);
}

EditorSceneAction EditorModel::refresh_assets() {
    const auto before=asset_registry_.revision();const auto success=asset_registry_.refresh();refresh();
    return {success,success?"ok":"asset.refresh-failed",success?"Asset roots rescanned.":"Asset rescan completed with errors.",{},asset_registry_.revision()-before};
}

EditorSceneAction EditorModel::import_selected_asset() {
    const auto* asset=selected_asset();if(asset==nullptr)return {false,"asset.no-selection","No asset is selected."};
    const auto roots=asset_registry_.asset_roots();if(roots.empty())return {false,"asset.root-missing","The project has no asset root."};
    AssetJobRequest request{.kind=AssetJobKind::import,.asset_id=asset->id,.source_uri=asset->source,
        .input_fingerprint=asset->content_hash,.source_revision=asset_registry_.revision(),.description="Import selected asset"};
    AssetWorkflowConfig config{.asset_roots=roots,.artifact_root=roots.front().parent_path()/"generated"/"asset-workflow"};
    const auto result=asset_jobs_.submit(request,make_asset_workflow_executor(std::move(config)));
    if(result.accepted){active_asset_job_id_=result.job_id;active_asset_job_kind_=AssetJobKind::import;
        active_asset_job_base_revision_=asset_registry_.revision();active_asset_job_reconciled_=false;}
    return {result.accepted,result.code,result.detail,asset->id,asset_registry_.revision()};
}

EditorSceneAction EditorModel::inspect_selected_asset() {
    const auto* asset=selected_asset();if(asset==nullptr)return {false,"asset.no-selection","No asset is selected."};
    const auto roots=asset_registry_.asset_roots();if(roots.empty())return {false,"asset.root-missing","The project has no asset root."};
    AssetJobRequest request{.kind=AssetJobKind::inspect,.asset_id=asset->id,.source_uri=asset->source,
        .input_fingerprint=asset->content_hash,.source_revision=asset_registry_.revision(),.description="Inspect selected asset"};
    AssetWorkflowConfig config{.asset_roots=roots,.artifact_root=roots.front().parent_path()/"generated"/"asset-workflow"};
    const auto result=asset_jobs_.submit(request,make_asset_workflow_executor(std::move(config)));
    if(result.accepted){active_asset_job_id_=result.job_id;active_asset_job_kind_=AssetJobKind::inspect;
        active_asset_job_base_revision_=asset_registry_.revision();active_asset_job_reconciled_=false;}
    return {result.accepted,result.code,result.detail,asset->id,asset_registry_.revision()};
}

EditorSceneAction EditorModel::generate_selected_asset_thumbnail() {
    const auto* selected=selected_asset();if(selected==nullptr)return {false,"asset.no-selection","No asset is selected."};
    const auto* record=asset_registry_.find(selected->id);if(record==nullptr)return {false,"asset.not-found","The selected asset record no longer exists."};
    const auto roots=asset_registry_.asset_roots();if(roots.empty())return {false,"asset.root-missing","The project has no asset root."};
    const auto record_copy=*record;const auto source_path=asset_registry_.source_path(*record);
    const auto generated_root=roots.front().parent_path()/"generated";
    AssetJobRequest request{.kind=AssetJobKind::thumbnail,.asset_id=record->id,.source_uri=record->uri,
        .input_fingerprint=record->content_hash,.target_profile="editor-160x96-v1",
        .source_revision=asset_registry_.revision(),.description="Generate selected asset thumbnail"};
    const auto result=asset_jobs_.submit(request,[record_copy,source_path,generated_root](
        const AssetJobRequest&,AssetJobContext& context) {
        if(!context.report(0.08F,"plan","Planning a deterministic thumbnail."))
            return AssetJobExecutionResult{.cancelled=true,.code="thumbnail.cancelled"};
        auto plan=plan_thumbnail(record_copy);
        if(const auto cache=thumbnail_cache_path(generated_root,plan.artifact_uri);cache&&std::filesystem::is_regular_file(*cache))
            plan=plan_thumbnail(ThumbnailSource{.asset=record_copy,.artifact_uris={plan.artifact_uri}});
        if(!plan.valid)return AssetJobExecutionResult{.code=plan.code,.detail=plan.detail,.diagnostics=plan.diagnostics};
        if(context.cancellation_requested())return AssetJobExecutionResult{.cancelled=true,.code="thumbnail.cancelled"};
        static_cast<void>(context.report(0.35F,"render","Producing the bounded PNG preview."));
        ThumbnailExecutionOptions options;
        options.read_source=[source_path](const std::string_view){return read_thumbnail_source(source_path,64U*1024U*1024U);};
        options.write_artifact=[generated_root](const std::string_view uri,const std::string_view format,
            const std::span<const std::uint8_t> bytes,std::string& code,std::string& detail) {
            if(format!="image/png"){code="thumbnail.format-unsupported";detail="The Editor cache accepts PNG previews.";return false;}
            return write_thumbnail_artifact(generated_root,uri,bytes,code,detail);
        };
        const auto receipt=execute_thumbnail(plan,options);
        if(!receipt.success)return AssetJobExecutionResult{.code=receipt.code,.detail=receipt.detail,.diagnostics=receipt.diagnostics};
        static_cast<void>(context.report(1.0F,"complete","Thumbnail artifact is ready."));
        return AssetJobExecutionResult{.success=true,.code=receipt.code,.detail=receipt.detail,
            .diagnostics=receipt.diagnostics,.artifact_uris={receipt.artifact_uri}};
    });
    if(result.accepted){active_asset_job_id_=result.job_id;active_asset_job_kind_=AssetJobKind::thumbnail;
        active_asset_job_base_revision_=asset_registry_.revision();active_asset_job_reconciled_=false;}
    return {result.accepted,result.code,result.detail,record->id,asset_registry_.revision()};
}

EditorSceneAction EditorModel::cook_selected_asset(const std::string_view target_profile) {
    const auto* asset=selected_asset();if(asset==nullptr) return {false,"asset.no-selection","No asset is selected."};
    const auto roots=asset_registry_.asset_roots();
    if(roots.empty())return {false,"asset.root-missing","The project has no asset root."};
    AssetJobRequest request{.kind=AssetJobKind::cook,.asset_id=asset->id,.source_uri=asset->source,
        .input_fingerprint=asset->content_hash,.target_profile=std::string(target_profile),
        .source_revision=asset_registry_.revision(),.description="Cook selected asset"};
    const auto asset_id=asset->id;
    const auto result=asset_jobs_.submit(request,[roots,asset_id,target=std::string(target_profile)](
        const AssetJobRequest&,AssetJobContext& context) {
        static_cast<void>(context.report(0.08F,"scan","Building an isolated asset registry snapshot."));
        AssetRegistry isolated(roots.front());
        if(context.cancellation_requested())
            return AssetJobExecutionResult{.cancelled=true,.code="asset.cook.cancelled"};
        for(std::size_t index=1;index<roots.size();++index) {
            if(context.cancellation_requested())return AssetJobExecutionResult{.cancelled=true,.code="asset.cook.cancelled"};
            static_cast<void>(isolated.add_root(roots[index]));
        }
        static_cast<void>(context.report(0.30F,"plan","Validating the deterministic Cook plan."));
        const auto plan=Json::parse(isolated.cook_plan_json({asset_id},target),nullptr,false);
        if(plan.is_discarded()||!plan.value("valid",false))return AssetJobExecutionResult{.code="asset.cook-plan-invalid",
            .detail="The selected asset cannot be cooked.",.diagnostics={plan.is_discarded()?"Cook plan returned invalid JSON.":plan.dump()}};
        if(context.cancellation_requested())return AssetJobExecutionResult{.cancelled=true,.code="asset.cook.cancelled"};
        static_cast<void>(context.report(0.55F,"cook","Writing content-addressed Cook artifacts."));
        const auto receipt=Json::parse(isolated.apply_cook_plan_json(plan.dump(),false),nullptr,false);
        if(receipt.is_discarded()||!receipt.value("success",false))return AssetJobExecutionResult{.code=receipt.is_discarded()?"asset.cook-invalid-receipt":receipt.value("code","asset.cook-failed"),
            .detail=receipt.is_discarded()?"Cook returned invalid JSON.":receipt.value("detail","Asset Cook failed."),
            .diagnostics={receipt.is_discarded()?"Invalid Cook receipt.":receipt.dump()}};
        std::vector<std::string> artifacts;
        for(const auto& uri:receipt.value("artifacts",Json::array()))if(uri.is_string())artifacts.push_back(uri.get<std::string>());
        static_cast<void>(context.report(1.0F,"complete","Cook artifacts committed."));
        return AssetJobExecutionResult{.success=true,.code="ok",.detail=receipt.value("detail","Asset Cook completed."),
            .artifact_uris=std::move(artifacts)};
    });
    if(result.accepted){active_asset_job_id_=result.job_id;active_asset_job_kind_=AssetJobKind::cook;
        active_asset_job_base_revision_=asset_registry_.revision();active_asset_job_reconciled_=false;}
    return {result.accepted,result.code,result.detail,asset->id,asset_registry_.revision()};
}

std::string EditorModel::active_asset_job_json() const {
    if(active_asset_job_id_.empty())return Json{{"schemaVersion","noemancer.asset-job-observation/0.1"},
        {"state","idle"},{"active",false}}.dump();
    return asset_jobs_.observation_json(active_asset_job_id_);
}

std::optional<EditorSceneAction> EditorModel::reconcile_active_asset_job() {
    if(active_asset_job_id_.empty()||active_asset_job_reconciled_)return std::nullopt;
    const auto snapshot=asset_jobs_.snapshot(active_asset_job_id_);if(!snapshot)return std::nullopt;
    if(snapshot->state==AssetJobState::queued||snapshot->state==AssetJobState::running)return std::nullopt;
    active_asset_job_reconciled_=true;
    if(active_asset_job_kind_==AssetJobKind::thumbnail&&snapshot->state==AssetJobState::succeeded) {
        refresh();
        return EditorSceneAction{true,"thumbnail.ready","Thumbnail artifact is ready for the Asset Browser.",
            snapshot->asset_id,asset_registry_.revision()};
    }
    if(active_asset_job_kind_!=AssetJobKind::import||snapshot->state!=AssetJobState::succeeded)return std::nullopt;
    if(asset_registry_.revision()!=active_asset_job_base_revision_)
        return EditorSceneAction{false,"asset.import-live-revision-conflict",
            "Import evidence is valid, but the live Asset Registry changed; rescan before applying it.",snapshot->asset_id,asset_registry_.revision()};
    const auto success=asset_registry_.refresh();refresh();
    return EditorSceneAction{success,success?"asset.import-applied":"asset.import-applied-with-errors",
        success?"Background import evidence applied to the live Asset Registry.":"Import applied, but the rescan reported asset errors.",
        snapshot->asset_id,asset_registry_.revision()};
}

std::string EditorModel::selected_asset_repair_json() const {
    AssetRepairInput input{.assets=asset_registry_.records(),.registry_revision=asset_registry_.revision(),
        .max_diagnostics=128U,.max_actions=128U,.max_text_bytes=512U,.max_json_bytes=32U*1024U};
    if(!active_asset_job_id_.empty())if(const auto snapshot=asset_jobs_.snapshot(active_asset_job_id_))
        input.jobs.push_back({.asset_id=snapshot->asset_id,.kind=AssetJobQueue::kind_name(snapshot->kind),
            .state=AssetJobQueue::state_name(snapshot->state),.input_fingerprint=snapshot->input_fingerprint,
            .source_revision=snapshot->source_revision,.code=snapshot->code,.detail=snapshot->detail});
    return asset_repair_report_json(diagnose_asset_repairs(input));
}

EditorSceneAction EditorModel::execute_selected_asset_repair(const std::string_view action_id) {
    AssetRepairInput input{.assets=asset_registry_.records(),.registry_revision=asset_registry_.revision(),
        .max_diagnostics=128U,.max_actions=128U};
    if(!active_asset_job_id_.empty())if(const auto snapshot=asset_jobs_.snapshot(active_asset_job_id_))
        input.jobs.push_back({.asset_id=snapshot->asset_id,.kind=AssetJobQueue::kind_name(snapshot->kind),
            .state=AssetJobQueue::state_name(snapshot->state),.input_fingerprint=snapshot->input_fingerprint,
            .source_revision=snapshot->source_revision,.code=snapshot->code,.detail=snapshot->detail});
    const auto report=diagnose_asset_repairs(input);
    const auto found=std::ranges::find(report.plan.actions,action_id,&AssetRepairAction::action_id);
    if(found==report.plan.actions.end())return {false,"asset.repair-action-not-found","The repair action is stale or unavailable."};
    if(!selected_asset_id_.empty()&&found->asset_id!=selected_asset_id_&&found->related_asset_id!=selected_asset_id_)
        return {false,"asset.repair-selection-mismatch","The repair action does not belong to the selected asset."};
    switch(found->kind) {
    case AssetRepairActionKind::rescan:return refresh_assets();
    case AssetRepairActionKind::reimport:return import_selected_asset();
    case AssetRepairActionKind::inspect:return inspect_selected_asset();
    case AssetRepairActionKind::retry_cook:
        if(!active_asset_job_id_.empty())if(const auto snapshot=asset_jobs_.snapshot(active_asset_job_id_);
            snapshot&&snapshot->asset_id==found->asset_id&&snapshot->state==AssetJobState::failed)return retry_active_asset_job();
        return cook_selected_asset();
    case AssetRepairActionKind::reveal_dependent: {
        const auto id=found->asset_id;
        const auto asset=std::ranges::find(assets_,id,&EditorAsset::id);
        if(asset==assets_.end())return {false,"asset.repair-dependent-not-found","The related asset is not present."};
        select_asset(static_cast<std::size_t>(std::distance(assets_.begin(),asset)));
        return {true,"asset.repair-dependent-revealed","The related asset is now selected.",id,asset_registry_.revision()};
    }}
    return {false,"asset.repair-action-unsupported","The repair action is unsupported."};
}

EditorSceneAction EditorModel::cancel_active_asset_job() {
    if(active_asset_job_id_.empty())return {false,"asset.job-not-active","There is no active asset job."};
    const auto result=asset_jobs_.cancel(active_asset_job_id_);
    return {result.accepted,result.code,result.detail,{},asset_registry_.revision()};
}

EditorSceneAction EditorModel::retry_active_asset_job() {
    if(active_asset_job_id_.empty())return {false,"asset.job-not-active","There is no asset job to retry."};
    const auto result=asset_jobs_.retry(active_asset_job_id_);
    return {result.accepted,result.code,result.detail,{},asset_registry_.revision()};
}

void EditorModel::refresh() {
    const auto source = world_.scene_source_uri();
    if (source != observed_scene_source_) {
        observed_scene_source_ = source;
        saved_scene_json_ = world_.canonical_scene_json();
    }
    objects_.clear();
    std::string first_renderable_id;
    for (const auto& entity : world_.entity_views()) {
        if (first_renderable_id.empty() && entity.mesh_renderer && entity.transform) {
            first_renderable_id = entity.id;
        }
        objects_.push_back({
            .id = entity.id,
            .name = entity.display_name,
            .kind = entity.type,
            .parent_id = entity.parent_guid,
            .source = entity.source,
            .transform = entity.transform,
            .revision = entity.revision
        });
    }
    std::erase_if(selected_object_ids_,[&](const std::string& id){return std::ranges::find(objects_,id,&EditorObject::id)==objects_.end();});
    if (selected_object_index() >= objects_.size()) {
        const auto transform_object = std::find_if(objects_.begin(), objects_.end(), [](const EditorObject& object) {
            return object.transform.has_value();
        });
        selected_object_id_ = !first_renderable_id.empty()
            ? first_renderable_id
            : (transform_object != objects_.end()
                ? transform_object->id
                : (objects_.empty() ? std::string{} : objects_.front().id));
    }
    if(selected_object_ids_.empty()&&!selected_object_id_.empty()) selected_object_ids_.push_back(selected_object_id_);
    refresh_inspector();

    assets_.clear();
    for (const auto& asset : asset_registry_.records()) {
        const auto thumbnail=plan_thumbnail(asset);
        const auto generated_root=asset_registry_.asset_roots().empty()?std::filesystem::path{}:
            asset_registry_.asset_roots().front().parent_path()/"generated";
        const auto cache=generated_root.empty()?std::optional<std::filesystem::path>{}:
            thumbnail_cache_path(generated_root,thumbnail.artifact_uri);
        assets_.push_back({
            .id = asset.id,
            .name = asset.display_name,
            .kind = asset.kind,
            .source = asset.uri,
            .import_state = asset.import_state,
            .content_hash = asset.content_hash,
            .license = asset.license,
            .available = asset.available,
            .thumbnail_uri = thumbnail.valid ? thumbnail.artifact_uri : std::string{},
            .thumbnail_strategy = thumbnail.strategy,
            .thumbnail_cached = cache && std::filesystem::is_regular_file(*cache)
        });
    }
    if (selected_asset_index() >= assets_.size() && !assets_.empty()) {
        const auto model = std::find_if(assets_.begin(), assets_.end(), [](const EditorAsset& asset) {
            return asset.kind == "Model";
        });
        selected_asset_id_ = model != assets_.end() ? model->id : assets_.front().id;
    }
}

TransformUpdateResult EditorModel::set_selected_transform(const Transform transform) {
    const auto receipt=Json::parse(world_.edit_transform_json(selected_object().id,transform,world_.revision(),"editor.gizmo",false));
    TransformUpdateResult result{receipt.value("success",false),receipt.value("code","world.transform-edit-failed"),
        receipt.value("detail","Transform edit failed."),receipt.value("revisionAfter",world_.revision())};
    if(result.success) {record_edit(EditDomain::scene);refresh();} return result;
}

ActionReceipt EditorModel::set_selected_property(const std::string_view property, const std::string_view value_json) {
    const auto plan=world_.plan_property_update(selected_object().id,property,value_json,world_.revision(),"editor.inspector");
    const auto receipt=world_.apply_property_plan(plan,false);
    if(receipt.success) {record_edit(EditDomain::scene);refresh();}
    return receipt;
}

void EditorModel::refresh_inspector() {
    inspector_sections_.clear();
    if(selected_object_id_.empty()) return;
    const auto document=Json::parse(world_.inspector_document_json(selected_object_id_),nullptr,false);
    if(document.is_discarded()||!document.value("valid",false)) return;
    for(const auto& source_section:document.at("sections")) {
        InspectorSection section{.id=source_section.value("id",""),.component=source_section.value("component",""),
            .label=source_section.value("label",""),.default_expanded=source_section.value("defaultExpanded",false)};
        for(const auto& source_property:source_section.at("properties")) {
            const auto constraints=source_property.value("constraints",Json::object());
            InspectorProperty property{.id=source_property.value("id",""),.component=source_property.value("component",section.component),
                .property=source_property.value("property",""),.label=source_property.value("label",""),
                .value_type=source_property.value("valueType","string"),.control=source_property.value("control","label"),
                .value_json=source_property.contains("value")?source_property.at("value").dump():"null",
                .unit=constraints.value("unit",""),.minimum=constraints.value("minimum",0.0),
                .maximum=constraints.value("maximum",1.0),.step=constraints.value("step",0.05),
                .has_minimum=constraints.contains("minimum")||constraints.contains("minimumExclusive"),
                .has_maximum=constraints.contains("maximum"),.editable=source_property.value("editable",false)};
            if(constraints.contains("options")) for(const auto& option:constraints.at("options")) property.options.push_back(option.get<std::string>());
            if(section.component=="ManagedScript"&&property.property=="engine.entity.managedScript.typeName") {
                const auto catalog=Json::parse(world_.scripting_project_types_json(),nullptr,false);
                if(catalog.is_object())for(const auto& type:catalog.value("types",Json::array()))
                    property.options.push_back(type.value("fullName",std::string{}));
                std::erase(property.options,std::string{});
                if(!property.options.empty())property.control="combo";
            }
            section.properties.push_back(std::move(property));
        }
        inspector_sections_.push_back(std::move(section));
    }
}

void EditorModel::record_edit(const EditDomain domain) {
    edit_undo_timeline_.push_back(domain);edit_redo_timeline_.clear();
}

ActionReceipt EditorModel::undo() {
    const auto domain=!edit_undo_timeline_.empty()?edit_undo_timeline_.back():
        (asset_registry_.can_undo_text_source()?EditDomain::asset_source:EditDomain::scene);
    ActionReceipt result;
    if(domain==EditDomain::asset_source) {
        CommandRegistry commands(world_,asset_registry_);const auto invocation=commands.invoke("asset.source.undo",R"({"manager":"editor.undo"})");
        const auto envelope=Json::parse(invocation.output_json,nullptr,false);const auto receipt=envelope.value("result",Json::object());result={.success=receipt.value("success",false),.dry_run=false,
            .code=receipt.value("code","asset.undo-failed"),.detail=receipt.value("detail","Asset undo failed."),
            .operation_id="editor.asset-source.undo",.revision_before=world_.revision(),.revision_after=world_.revision()};
    } else result=world_.undo(world_.revision(),"editor.undo");
    if(result.success) {
        if(!edit_undo_timeline_.empty()){edit_undo_timeline_.pop_back();edit_redo_timeline_.push_back(domain);}
        refresh();
    }
    return result;
}

ActionReceipt EditorModel::redo() {
    const auto domain=!edit_redo_timeline_.empty()?edit_redo_timeline_.back():
        (asset_registry_.can_redo_text_source()?EditDomain::asset_source:EditDomain::scene);
    ActionReceipt result;
    if(domain==EditDomain::asset_source) {
        CommandRegistry commands(world_,asset_registry_);const auto invocation=commands.invoke("asset.source.redo",R"({"manager":"editor.redo"})");
        const auto envelope=Json::parse(invocation.output_json,nullptr,false);const auto receipt=envelope.value("result",Json::object());result={.success=receipt.value("success",false),.dry_run=false,
            .code=receipt.value("code","asset.redo-failed"),.detail=receipt.value("detail","Asset redo failed."),
            .operation_id="editor.asset-source.redo",.revision_before=world_.revision(),.revision_after=world_.revision()};
    } else result=world_.redo(world_.revision(),"editor.redo");
    if(result.success) {
        if(!edit_redo_timeline_.empty()){edit_redo_timeline_.pop_back();edit_undo_timeline_.push_back(domain);}
        refresh();
    }
    return result;
}

EditorSceneAction EditorModel::create_empty_entity(const std::string_view display_name,
                                                   const std::string_view parent_entity_id) {
    std::string entity_id;
    do {
        entity_id = "entity.editor." + std::to_string(generated_entity_sequence_++);
    } while (std::ranges::any_of(objects_, [&](const EditorObject& object) { return object.id == entity_id; }));
    const auto receipt = Json::parse(world_.edit_scene_entity_json("create",{},entity_id,display_name,parent_entity_id,{},
        false,world_.revision(),"editor.outliner",false));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.edit-failed"),
        receipt.value("detail","Scene edit failed."),receipt.value("entityId",entity_id),receipt.value("revisionAfter",world_.revision())};
    if (result.success) { record_edit(EditDomain::scene);refresh(); static_cast<void>(select_object(result.entity_id)); }
    return result;
}

EditorSceneAction EditorModel::duplicate_selected() {
    const auto copied=copy_selected();if(!copied.success) return copied;return paste_copied();
}

EditorSceneAction EditorModel::copy_selected() {
    if(selected_object_ids_.empty()) return {false,"world.entity-not-found","No entities are selected."};
    const auto scene=Json::parse(world_.canonical_scene_json());std::unordered_set<std::string> included(selected_object_ids_.begin(),selected_object_ids_.end());
    std::unordered_map<std::string,std::string> parents;for(const auto& entity:scene.at("entities")) parents[entity.at("guid").get<std::string>()]=
        entity.contains("parent")&&entity.at("parent").is_string()?entity.at("parent").get<std::string>():std::string{};
    std::vector<std::string> roots;for(const auto& id:selected_object_ids_) {auto parent=parents[id];bool ancestor_selected=false;
        while(!parent.empty()) {if(included.contains(parent)) {ancestor_selected=true;break;}parent=parents[parent];}if(!ancestor_selected) roots.push_back(id);}
    bool changed=true;while(changed) {changed=false;for(const auto& entity:scene.at("entities")) {
        const auto parent=entity.value("parent",Json(nullptr));
        if(parent.is_string()&&included.contains(parent.get<std::string>())&&included.insert(entity.at("guid").get<std::string>()).second) changed=true;
    }}
    Json entities=Json::array();for(const auto& entity:scene.at("entities")) if(included.contains(entity.at("guid").get<std::string>())) entities.push_back(entity);
    scene_clipboard_json_=Json{{"schemaVersion","noemancer.editor-scene-clipboard/0.1"},{"entities",std::move(entities)},
        {"selectedRoots",roots}}.dump();
    return {true,"ok","Copied "+std::to_string(included.size())+" scene entities.",selected_object_id_,world_.revision()};
}

bool EditorModel::can_paste() const noexcept { return !scene_clipboard_json_.empty(); }

EditorSceneAction EditorModel::paste_copied() {
    if(!can_paste()) return {false,"editor.clipboard-empty","The scene clipboard is empty."};
    auto clipboard=Json::parse(scene_clipboard_json_,nullptr,false);auto scene=Json::parse(world_.canonical_scene_json(),nullptr,false);
    if(clipboard.is_discarded()||scene.is_discarded()) return {false,"editor.clipboard-invalid","The scene clipboard cannot be parsed."};
    std::unordered_set<std::string> used;for(const auto& entity:scene.at("entities")) used.insert(entity.at("guid").get<std::string>());
    std::unordered_map<std::string,std::string> remap;
    for(const auto& source:clipboard.at("entities")) {const auto old_id=source.at("guid").get<std::string>();std::string new_id;
        do {new_id=old_id+".copy"+std::to_string(generated_entity_sequence_++);} while(used.contains(new_id));used.insert(new_id);remap.emplace(old_id,new_id);}
    std::unordered_set<std::string> roots;for(const auto& id:clipboard.at("selectedRoots")) roots.insert(id.get<std::string>());
    const auto remap_references=[&](const auto& self,Json& value)->void {if(value.is_string()) {const auto found=remap.find(value.get<std::string>());if(found!=remap.end()) value=found->second;}
        else if(value.is_array()) for(auto& child:value) self(self,child);else if(value.is_object()) for(auto& [key,child]:value.items()) {static_cast<void>(key);self(self,child);}};
    for(auto source:clipboard.at("entities")) {const auto old_id=source.at("guid").get<std::string>();remap_references(remap_references,source);
        if(source.contains("components")&&source["components"].contains("ManagedScript"))
            source["components"]["ManagedScript"]["instanceId"]="script."+source.at("guid").get<std::string>();
        if(roots.contains(old_id)) source["name"]=source.value("name",old_id)+" Copy";scene["entities"].push_back(std::move(source));}
    const auto receipt=Json::parse(world_.replace_scene_document_json(scene.dump(),world_.revision(),"editor.clipboard.paste",false));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.edit-failed"),receipt_detail(receipt,"Paste failed."),{},receipt.value("revisionAfter",world_.revision())};
    if(result.success) {record_edit(EditDomain::scene);refresh();selected_object_ids_.clear();for(const auto& id:roots) selected_object_ids_.push_back(remap.at(id));
        if(!selected_object_ids_.empty()) {selected_object_id_=selected_object_ids_.back();result.entity_id=selected_object_id_;refresh_inspector();}}
    return result;
}

EditorSceneAction EditorModel::rename_selected(const std::string_view display_name) {
    if (selected_object_id_.empty()) return {false,"world.entity-not-found","No entity is selected."};
    const auto receipt = Json::parse(world_.edit_scene_entity_json("rename",selected_object_id_,{},display_name,{},{},false,
        world_.revision(),"editor.outliner",false));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.edit-failed"),
        receipt.value("detail","Entity rename failed."),selected_object_id_,receipt.value("revisionAfter",world_.revision())};
    if (result.success) {record_edit(EditDomain::scene);refresh();}
    return result;
}

EditorSceneAction EditorModel::reparent_entity(const std::string_view entity_id, const std::string_view parent_entity_id) {
    if (entity_id.empty()) return {false,"world.entity-not-found","No entity was provided for reparenting."};
    const auto receipt = Json::parse(world_.edit_scene_entity_json("reparent",entity_id,{},{},parent_entity_id,{},false,
        world_.revision(),"editor.outliner",false));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.edit-failed"),
        receipt.value("detail","Entity reparent failed."),std::string(entity_id),receipt.value("revisionAfter",world_.revision())};
    if (result.success) { record_edit(EditDomain::scene);refresh(); static_cast<void>(select_object(entity_id)); }
    return result;
}

EditorSceneAction EditorModel::delete_selected(const bool recursive) {
    if(selected_object_ids_.empty()) return {false,"world.entity-not-found","No entities are selected."};
    auto scene=Json::parse(world_.canonical_scene_json());std::unordered_set<std::string> removed(selected_object_ids_.begin(),selected_object_ids_.end());
    bool changed=true;while(recursive&&changed) {changed=false;for(const auto& entity:scene.at("entities")) {
        if(entity.contains("parent")&&entity.at("parent").is_string()&&removed.contains(entity.at("parent").get<std::string>())&&
           removed.insert(entity.at("guid").get<std::string>()).second) changed=true;}}
    if(!recursive) for(const auto& entity:scene.at("entities")) if(entity.contains("parent")&&entity.at("parent").is_string()&&
        removed.contains(entity.at("parent").get<std::string>())&&!removed.contains(entity.at("guid").get<std::string>()))
        return {false,"scene.entity-has-children","Delete requires recursive=true while a selected entity owns unselected children."};
    auto& entities=scene["entities"];entities.erase(std::remove_if(entities.begin(),entities.end(),[&](const Json& entity){
        return removed.contains(entity.at("guid").get<std::string>());}),entities.end());
    const auto receipt=Json::parse(world_.replace_scene_document_json(scene.dump(),world_.revision(),"editor.outliner.batch-delete",false));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.edit-failed"),
        receipt_detail(receipt,"Scene delete failed."),selected_object_id_,receipt.value("revisionAfter",world_.revision())};
    if(result.success) {record_edit(EditDomain::scene);selected_object_id_.clear();selected_object_ids_.clear();refresh();}
    return result;
}

EditorSceneAction EditorModel::add_component(const std::string_view component) {
    if (selected_object_id_.empty()) return {false,"world.entity-not-found","No entity is selected."};
    const auto receipt=Json::parse(world_.edit_scene_entity_json("add-component",selected_object_id_,{},{},{},component,false,
        world_.revision(),"editor.inspector",false));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.edit-failed"),
        receipt_detail(receipt,"Component add failed."),selected_object_id_,receipt.value("revisionAfter",world_.revision())};
    if(result.success) {record_edit(EditDomain::scene);refresh();}
    return result;
}

EditorSceneAction EditorModel::remove_component(const std::string_view component) {
    if (selected_object_id_.empty()) return {false,"world.entity-not-found","No entity is selected."};
    const auto receipt=Json::parse(world_.edit_scene_entity_json("remove-component",selected_object_id_,{},{},{},component,false,
        world_.revision(),"editor.inspector",false));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.edit-failed"),
        receipt_detail(receipt,"Component removal failed."),selected_object_id_,receipt.value("revisionAfter",world_.revision())};
    if(result.success) {record_edit(EditDomain::scene);refresh();}
    return result;
}

EditorSceneAction EditorModel::new_scene(const std::string_view display_name) {
    if(scene_dirty())return {false,"scene.unsaved-changes","Save or discard current scene changes before creating a new scene."};
    auto name=std::string(display_name);if(name.empty())name="Untitled Scene";
    const auto stamp=static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream identity;identity<<"scene.new."<<std::hex<<stamp;const auto scene_id=identity.str();
    SceneDocument document{.scene_guid=scene_id,.name=name,.source_uri="unsaved://"+scene_id,
        .entities={
            SceneEntityDocument{.guid=scene_id+".root",.name=name+" Root"},
            SceneEntityDocument{.guid=scene_id+".camera",.name="Main Camera",.parent_guid=scene_id+".root",
                .transform=SceneTransform{{0.0,3.0,8.0}},.camera=SceneCamera{{0.0,1.0,0.0},45.0,0.1,1000.0,true}},
            SceneEntityDocument{.guid=scene_id+".sun",.name="Sun",.parent_guid=scene_id+".root",
                .directional_light=SceneDirectionalLight{}}
        }};
    const auto loaded=world_.load_scene(document);
    if(!loaded.success)return {false,"scene.new-invalid","The new scene template failed validation."};
    edit_undo_timeline_.clear();edit_redo_timeline_.clear();selected_object_id_.clear();selected_object_ids_.clear();
    refresh();saved_scene_json_="{}";
    return {true,"ok","New scene created. Use Save Scene As to choose its project source.",{},world_.revision()};
}

EditorSceneAction EditorModel::save_scene() {
    const auto receipt = Json::parse(world_.save_scene_to_source_json());
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.save-failed"),
        receipt.value("detail","Scene save failed."),{},receipt.value("revision",world_.revision())};
    if (result.success) saved_scene_json_ = world_.canonical_scene_json();
    return result;
}

EditorSceneAction EditorModel::save_scene_as(const std::string_view source_path, const bool overwrite) {
    const auto receipt=Json::parse(world_.save_scene_as_source_json(source_path,overwrite));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.save-failed"),
        receipt_detail(receipt,"Scene Save As failed."),{},receipt.value("revision",world_.revision())};
    if (result.success) { refresh(); saved_scene_json_=world_.canonical_scene_json(); }
    return result;
}

EditorSceneAction EditorModel::open_scene(const std::string_view source_path) {
    if (scene_dirty()) return {false,"scene.unsaved-changes","Save or undo current scene changes before opening another scene."};
    const auto receipt=Json::parse(world_.open_scene_from_source_json(source_path));
    EditorSceneAction result{receipt.value("success",false),receipt.value("code","scene.open-failed"),
        receipt_detail(receipt,"Scene open failed."),{},receipt.value("revision",world_.revision())};
    if (result.success) { edit_undo_timeline_.clear();edit_redo_timeline_.clear();selected_object_id_.clear();selected_object_ids_.clear();refresh();saved_scene_json_=world_.canonical_scene_json(); }
    return result;
}

std::string EditorModel::scene_recovery_candidates_json(const std::string_view project_root) const {
    Json result{{"schemaVersion","noemancer.scene-recovery-candidates/0.1"},{"success",true},{"code","ok"},
        {"items",Json::array()},{"count",0U},{"invalidCount",0U},{"scannedEntries",0U},
        {"truncated",false},{"limit",128U},{"scanLimit",4096U}};
    const auto scenes=project_member(std::filesystem::path(project_root),"scenes");
    if(!scenes){result["success"]=false;result["code"]="scene.recovery-project-invalid";return result.dump();}
    std::error_code error;if(!std::filesystem::is_directory(*scenes,error))return result.dump();
    struct Candidate final {std::int64_t modified{};Json value;};std::vector<Candidate> candidates;
    constexpr std::string_view suffix=".noemancer-recovery";constexpr std::size_t scan_limit=4096U;
    std::size_t invalid{};std::size_t scanned{};bool scan_truncated{};const auto canonical_root=scenes->parent_path();
    for(std::filesystem::recursive_directory_iterator iterator(*scenes,std::filesystem::directory_options::skip_permission_denied,error),end;
        !error&&iterator!=end;iterator.increment(error)) {
        if(scanned>=scan_limit){scan_truncated=true;break;}++scanned;
        const auto regular=iterator->is_regular_file(error);if(error){error.clear();++invalid;continue;}if(!regular)continue;
        const auto filename=iterator->path().filename().string();
        if(!std::string_view(filename).ends_with(suffix))continue;
        const auto canonical_recovery=std::filesystem::weakly_canonical(iterator->path(),error);if(error){error.clear();++invalid;continue;}
        const auto bounded_relative=canonical_recovery.lexically_relative(canonical_root);
        if(bounded_relative.empty()||bounded_relative.begin()->string()==".."){++invalid;continue;}
        const auto recovery_contents=read_file(canonical_recovery);const auto target_text=canonical_recovery.string().substr(0,canonical_recovery.string().size()-suffix.size());
        const auto target=std::filesystem::path(target_text);const auto parsed=SceneDocumentCodec::parse_json(recovery_contents,target.generic_string());
        if(!parsed){++invalid;continue;}const auto current_contents=read_file(target);if(!current_contents.empty()&&current_contents==recovery_contents)continue;
        const auto relative=canonical_recovery.lexically_relative(canonical_root);
        const auto target_relative=target.lexically_relative(canonical_root);
        const auto write_time=iterator->last_write_time(error);const auto modified=error?0:write_time.time_since_epoch().count();error.clear();
        const auto bytes=iterator->file_size(error);error.clear();
        candidates.push_back({modified,{{"id",relative.generic_string()},{"recoveryPath",relative.generic_string()},
            {"targetPath",target_relative.generic_string()},{"sceneGuid",parsed.document->scene_guid},{"name",parsed.document->name},
            {"bytes",bytes},{"modifiedTicks",modified},{"targetExists",std::filesystem::is_regular_file(target,error)}}});error.clear();
    }
    if(error){scan_truncated=true;error.clear();}
    std::ranges::sort(candidates,[](const auto& left,const auto& right){
        return left.modified!=right.modified?left.modified>right.modified:left.value.at("id")<right.value.at("id");});
    constexpr std::size_t limit=128U;result["count"]=candidates.size();result["invalidCount"]=invalid;
    result["scannedEntries"]=scanned;result["truncated"]=scan_truncated||candidates.size()>limit;
    for(std::size_t index=0;index<std::min(limit,candidates.size());++index)
        result["items"].push_back(std::move(candidates[index].value));
    return result.dump();
}

EditorSceneAction EditorModel::recover_scene(const std::string_view project_root,const std::string_view recovery_relative_path) {
    if(scene_dirty())return {false,"scene.unsaved-changes","Save or discard current scene changes before recovering another scene."};
    constexpr std::string_view suffix=".noemancer-recovery";const auto relative=std::filesystem::path(recovery_relative_path);
    if(!std::string_view(relative.generic_string()).ends_with(suffix))
        return {false,"scene.recovery-path-invalid","Recovery selection is not a recovery sidecar."};
    const auto recovery=project_member(std::filesystem::path(project_root),relative);
    if(!recovery||!std::filesystem::is_regular_file(*recovery))
        return {false,"scene.recovery-path-invalid","Recovery selection is outside the active project or unavailable."};
    const auto target=std::filesystem::path(recovery->string().substr(0,recovery->string().size()-suffix.size()));
    const auto parsed=SceneDocumentCodec::parse_json(read_file(*recovery),target.generic_string());
    if(!parsed)return {false,"scene.recovery-invalid","Recovery sidecar is not a valid scene document."};
    auto recovered=*parsed.document;recovered.source_uri=target.generic_string();const auto loaded=world_.load_scene(recovered);
    if(!loaded.success)return {false,"scene.recovery-load-failed","Recovery scene could not be instantiated."};
    const auto target_parse=SceneDocumentCodec::parse_json(read_file(target),target.generic_string());
    saved_scene_json_=target_parse?SceneDocumentCodec::write_canonical_json(*target_parse.document):std::string{};
    edit_undo_timeline_.clear();edit_redo_timeline_.clear();selected_object_id_.clear();selected_object_ids_.clear();refresh();
    return {true,"ok","Recovery candidate loaded as unsaved changes. Save Scene to replace the current source.",{},world_.revision()};
}

void EditorModel::reset_for_loaded_project() {
    if(!active_asset_job_id_.empty())static_cast<void>(asset_jobs_.cancel(active_asset_job_id_));
    active_asset_job_id_.clear();active_asset_job_reconciled_=false;
    edit_undo_timeline_.clear();edit_redo_timeline_.clear();scene_clipboard_json_.clear();
    selected_object_id_.clear();selected_object_ids_.clear();selected_asset_id_.clear();
    refresh();saved_scene_json_=world_.canonical_scene_json();
}

bool EditorModel::can_undo() const noexcept { return !edit_undo_timeline_.empty()||asset_registry_.can_undo_text_source()||world_.can_undo(); }
bool EditorModel::can_redo() const noexcept { return !edit_redo_timeline_.empty()||asset_registry_.can_redo_text_source()||world_.can_redo(); }
bool EditorModel::can_save_scene() const {
    return !observed_scene_source_.empty() && observed_scene_source_.find("://") == std::string::npos;
}
bool EditorModel::scene_dirty() const { return world_.canonical_scene_json() != saved_scene_json_; }
const std::string& EditorModel::scene_source() const noexcept { return observed_scene_source_; }
std::uint64_t EditorModel::world_revision() const noexcept { return world_.revision(); }

void EditorModel::set_focused_panel(const std::string_view panel_id) noexcept {
    if(std::ranges::find(panels_,panel_id,&EditorPanel::id)!=panels_.end())focused_panel_id_=panel_id;
}

const std::string& EditorModel::focused_panel() const noexcept { return focused_panel_id_; }

std::string EditorModel::focused_observation_json() const {
    if(focused_panel_id_=="editor.panel.animation-graph")return selected_animation_graph_authoring_json();
    if (selected_object_id_.empty()) {
        return Json{{"schemaVersion","noemancer.editor-focus/0.1"},{"valid",false},
            {"code","editor.no-selection"},{"revision",world_.revision()},{"entity",nullptr}}.dump();
    }
    ObservationQuery query{
        .entity_ids = selected_object_ids_,
        .fields = {"identity", "hierarchy", "source", "transform", "velocity", "render"},
        .depth = 1,
        .byte_budget = std::max<std::size_t>(4096,selected_object_ids_.size()*2048)
    };
    return world_.observe_json(query);
}

std::string EditorModel::semantic_snapshot_json() const {
    std::ostringstream output;
    output << "{\"schemaVersion\":\"0.1\",\"revision\":" << world_.revision()
           << ",\"focusedPanel\":" << Json(focused_panel_id_).dump() << ",\"selection\":{\"objectId\":\""
           << escape_json(selected_object_id_) << "\",\"objectIds\":" << Json(selected_object_ids_).dump()
           << ",\"count\":" << selected_object_ids_.size() << "},\"clipboard\":{\"canPaste\":" << (can_paste()?"true":"false") << "},\"panels\":[";
    for (std::size_t index = 0; index < panels_.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& panel = panels_.at(index);
        output << "{\"id\":\"" << escape_json(panel.id)
               << "\",\"title\":\"" << escape_json(panel.title)
               << "\",\"region\":\"" << escape_json(panel.region) << "\"}";
    }
    output << "],\"visibleObjects\":[";
    for (std::size_t index = 0; index < objects_.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& object = objects_.at(index);
        output << "{\"id\":\"" << escape_json(object.id)
               << "\",\"name\":\"" << escape_json(object.name)
               << "\",\"kind\":\"" << escape_json(object.kind)
               << "\",\"parentId\":";
        if (object.parent_id.empty()) output << "null";
        else output << "\"" << escape_json(object.parent_id) << "\"";
        output << ",\"source\":{\"uri\":\"" << escape_json(object.source.uri)
               << "\",\"pointer\":\"" << escape_json(object.source.json_pointer) << "\"}}";
    }
    output << "],\"selectedAssetId\":" << Json(selected_asset_id_).dump()
            << ",\"assetJob\":" << active_asset_job_json()
            << ",\"assetRepair\":" << selected_asset_repair_json()
            << ",\"tilemapAuthoring\":" << selected_tilemap_authoring_json()
            << ",\"animationGraphAuthoring\":" << selected_animation_graph_authoring_json()
           << ",\"inspector\":" << world_.inspector_document_json(selected_object_id_) << '}';
    return output.str();
}

} // namespace noemancer
