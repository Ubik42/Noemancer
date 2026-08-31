#include "editor/physics_constraint_panel.hpp"

#include "engine/physics_constraints.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <string>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::ordered_json;

constexpr float kPi = 3.14159265358979323846F;
constexpr std::size_t kMaximumManagerIdBytes = 96U;

Json finite_number(const float value) {
    return std::isfinite(value) ? Json(value) : Json(nullptr);
}

Json vec3_json(const PhysicsConstraintVec3& value) {
    return Json::array({finite_number(value.x), finite_number(value.y), finite_number(value.z)});
}

Json frame_json(const PhysicsConstraintFrame& frame) {
    return Json{{"anchorA", vec3_json(frame.anchor_a)},
                {"anchorB", vec3_json(frame.anchor_b)},
                {"primaryAxisA", vec3_json(frame.primary_axis_a)},
                {"secondaryAxisA", vec3_json(frame.secondary_axis_a)},
                {"primaryAxisB", vec3_json(frame.primary_axis_b)},
                {"secondaryAxisB", vec3_json(frame.secondary_axis_b)}};
}

Json spec_json(const PhysicsConstraintSpec& spec) {
    return Json{{"id", spec.id},
                {"type", physics_constraint_type_name(spec.type)},
                {"bodyA", spec.body_a},
                {"bodyB", spec.body_b},
                {"frame", frame_json(spec.frame)},
                {"lowerLimit", finite_number(spec.lower_limit)},
                {"upperLimit", finite_number(spec.upper_limit)},
                {"restLength", finite_number(spec.rest_length)},
                {"springFrequencyHz", finite_number(spec.spring_frequency_hz)},
                {"springDampingRatio", finite_number(spec.spring_damping_ratio)},
                {"enabled", spec.enabled}};
}

std::string type_label(const PhysicsConstraintType type) {
    switch (type) {
    case PhysicsConstraintType::fixed:
        return "固定";
    case PhysicsConstraintType::distance:
        return "距离";
    case PhysicsConstraintType::hinge:
        return "铰链";
    case PhysicsConstraintType::slider:
        return "滑轨";
    case PhysicsConstraintType::spring:
        return "弹簧";
    }
    return "未知";
}

std::string entity_label(const PhysicsConstraintPanelSnapshot& snapshot,
                         const std::string_view entity_id) {
    for (const auto& choice : snapshot.rigid_bodies) {
        if (choice.entity_id == entity_id) {
            return choice.display_name.empty() ? choice.entity_id : choice.display_name;
        }
    }
    return {};
}

std::string spec_signature(const PhysicsConstraintSpec& spec) {
    return spec_json(spec).dump();
}

std::string option_signature(const PhysicsConstraintEntityOption& option) {
    return option.entity_id + "\n" + option.display_name;
}

std::uint64_t stable_hash(const std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : value) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

std::string hex_hash(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

void add_diagnostic(PhysicsConstraintPanelValidation& validation,
                    std::string code, std::string path, std::string message) {
    // A malformed external snapshot must never make the semantic projection
    // unbounded.  The first entries are ordered and therefore repeatable.
    if (validation.diagnostics.size() >= 128U) return;
    validation.diagnostics.push_back(
        {std::move(code), std::move(path), std::move(message)});
}

PhysicsConstraintSpec make_default_spec(const PhysicsConstraintType type,
                                        std::string id,
                                        const std::vector<PhysicsConstraintEntityOption>& choices) {
    PhysicsConstraintSpec result;
    result.id = std::move(id);
    result.type = type;
    if (!choices.empty()) result.body_a = choices[0].entity_id;
    if (choices.size() > 1U) result.body_b = choices[1].entity_id;
    result.frame.primary_axis_a = {0.0F, 1.0F, 0.0F};
    result.frame.secondary_axis_a = {1.0F, 0.0F, 0.0F};
    result.frame.primary_axis_b = {0.0F, 1.0F, 0.0F};
    result.frame.secondary_axis_b = {1.0F, 0.0F, 0.0F};
    result.rest_length = 1.0F;
    result.spring_damping_ratio = 1.0F;

    switch (type) {
    case PhysicsConstraintType::fixed:
        result.lower_limit = 0.0F;
        result.upper_limit = 0.0F;
        result.spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::distance:
        result.lower_limit = 0.0F;
        result.upper_limit = 1.0F;
        result.spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::hinge:
        result.lower_limit = -kPi;
        result.upper_limit = kPi;
        result.spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::slider:
        result.lower_limit = -1.0F;
        result.upper_limit = 1.0F;
        result.spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::spring:
        result.lower_limit = 0.0F;
        result.upper_limit = 0.0F;
        result.rest_length = 1.0F;
        result.spring_frequency_hz = 4.0F;
        result.spring_damping_ratio = 0.5F;
        break;
    }
    return result;
}

Json field_json(const std::string_view id, const std::string_view role,
                const std::string_view label, const std::string_view binding,
                Json value, const std::string_view intent) {
    return Json{{"nodeId", std::string(physics_constraint_panel_node_id) + "." + std::string(id)},
                {"role", role},
                {"label", label},
                {"binding", binding},
                {"value", std::move(value)},
                {"intent", intent}};
}

Json optional_spec_json(const std::optional<PhysicsConstraintSpec>& spec) {
    return spec ? spec_json(*spec) : Json(nullptr);
}

} // namespace

std::string_view physics_constraint_panel_request_kind_name(
    const PhysicsConstraintPanelRequestKind kind) noexcept {
    switch (kind) {
    case PhysicsConstraintPanelRequestKind::upsert:
        return "upsert";
    case PhysicsConstraintPanelRequestKind::remove:
        return "remove";
    }
    return "unknown";
}

PhysicsConstraintPanel::PhysicsConstraintPanel(PhysicsConstraintPanelSnapshot snapshot)
    : snapshot_(std::move(snapshot)) {
    normalize_snapshot();
    rebuild_projection();
}

const PhysicsConstraintPanelSnapshot& PhysicsConstraintPanel::snapshot() const noexcept {
    return snapshot_;
}

const std::optional<PhysicsConstraintSpec>& PhysicsConstraintPanel::draft() const noexcept {
    return draft_;
}

const PhysicsConstraintPanelValidation& PhysicsConstraintPanel::validation() const noexcept {
    return validation_;
}

std::string_view PhysicsConstraintPanel::selected_constraint_id() const noexcept {
    return selected_constraint_id_;
}

std::string_view PhysicsConstraintPanel::last_error() const noexcept {
    return last_error_;
}

PhysicsConstraintPanelState PhysicsConstraintPanel::state() const {
    return {.snapshot = snapshot_,
            .selected_constraint_id = selected_constraint_id_,
            .draft = draft_,
            .validation = validation_,
            .can_upsert = draft_.has_value() && validation_.valid &&
                (is_existing_draft() || snapshot_.constraints.size() < physics_constraint_panel_max_constraints),
            .can_remove = !selected_constraint_id_.empty() && has_constraint(selected_constraint_id_),
            .has_pending_request = pending_request_.has_value(),
            .pending_request = pending_request_,
            .last_error = last_error_};
}

void PhysicsConstraintPanel::set_snapshot(PhysicsConstraintPanelSnapshot snapshot) {
    const auto previous_selection = selected_constraint_id_;
    const auto incoming_revision = snapshot.world_revision == 0U ? 1U : snapshot.world_revision;
    const auto incoming_manager = snapshot.manager_id.empty() ? std::string("world.physics") :
        snapshot.manager_id.substr(0U, kMaximumManagerIdBytes);
    if (pending_request_ &&
        (pending_request_->base_revision != incoming_revision ||
         pending_request_->manager_id != incoming_manager)) {
        pending_request_.reset();
        set_error("场景版本或物理管理器已经变化，待提交的物理约束请求已撤销，请重新检查关系台内容。");
    }

    snapshot_ = std::move(snapshot);
    normalize_snapshot();
    selected_constraint_id_ = previous_selection;
    if (!selected_constraint_id_.empty()) {
        const auto found = std::ranges::find(snapshot_.constraints, selected_constraint_id_,
                                             &PhysicsConstraintSpec::id);
        if (found != snapshot_.constraints.end()) {
            draft_ = *found;
        } else {
            selected_constraint_id_.clear();
            draft_.reset();
            if (last_error_.empty()) {
                set_error("原先选中的约束已经不在当前场景中，关系台已清空选择。");
            }
        }
    } else {
        draft_.reset();
    }
    rebuild_projection();
}

bool PhysicsConstraintPanel::select_constraint(const std::string_view constraint_id) {
    if (constraint_id.empty()) {
        clear_selection();
        return true;
    }
    const auto found = std::ranges::find(snapshot_.constraints, constraint_id,
                                         &PhysicsConstraintSpec::id);
    if (found == snapshot_.constraints.end()) {
        set_error("找不到对应的物理约束，当前场景关系图可能已经更新。");
        return false;
    }
    selected_constraint_id_ = found->id;
    draft_ = *found;
    last_error_.clear();
    rebuild_projection();
    return true;
}

void PhysicsConstraintPanel::clear_selection() {
    selected_constraint_id_.clear();
    draft_.reset();
    last_error_.clear();
    rebuild_projection();
}

bool PhysicsConstraintPanel::set_draft(std::optional<PhysicsConstraintSpec> draft) {
    if (draft) {
        selected_constraint_id_ = draft->id;
    } else {
        selected_constraint_id_.clear();
    }
    draft_ = std::move(draft);
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::create_draft(const PhysicsConstraintType type,
                                          std::string constraint_id) {
    if (constraint_id.empty()) {
        constraint_id = "constraint." + std::string(physics_constraint_type_name(type)) + ".new";
        const auto base_id = constraint_id;
        std::size_t ordinal = 2U;
        while (has_constraint(constraint_id)) {
            constraint_id = base_id + "." + std::to_string(ordinal++);
        }
    }
    draft_ = make_default_spec(type, std::move(constraint_id), snapshot_.rigid_bodies);
    selected_constraint_id_ = draft_->id;
    last_error_.clear();
    rebuild_projection();
    return true;
}

void PhysicsConstraintPanel::clear_draft() {
    clear_selection();
}

bool PhysicsConstraintPanel::ensure_draft() {
    if (draft_) return true;
    set_error("请先在场景关系图中选择约束，或创建一个新的约束草稿。");
    return false;
}

bool PhysicsConstraintPanel::set_constraint_id(std::string constraint_id) {
    if (!ensure_draft()) return false;
    draft_->id = std::move(constraint_id);
    selected_constraint_id_ = draft_->id;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_constraint_type(const PhysicsConstraintType type) {
    if (!ensure_draft()) return false;
    draft_->type = type;
    // Switching type should leave a useful, valid draft rather than retain
    // incompatible limits from the previous constraint kind.
    switch (type) {
    case PhysicsConstraintType::fixed:
        draft_->lower_limit = 0.0F;
        draft_->upper_limit = 0.0F;
        draft_->spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::distance:
        draft_->lower_limit = 0.0F;
        draft_->upper_limit = 1.0F;
        draft_->spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::hinge:
        draft_->lower_limit = -kPi;
        draft_->upper_limit = kPi;
        draft_->spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::slider:
        draft_->lower_limit = -1.0F;
        draft_->upper_limit = 1.0F;
        draft_->spring_frequency_hz = 0.0F;
        break;
    case PhysicsConstraintType::spring:
        draft_->lower_limit = 0.0F;
        draft_->upper_limit = 0.0F;
        draft_->rest_length = std::max(draft_->rest_length, 1.0F);
        draft_->spring_frequency_hz = 4.0F;
        draft_->spring_damping_ratio = 0.5F;
        break;
    }
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_body_a(std::string entity_id) {
    if (!ensure_draft()) return false;
    draft_->body_a = std::move(entity_id);
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_body_b(std::string entity_id) {
    if (!ensure_draft()) return false;
    draft_->body_b = std::move(entity_id);
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_enabled(const bool enabled) {
    if (!ensure_draft()) return false;
    draft_->enabled = enabled;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_anchor_a(const PhysicsConstraintVec3 value) {
    if (!ensure_draft()) return false;
    draft_->frame.anchor_a = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_anchor_b(const PhysicsConstraintVec3 value) {
    if (!ensure_draft()) return false;
    draft_->frame.anchor_b = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_primary_axis_a(const PhysicsConstraintVec3 value) {
    if (!ensure_draft()) return false;
    draft_->frame.primary_axis_a = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_secondary_axis_a(const PhysicsConstraintVec3 value) {
    if (!ensure_draft()) return false;
    draft_->frame.secondary_axis_a = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_primary_axis_b(const PhysicsConstraintVec3 value) {
    if (!ensure_draft()) return false;
    draft_->frame.primary_axis_b = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_secondary_axis_b(const PhysicsConstraintVec3 value) {
    if (!ensure_draft()) return false;
    draft_->frame.secondary_axis_b = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_lower_limit(const float value) {
    if (!ensure_draft()) return false;
    draft_->lower_limit = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_upper_limit(const float value) {
    if (!ensure_draft()) return false;
    draft_->upper_limit = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_rest_length(const float value) {
    if (!ensure_draft()) return false;
    draft_->rest_length = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_spring_frequency_hz(const float value) {
    if (!ensure_draft()) return false;
    draft_->spring_frequency_hz = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::set_spring_damping_ratio(const float value) {
    if (!ensure_draft()) return false;
    draft_->spring_damping_ratio = value;
    last_error_.clear();
    rebuild_projection();
    return true;
}

bool PhysicsConstraintPanel::request_upsert(const bool dry_run) {
    if (!draft_) {
        set_error("没有可提交的物理约束草稿。");
        return false;
    }
    if (!validation_.valid) {
        set_error("物理约束草稿仍有诊断问题，修正后才能提交。");
        return false;
    }
    if (!is_existing_draft() && snapshot_.constraints.size() >= physics_constraint_panel_max_constraints) {
        set_error("当前场景已达到 1024 条物理约束上限。");
        return false;
    }
    return queue_request({.kind = PhysicsConstraintPanelRequestKind::upsert,
                          .request_id = make_request_id(PhysicsConstraintPanelRequestKind::upsert,
                                                        draft_->id, draft_, dry_run),
                          .manager_id = snapshot_.manager_id,
                          .base_revision = snapshot_.world_revision,
                          .dry_run = dry_run,
                          .constraint_id = draft_->id,
                          .constraint = draft_});
}

bool PhysicsConstraintPanel::request_remove(const bool dry_run) {
    if (selected_constraint_id_.empty() || !has_constraint(selected_constraint_id_)) {
        set_error("请先选择一个当前场景中的物理约束才能删除。");
        return false;
    }
    return queue_request({.kind = PhysicsConstraintPanelRequestKind::remove,
                          .request_id = make_request_id(PhysicsConstraintPanelRequestKind::remove,
                                                        selected_constraint_id_, std::nullopt, dry_run),
                          .manager_id = snapshot_.manager_id,
                          .base_revision = snapshot_.world_revision,
                          .dry_run = dry_run,
                          .constraint_id = selected_constraint_id_,
                          .constraint = std::nullopt});
}

std::optional<PhysicsConstraintPanelRequest> PhysicsConstraintPanel::consume_request() {
    auto result = std::move(pending_request_);
    pending_request_.reset();
    return result;
}

std::string PhysicsConstraintPanel::semantic_state_json() const {
    Json root{{"schema", physics_constraint_panel_schema},
              {"nodeId", physics_constraint_panel_node_id},
              {"title", "物理约束关系台"},
              {"description", "在场景关系图上校准刚体之间的固定、距离、铰链、滑轨与弹簧连接。"},
              {"presentation", Json{{"concept", "scene-cartography-instrument-bench"},
                                     {"language", "zh-CN"},
                                     {"surface", "relationship-workbench"},
                                     {"density", "compact"},
                                     {"accent", "survey-orange"}}}};

    root["snapshot"] = Json{{"worldRevision", snapshot_.world_revision},
                             {"manager", snapshot_.manager_id},
                             {"constraintCount", snapshot_.constraints.size()},
                             {"entityChoiceCount", snapshot_.rigid_bodies.size()},
                             {"sourceConstraintCount", source_constraint_count_},
                             {"sourceEntityChoiceCount", source_entity_choice_count_},
                             {"limits", Json{{"maxConstraints", physics_constraint_panel_max_constraints},
                                               {"maxEntityChoices", physics_constraint_panel_max_entity_choices}}},
                             {"truncated", Json{{"constraints", source_constraint_count_ > snapshot_.constraints.size()},
                                                  {"entityChoices", source_entity_choice_count_ > snapshot_.rigid_bodies.size()}}}};

    root["selection"] = Json{{"constraintId", selected_constraint_id_},
                              {"stable", !selected_constraint_id_.empty() && has_constraint(selected_constraint_id_)}};

    auto choices = Json::array();
    for (const auto& choice : snapshot_.rigid_bodies) {
        choices.push_back(Json{{"entityId", choice.entity_id},
                               {"label", choice.display_name.empty() ? choice.entity_id : choice.display_name},
                               {"displayName", choice.display_name}});
    }
    root["entityChoices"] = std::move(choices);

    auto constraints = Json::array();
    for (const auto& constraint : snapshot_.constraints) {
        auto item = spec_json(constraint);
        item["typeLabel"] = type_label(constraint.type);
        item["bodyALabel"] = entity_label(snapshot_, constraint.body_a);
        item["bodyBLabel"] = entity_label(snapshot_, constraint.body_b);
        item["selected"] = selected_constraint_id_ == constraint.id;
        constraints.push_back(std::move(item));
    }
    root["constraints"] = std::move(constraints);
    root["draft"] = optional_spec_json(draft_);

    Json validation{{"valid", validation_.valid}};
    auto diagnostics = Json::array();
    for (const auto& diagnostic : validation_.diagnostics) {
        diagnostics.push_back(Json{{"code", diagnostic.code},
                                   {"path", diagnostic.path},
                                   {"message", diagnostic.message}});
    }
    validation["diagnostics"] = std::move(diagnostics);
    root["validation"] = std::move(validation);

    auto fields = Json::array();
    fields.push_back(field_json("id", "text", "关系 ID", "draft.id",
                                draft_ ? Json(draft_->id) : Json(nullptr), "set-constraint-id"));

    auto type_choices = Json::array();
    for (const auto type : {PhysicsConstraintType::fixed, PhysicsConstraintType::distance,
                            PhysicsConstraintType::hinge, PhysicsConstraintType::slider,
                            PhysicsConstraintType::spring}) {
        type_choices.push_back(Json{{"value", physics_constraint_type_name(type)},
                                    {"label", type_label(type)}});
    }
    auto type_field = field_json("type", "select", "连接类型", "draft.type",
                                draft_ ? Json(physics_constraint_type_name(draft_->type)) : Json(nullptr),
                                "set-constraint-type");
    type_field["choices"] = std::move(type_choices);
    fields.push_back(std::move(type_field));

    auto body_a_field = field_json("body-a", "select", "刚体 A", "draft.bodyA",
                                   draft_ ? Json(draft_->body_a) : Json(nullptr), "set-constraint-body-a");
    auto body_b_field = field_json("body-b", "select", "刚体 B", "draft.bodyB",
                                   draft_ ? Json(draft_->body_b) : Json(nullptr), "set-constraint-body-b");
    auto choice_values = Json::array();
    for (const auto& choice : snapshot_.rigid_bodies) {
        choice_values.push_back(Json{{"value", choice.entity_id},
                                     {"label", choice.display_name.empty() ? choice.entity_id : choice.display_name}});
    }
    body_a_field["choices"] = choice_values;
    body_b_field["choices"] = std::move(choice_values);
    fields.push_back(std::move(body_a_field));
    fields.push_back(std::move(body_b_field));

    auto enabled_field = field_json("enabled", "checkbox", "启用约束", "draft.enabled",
                                    draft_ ? Json(draft_->enabled) : Json(nullptr), "set-constraint-enabled");
    fields.push_back(std::move(enabled_field));

    const auto frame_value = draft_ ? frame_json(draft_->frame) : Json(nullptr);
    auto frame_field = field_json("frame", "relationship-frame", "关系参考框架", "draft.frame",
                                  frame_value, "set-constraint-frame");
    frame_field["parts"] = Json{{"anchorA", "draft.frame.anchorA"},
                                 {"anchorB", "draft.frame.anchorB"},
                                 {"primaryAxisA", "draft.frame.primaryAxisA"},
                                 {"secondaryAxisA", "draft.frame.secondaryAxisA"},
                                 {"primaryAxisB", "draft.frame.primaryAxisB"},
                                 {"secondaryAxisB", "draft.frame.secondaryAxisB"}};
    fields.push_back(std::move(frame_field));

    fields.push_back(field_json("anchor-a", "vector3", "锚点 A", "draft.frame.anchorA",
                                draft_ ? vec3_json(draft_->frame.anchor_a) : Json(nullptr), "set-constraint-anchor-a"));
    fields.push_back(field_json("anchor-b", "vector3", "锚点 B", "draft.frame.anchorB",
                                draft_ ? vec3_json(draft_->frame.anchor_b) : Json(nullptr), "set-constraint-anchor-b"));
    fields.push_back(field_json("primary-axis-a", "vector3", "主轴 A", "draft.frame.primaryAxisA",
                                draft_ ? vec3_json(draft_->frame.primary_axis_a) : Json(nullptr), "set-constraint-primary-axis-a"));
    fields.push_back(field_json("secondary-axis-a", "vector3", "副轴 A", "draft.frame.secondaryAxisA",
                                draft_ ? vec3_json(draft_->frame.secondary_axis_a) : Json(nullptr), "set-constraint-secondary-axis-a"));
    fields.push_back(field_json("primary-axis-b", "vector3", "主轴 B", "draft.frame.primaryAxisB",
                                draft_ ? vec3_json(draft_->frame.primary_axis_b) : Json(nullptr), "set-constraint-primary-axis-b"));
    fields.push_back(field_json("secondary-axis-b", "vector3", "副轴 B", "draft.frame.secondaryAxisB",
                                draft_ ? vec3_json(draft_->frame.secondary_axis_b) : Json(nullptr), "set-constraint-secondary-axis-b"));
    fields.push_back(field_json("lower-limit", "number", "下限", "draft.lowerLimit",
                                draft_ ? finite_number(draft_->lower_limit) : Json(nullptr), "set-constraint-lower-limit"));
    fields.push_back(field_json("upper-limit", "number", "上限", "draft.upperLimit",
                                draft_ ? finite_number(draft_->upper_limit) : Json(nullptr), "set-constraint-upper-limit"));
    fields.push_back(field_json("rest-length", "number", "静止长度", "draft.restLength",
                                draft_ ? finite_number(draft_->rest_length) : Json(nullptr), "set-constraint-rest-length"));
    fields.push_back(field_json("spring-frequency", "number", "弹簧频率（Hz）", "draft.springFrequencyHz",
                                draft_ ? finite_number(draft_->spring_frequency_hz) : Json(nullptr), "set-constraint-spring-frequency"));
    fields.push_back(field_json("spring-damping", "number", "弹簧阻尼比", "draft.springDampingRatio",
                                draft_ ? finite_number(draft_->spring_damping_ratio) : Json(nullptr), "set-constraint-spring-damping"));
    root["fields"] = std::move(fields);

    root["actions"] = Json{{"createDraft", physics_constraint_panel_create_id},
                            {"upsert", physics_constraint_panel_upsert_id},
                            {"upsertDryRun", physics_constraint_panel_upsert_dry_run_id},
                            {"remove", physics_constraint_panel_remove_id}};
    root["capabilities"] = Json{{"selectionByStableId", true},
                                 {"revisionBoundRequests", true},
                                 {"dryRun", true},
                                 {"nativeBackendTypes", false},
                                 {"constraintTypes", 5U}};

    if (pending_request_) {
        root["request"] = Json{{"kind", physics_constraint_panel_request_kind_name(pending_request_->kind)},
                                {"requestId", pending_request_->request_id},
                                {"manager", pending_request_->manager_id},
                                {"baseRevision", pending_request_->base_revision},
                                {"dryRun", pending_request_->dry_run},
                                {"constraintId", pending_request_->constraint_id},
                                {"constraint", optional_spec_json(pending_request_->constraint)}};
    } else {
        root["request"] = nullptr;
    }
    root["lastError"] = last_error_;
    return root.dump(2) + "\n";
}

void PhysicsConstraintPanel::normalize_snapshot() {
    snapshot_.world_revision = snapshot_.world_revision == 0U ? 1U : snapshot_.world_revision;
    if (snapshot_.manager_id.empty()) snapshot_.manager_id = "world.physics";
    if (snapshot_.manager_id.size() > kMaximumManagerIdBytes)
        snapshot_.manager_id.resize(kMaximumManagerIdBytes);

    source_constraint_count_ = snapshot_.constraints.size();
    source_entity_choice_count_ = snapshot_.rigid_bodies.size();

    std::sort(snapshot_.constraints.begin(), snapshot_.constraints.end(),
              [](const auto& left, const auto& right) {
                  if (left.id != right.id) return left.id < right.id;
                  return spec_signature(left) < spec_signature(right);
              });
    std::sort(snapshot_.rigid_bodies.begin(), snapshot_.rigid_bodies.end(),
              [](const auto& left, const auto& right) {
                  if (left.entity_id != right.entity_id) return left.entity_id < right.entity_id;
                  return option_signature(left) < option_signature(right);
              });
    if (snapshot_.constraints.size() > physics_constraint_panel_max_constraints)
        snapshot_.constraints.resize(physics_constraint_panel_max_constraints);
    if (snapshot_.rigid_bodies.size() > physics_constraint_panel_max_entity_choices)
        snapshot_.rigid_bodies.resize(physics_constraint_panel_max_entity_choices);
}

void PhysicsConstraintPanel::rebuild_projection() {
    validation_ = {};
    if (source_constraint_count_ > snapshot_.constraints.size()) {
        add_diagnostic(validation_, "physics-constraint.snapshot.constraints-truncated",
                       "snapshot.constraints", "场景约束数量超过 1024 条，关系台已按稳定 ID 保留前 1024 条。");
    }
    if (source_entity_choice_count_ > snapshot_.rigid_bodies.size()) {
        add_diagnostic(validation_, "physics-constraint.snapshot.entity-choices-truncated",
                       "snapshot.rigidBodies", "刚体选项超过 4096 个，关系台已按稳定 ID 保留前 4096 个。");
    }

    for (std::size_t index = 1U; index < snapshot_.constraints.size(); ++index) {
        if (snapshot_.constraints[index - 1U].id == snapshot_.constraints[index].id) {
            add_diagnostic(validation_, "physics-constraint.snapshot.duplicate-id",
                           "snapshot.constraints[" + std::to_string(index) + "].id",
                           "场景中存在重复的约束 ID；提交前应由场景文档校验器拒绝。 ");
        }
    }

    if (draft_) {
        const auto& value = *draft_;
        if (value.body_a.empty() || !has_entity(value.body_a)) {
            add_diagnostic(validation_, "physics-constraint.body-a.invalid-reference",
                           "draft.bodyA", "刚体 A 没有指向当前场景中可用的刚体实体。");
        }
        if (value.body_b.empty() || !has_entity(value.body_b)) {
            add_diagnostic(validation_, "physics-constraint.body-b.invalid-reference",
                           "draft.bodyB", "刚体 B 没有指向当前场景中可用的刚体实体。");
        }
        if (!value.body_a.empty() && value.body_a == value.body_b) {
            add_diagnostic(validation_, "physics-constraint.body.self-reference",
                           "draft.bodyB", "刚体 A 与刚体 B 不能是同一个实体。");
        }

        const auto result = validate_physics_constraint_spec(value);
        if (!result.success) {
            add_diagnostic(validation_, "physics-constraint.spec.invalid", "draft",
                           "约束参数不满足引擎契约：" + result.detail);
        }
    }
    validation_.valid = validation_.diagnostics.empty();
}

void PhysicsConstraintPanel::set_error(std::string message) {
    last_error_ = std::move(message);
}

bool PhysicsConstraintPanel::queue_request(PhysicsConstraintPanelRequest request) {
    if (pending_request_) {
        set_error("关系台已有一个待处理请求，请先消费它再发出新的请求。");
        return false;
    }
    pending_request_ = std::move(request);
    last_error_.clear();
    return true;
}

std::string PhysicsConstraintPanel::make_request_id(
    const PhysicsConstraintPanelRequestKind kind,
    const std::string& constraint_id,
    const std::optional<PhysicsConstraintSpec>& constraint,
    const bool dry_run) const {
    const auto payload = snapshot_.manager_id + ":" + std::to_string(snapshot_.world_revision) + ":" +
        std::string(physics_constraint_panel_request_kind_name(kind)) + ":" + constraint_id + ":" +
        (dry_run ? "dry-run:" : "commit:") + (constraint ? spec_signature(*constraint) : "null");
    return std::string(physics_constraint_panel_node_id) + "." +
        std::string(physics_constraint_panel_request_kind_name(kind)) + "." +
        std::to_string(snapshot_.world_revision) + "." + hex_hash(stable_hash(payload));
}

bool PhysicsConstraintPanel::has_entity(const std::string_view entity_id) const noexcept {
    return std::ranges::any_of(snapshot_.rigid_bodies,
                               [entity_id](const auto& choice) { return choice.entity_id == entity_id; });
}

bool PhysicsConstraintPanel::has_constraint(const std::string_view constraint_id) const noexcept {
    return std::ranges::any_of(snapshot_.constraints,
                               [constraint_id](const auto& constraint) { return constraint.id == constraint_id; });
}

bool PhysicsConstraintPanel::is_existing_draft() const noexcept {
    return draft_.has_value() && has_constraint(draft_->id);
}

} // namespace noemancer
