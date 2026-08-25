#include "engine/command_registry.hpp"

#include "engine/asset_registry.hpp"
#include "engine/animation_graph.hpp"
#include "engine/animation_graph_patch.hpp"
#include "engine/engine_host.hpp"
#include "engine/network_replication.hpp"
#include "engine/network_transport.hpp"
#include "engine/project_ui_authoring_commands.hpp"
#include "engine/render_graph.hpp"
#include "engine/render_world.hpp"
#include "engine/retained_ui_runtime.hpp"
#include "engine/semantic_state.hpp"
#include "engine/sprite_asset.hpp"
#include "engine/tilemap_asset.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::atomic<std::uint64_t> operation_counter{0};

std::string next_operation_id() {
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream output;
    output << "op_" << std::hex << timestamp << '-' << std::dec
           << std::setfill('0') << std::setw(4) << ++operation_counter;
    return output.str();
}

Json parse_object(const std::string_view value) {
    const auto parsed = Json::parse(value.empty() ? "{}" : value);
    if (!parsed.is_object()) {
        throw std::invalid_argument("Tool arguments must be a JSON object");
    }
    return parsed;
}

Json asset_source_receipt_json(const AssetSourceEditReceipt& receipt) {
    return {{"success",receipt.success},{"code",receipt.code},{"detail",receipt.detail},{"assetId",receipt.asset_id},
        {"source",receipt.source},{"manager",receipt.manager},{"registryRevision",receipt.registry_revision},
        {"transactionId",receipt.transaction_id}};
}

bool reload_authorable_asset_runtime(World& world,AssetRegistry& assets,const std::string_view asset_id) {
    const auto* asset=assets.find(asset_id);if(asset==nullptr||!asset->available)return false;
    if(asset->kind=="AnimationGraph"||asset->relative_path.ends_with(".animation-graph.json")) {
        std::ifstream stream(assets.source_path(*asset),std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
        const auto graph=AnimationGraphCodec::parse_json(source);return graph&&world.register_animation_graph(*graph.document);
    }
    if(!asset->relative_path.ends_with(".tilemap.json"))return false;
    std::ifstream map_stream(assets.source_path(*asset),std::ios::binary);
    const std::string map_source{std::istreambuf_iterator<char>(map_stream),std::istreambuf_iterator<char>()};map_stream.close();
    const auto map=TilemapAssetCodec::parse_tilemap_json(map_source);if(!map)return false;
    const auto* palette_asset=assets.find(map.document->palette_asset);if(palette_asset==nullptr||!palette_asset->available)return false;
    std::ifstream palette_stream(assets.source_path(*palette_asset),std::ios::binary);
    const std::string palette_source{std::istreambuf_iterator<char>(palette_stream),std::istreambuf_iterator<char>()};palette_stream.close();
    const auto palette=TilemapAssetCodec::parse_palette_json(palette_source);if(!palette)return false;
    return world.register_tile_palette(*palette.document)&&world.register_tilemap_asset(*map.document);
}

Transform parse_vector3(const Json& value) {
    if(!value.is_object()) throw std::invalid_argument("Vector3 argument must be an object");
    return {value.at("x").get<float>(),value.at("y").get<float>(),value.at("z").get<float>()};
}

Json parse_schema(const std::string& value) {
    return Json::parse(value);
}

Json validate_object(
    const std::string_view document,
    const std::string& schema_json) {
    auto input = parse_object(document);
    const auto schema = parse_schema(schema_json);
    const auto properties = schema.value("properties", Json::object());

    if (!properties.is_object()) {
        throw std::logic_error("Command inputSchema properties must be an object");
    }
    if (schema.value("additionalProperties", true) == false) {
        for (const auto& [key, unused] : input.items()) {
            static_cast<void>(unused);
            if (!properties.contains(key)) {
                throw std::invalid_argument("Unknown argument: " + key);
            }
        }
    }

    for (const auto& required : schema.value("required", Json::array())) {
        const auto property = required.get<std::string>();
        if (!input.contains(property)) {
            throw std::invalid_argument("Missing required argument: " + property);
        }
    }

    for (const auto& [name, property_schema] : properties.items()) {
        if (!input.contains(name)) {
            if (property_schema.contains("default")) {
                input[name] = property_schema.at("default");
            }
            continue;
        }

        const auto type = property_schema.value("type", std::string{});
        const auto& value = input.at(name);
        if (type == "integer") {
            if (!value.is_number_integer()) {
                throw std::invalid_argument(name + " must be an integer");
            }
            const auto number = value.get<std::int64_t>();
            if (property_schema.contains("minimum") &&
                number < property_schema.at("minimum").get<std::int64_t>()) {
                throw std::invalid_argument(name + " is below its minimum");
            }
            if (property_schema.contains("maximum") &&
                number > property_schema.at("maximum").get<std::int64_t>()) {
                throw std::invalid_argument(name + " is above its maximum");
            }
        } else if (type == "string" && !value.is_string()) {
            throw std::invalid_argument(name + " must be a string");
        } else if (type == "boolean" && !value.is_boolean()) {
            throw std::invalid_argument(name + " must be a boolean");
        } else if (type == "object" && !value.is_object()) {
            throw std::invalid_argument(name + " must be an object");
        } else if (type == "array" && !value.is_array()) {
            throw std::invalid_argument(name + " must be an array");
        } else if (type == "number" && !value.is_number()) {
            throw std::invalid_argument(name + " must be a number");
        }
    }
    return input;
}

SemanticVector3 parse_vector(const Json& value, const std::string_view field_name) {
    if (!value.is_object() || !value.contains("x") || !value.contains("y") || !value.contains("z") ||
        !value.at("x").is_number() || !value.at("y").is_number() || !value.at("z").is_number()) {
        throw std::invalid_argument(std::string(field_name) + " must contain numeric x, y and z fields");
    }
    const SemanticVector3 result{
        value.at("x").get<double>(),
        value.at("y").get<double>(),
        value.at("z").get<double>()
    };
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) {
        throw std::invalid_argument(std::string(field_name) + " values must be finite");
    }
    return result;
}

TransformChangePlan parse_transform_plan(const Json& value) {
    if (!value.is_object()) throw std::invalid_argument("plan must be an object");
    constexpr std::array required{
        "valid", "code", "detail", "planId", "contentHash", "manager",
        "baseRevision", "entityId", "before", "after"
    };
    for (const auto* field : required) {
        if (!value.contains(field)) throw std::invalid_argument(std::string("plan is missing ") + field);
    }
    return {
        .valid = value.at("valid").get<bool>(),
        .code = value.at("code").get<std::string>(),
        .detail = value.at("detail").get<std::string>(),
        .plan_id = value.at("planId").get<std::string>(),
        .content_hash = value.at("contentHash").get<std::string>(),
        .manager = value.at("manager").get<std::string>(),
        .entity_id = value.at("entityId").get<std::string>(),
        .base_revision = value.at("baseRevision").get<std::uint64_t>(),
        .before = parse_vector(value.at("before"), "plan.before"),
        .after = parse_vector(value.at("after"), "plan.after")
    };
}

PropertyChangePlan parse_property_plan(const Json& value) {
    if (!value.is_object()) throw std::invalid_argument("plan must be an object");
    constexpr std::array required{"valid","code","detail","planId","contentHash","manager","baseRevision",
        "entityId","property","before","after"};
    for (const auto* field:required) if(!value.contains(field)) throw std::invalid_argument(std::string("plan is missing ")+field);
    return {.valid=value.at("valid").get<bool>(),.code=value.at("code").get<std::string>(),
        .detail=value.at("detail").get<std::string>(),.plan_id=value.at("planId").get<std::string>(),
        .content_hash=value.at("contentHash").get<std::string>(),.manager=value.at("manager").get<std::string>(),
        .entity_id=value.at("entityId").get<std::string>(),.property=value.at("property").get<std::string>(),
        .base_revision=value.at("baseRevision").get<std::uint64_t>(),.before_value_json=value.at("before").dump(),
        .after_value_json=value.at("after").dump()};
}

std::string make_error_envelope(
    const std::string_view tool,
    const std::string_view code,
    const std::string_view message) {
    const Json envelope = {
        {"protocolVersion", "0.2"},
        {"ok", false},
        {"tool", tool},
        {"error", {
            {"code", code},
            {"message", message}
        }}
    };
    return envelope.dump();
}

std::string make_success_envelope(
    const std::string_view tool,
    const std::string_view access,
    const std::string_view result_json) {
    const auto result = Json::parse(result_json);
    const auto operation_id = result.value("operationId", next_operation_id());
    Json changed_objects = Json::array();
    if (!result.value("dryRun", false) &&
        result.contains("delta") && result.at("delta").is_object()) {
        const auto& delta = result.at("delta");
        changed_objects.push_back({
            {"id", delta.value("entityId", std::string{})},
            {"field", delta.value("field", std::string{})},
            {"revision", delta.value("revisionAfter", 0ULL)}
        });
    }
    const Json envelope = {
        {"protocolVersion", "0.2"},
        {"ok", true},
        {"tool", tool},
        {"result", result},
        {"receipt", {
            {"operationId", operation_id},
            {"access", access},
            {"status", result.value("dryRun", false) ? "validated" : "completed"},
            {"changedObjects", std::move(changed_objects)},
            {"artifacts", result.value("artifacts", Json::array())},
            {"evidence", result.value("evidence", Json::array())},
            {"warnings", Json::array()}
        }}
    };
    return envelope.dump();
}

std::string make_action_failure_envelope(
    const std::string_view tool,
    const std::string_view access,
    const std::string_view result_json) {
    auto envelope = Json::parse(make_success_envelope(tool, access, result_json));
    const auto& result = envelope.at("result");
    envelope["ok"] = false;
    envelope["receipt"]["status"] = "rejected";
    envelope["error"] = {
        {"code", result.value("code", "action_rejected")},
        {"message", result.value("detail", "The requested action was rejected.")}
    };
    return envelope.dump();
}

} // namespace

CommandRegistry::CommandRegistry()
    : owned_world_(std::make_unique<World>()),
      world_(owned_world_.get()),
      owned_assets_(std::make_unique<AssetRegistry>()),
      assets_(owned_assets_.get()) {
    static_cast<void>(world_->load_scene(make_bootstrap_scene_document()));
    register_commands();
}

CommandRegistry::CommandRegistry(World& world)
    : world_(&world),
      owned_assets_(std::make_unique<AssetRegistry>()),
      assets_(owned_assets_.get()) {
    register_commands();
}

CommandRegistry::CommandRegistry(World& world, AssetRegistry& assets)
    : world_(&world), assets_(&assets) {
    register_commands();
}

CommandRegistry::~CommandRegistry() = default;

void CommandRegistry::attach_project_ui_authoring(ProjectUiAuthoringSession& session) {
    project_ui_authoring_=std::make_unique<ProjectUiAuthoringCommandService>(session);
}

void CommandRegistry::attach_editor_context(
    std::function<std::string()> observe,
    std::function<std::string(std::string_view)> apply_intent) {
    editor_context_observe_=std::move(observe);
    editor_context_apply_intent_=std::move(apply_intent);
}

void CommandRegistry::attach_asset_document_reader(
    std::function<AssetDocumentReadResult(std::string_view, std::size_t)> reader) {
    asset_document_reader_ = std::move(reader);
}

void CommandRegistry::register_commands() {
    commands_.push_back(CommandDefinition{
        .name = "engine.status",
        .description = "Return the complete engine module graph and lifecycle status.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "offline",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","lifecycle","moduleCount","modules"]})",
        .handler = [](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            EngineHost host;
            host.register_default_modules();
            if (!host.initialize(true)) {
                throw std::runtime_error(std::string(host.last_error()));
            }
            return host.status_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "semantic.conventions",
        .description = "Return the core semantic field conventions used by engine observations.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "offline",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","registryId","conventions"]})",
        .handler = [](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            return SemanticConventionRegistry{}.schema_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "schema.get",
        .description = "Return the reflected engine component schema.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "offline",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["protocolVersion","components"]})",
        .handler = [](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            const World world;
            return world.schema_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "scene.validate",
        .description = "Validate and canonicalize a Noemancer scene document without changing runtime state.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "offline",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["document"],"properties":{"document":{"type":"object"},"sourceUri":{"type":"string","default":"memory://scene"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["valid","errors"]})",
        .handler = [](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            const auto parsed = SceneDocumentCodec::parse_json(
                input.at("document").dump(),
                input.at("sourceUri").get<std::string>());
            Json errors = Json::array();
            for (const auto& error : parsed.errors) {
                errors.push_back({
                    {"code", error.code},
                    {"path", error.path},
                    {"message", error.message}
                });
            }
            Json result = {
                {"valid", static_cast<bool>(parsed)},
                {"errors", std::move(errors)}
            };
            if (parsed) {
                result["canonicalDocument"] = Json::parse(
                    SceneDocumentCodec::write_canonical_json(*parsed.document));
            }
            return result.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.snapshot",
        .description = "Return a read-only snapshot of the current authoritative World.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "offline",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","scope","entities"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            return world_->snapshot_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.query",
        .description = "Return a focused, field-masked and byte-budgeted semantic observation bundle.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"entityIds":{"type":"array","default":[]},"fields":{"type":"array","default":[]},"depth":{"type":"integer","minimum":0,"maximum":8,"default":1},"byteBudget":{"type":"integer","minimum":512,"maximum":1048576,"default":16384},"cursor":{"type":"integer","minimum":0,"default":0}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","scope","entities","truncated"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            ObservationQuery query{
                .depth = input.at("depth").get<std::size_t>(),
                .byte_budget = input.at("byteBudget").get<std::size_t>(),
                .cursor = input.at("cursor").get<std::size_t>()
            };
            for (const auto& id : input.at("entityIds")) query.entity_ids.push_back(id.get<std::string>());
            const std::unordered_set<std::string> allowed_fields{
                "identity", "hierarchy", "source", "transform", "velocity", "render", "physics", "animation"
            };
            for (const auto& field_value : input.at("fields")) {
                const auto field = field_value.get<std::string>();
                if (!allowed_fields.contains(field)) {
                    throw std::invalid_argument("Unknown observation field: " + field);
                }
                query.fields.push_back(field);
            }
            return world_->observe_json(query);
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.delta",
        .description = "Return semantic changes since a known World revision or request a focused resync.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["sinceRevision"],"properties":{"sinceRevision":{"type":"integer","minimum":0}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","fromRevision","toRevision","resyncRequired","changes"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return world_->delta_json(input.at("sinceRevision").get<std::uint64_t>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "render.observe",
        .description = "Return the ECS-derived cameras, directional lights, renderables and material parameters for the active scene.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","scope","cameras","directionalLights","renderables"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            return world_->render_observation_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "render.graph.inspect",
        .description = "Return the validated render resources, pipelines, pass dependencies and deterministic execution order.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "offline",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","graphId","valid","resources","passes","executionOrder","errors"]})",
        .handler = [](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            return render_graph_json(make_forward_render_graph());
        }
    });

    commands_.push_back(CommandDefinition{
        .name="asset.sprite.pressure",
        .description="Generate a bounded deterministic long-sequence Sprite workload and compare the current single atlas with a deterministic multi-page incremental Cook plan without claiming encoded or GPU timing.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="offline",.task_kind="bounded",
        .input_schema_json=R"({"type":"object","properties":{"frameCount":{"type":"integer","minimum":1,"maximum":16384,"default":1024},"clipCount":{"type":"integer","minimum":1,"maximum":256,"default":8},"framesPerClip":{"type":"integer","minimum":1,"maximum":16384,"default":256},"atlasColumns":{"type":"integer","minimum":1,"maximum":256,"default":64},"frameEdge":{"type":"integer","minimum":1,"maximum":256,"default":16},"plannedPageEdge":{"type":"integer","minimum":1,"maximum":8192,"default":1024},"plannedPadding":{"type":"integer","minimum":0,"maximum":256,"default":1},"changedFrameIndex":{"type":"integer","minimum":0,"maximum":16383,"description":"Optional frame index used to estimate page-local incremental Cook."}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","valid","code","workload","atlas","pagePlan","scope"]})",
        .handler=[](const std::string_view arguments) {const auto input=parse_object(arguments);
            return sprite_pressure_report_json(input.value("frameCount",1024U),input.value("clipCount",8U),
                input.value("framesPerClip",256U),input.value("atlasColumns",64U),input.value("frameEdge",16U),
                input.value("plannedPageEdge",1024U),input.value("plannedPadding",1U),
                input.value("changedFrameIndex",std::numeric_limits<std::uint32_t>::max()));}
    });

    commands_.push_back(CommandDefinition{
        .name="render.tilemap.pressure",
        .description="Generate a bounded deterministic dense or sparse Tilemap workload and report chunk culling plus current 16-instance submission estimates without claiming GPU timing.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="offline",.task_kind="bounded",
        .input_schema_json=R"({"type":"object","properties":{"chunkColumns":{"type":"integer","minimum":1,"maximum":64,"default":8},"chunkRows":{"type":"integer","minimum":1,"maximum":64,"default":8},"chunkSize":{"type":"integer","minimum":4,"maximum":32,"default":16},"visibleChunkRadius":{"type":"integer","minimum":0,"maximum":32,"default":2},"occupiedCellsPerChunk":{"type":"integer","minimum":1,"maximum":1024,"description":"Omit for a dense chunk; otherwise limits generated occupied cells per chunk."}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","valid","code"]})",
        .handler=[](const std::string_view arguments) {const auto input=parse_object(arguments);
            return tilemap_pressure_report_json(input.value("chunkColumns",8U),input.value("chunkRows",8U),
                input.value("chunkSize",16U),input.value("visibleChunkRadius",2U),input.value("occupiedCellsPerChunk",0U));}
    });

    commands_.push_back(CommandDefinition{
        .name = "physics.observe",
        .description = "Return focused rigid-body, collider, velocity and contact state from the active simulation.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","backend","bodies","contacts"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            return world_->physics_observation_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.observe",
        .description = "Return active animation clips, playback cursors and deterministic sampling backend state.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","backend","players"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            return world_->animation_observation_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "render.sprite.observe",
        .description = "Return resolved sprite playback, visibility and deterministic draw-order state from the active World.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"entityId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","filterEntityId","items"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return world_->sprite_observation_json(input.value("entityId", std::string{}));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.state-machine.inspect",
        .description = "Return the bounded declarative state graph and live state/parameter instance for one animated entity.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","entityId","definition","instance"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments);
            return world_->animation_state_machine_json(input.at("entityId").get<std::string>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.state-machine.parameter.set",
        .description = "Set one declared parameter on an entity's registered animation state machine; the next simulation tick evaluates deterministic transitions.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","parameter","value"],"properties":{"entityId":{"type":"string"},"parameter":{"type":"string","minLength":1},"value":{"type":"number"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","entityId","parameter","revision"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments);
            return world_->animation_state_parameter_set_json(input.at("entityId").get<std::string>(),
                input.at("parameter").get<std::string>(),input.at("value").get<float>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.graph.instance.observe",
        .description = "Return one entity's bounded Animation Graph definition and live parameter instance from this Command Registry's attached World authority.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string","minLength":1}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","entityId","definition","instance"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments);
            return world_->animation_graph_json(input.at("entityId").get<std::string>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.graph.parameter.set",
        .description = "Set one parameter declared by an entity's registered Animation Graph without exposing runtime or middleware handles.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","parameter","value"],"properties":{"entityId":{"type":"string","minLength":1},"parameter":{"type":"string","minLength":1},"value":{"type":"number"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","entityId","parameter","revision"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments);
            return world_->animation_graph_parameter_set_json(input.at("entityId").get<std::string>(),
                input.at("parameter").get<std::string>(),input.at("value").get<float>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.graph.inspect",
        .description = "Inspect a validated Animation Graph source asset, its stable fingerprint, dependencies, and bounded author layout.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["assetId"],"properties":{"assetId":{"type":"string","minLength":1}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","assetId","fingerprint","definition","dependencies"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);const auto asset_id=input.at("assetId").get<std::string>();
            const auto* asset=assets_->find(asset_id);
            if(asset==nullptr||!asset->available||(asset->kind!="AnimationGraph"&&!asset->relative_path.ends_with(".animation-graph.json")))
                return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",false},
                    {"code","animation.graph-not-found"},{"assetId",asset_id},{"fingerprint",nullptr},
                    {"definition",nullptr},{"dependencies",Json::array()}}.dump();
            AssetDocumentReadResult source;
            if (asset_document_reader_) {
                source = asset_document_reader_(asset_id, animation_graph_patch_max_input_bytes);
            } else {
                const auto detached = assets_->read_animation_graph_source(asset_id);
                source = {detached.success, detached.code, detached.detail, asset_id, {}, detached.source};
            }
            if (!source.success) return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",false},
                {"code",source.code.empty()?"animation.graph-source-unavailable":source.code},
                {"detail",source.detail},{"assetId",asset_id},{"fingerprint",nullptr},
                {"definition",nullptr},{"dependencies",Json::array()}}.dump();
            if (!source.asset_id.empty() && source.asset_id != asset_id)
                return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",false},
                    {"code","animation.graph-reader-identity-mismatch"},
                    {"detail","The attached document reader returned a different Asset ID."},
                    {"assetId",asset_id},{"fingerprint",nullptr},{"definition",nullptr},
                    {"dependencies",Json::array()}}.dump();
            if (asset_document_reader_ && source.content_hash != asset->content_hash)
                return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",false},
                    {"code","animation.graph-reader-content-mismatch"},
                    {"detail","The attached document reader did not verify the current Registry content identity."},
                    {"assetId",asset_id},{"fingerprint",nullptr},{"definition",nullptr},
                    {"dependencies",Json::array()}}.dump();
            if(source.text.size()>animation_graph_patch_max_input_bytes)
                return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",false},
                    {"code","animation.graph-source-too-large"},
                    {"detail","The attached Animation Graph source exceeded its requested byte budget."},
                    {"assetId",asset_id},{"fingerprint",nullptr},{"definition",nullptr},
                    {"dependencies",Json::array()}}.dump();
            const auto validation=assets_->validate_animation_graph_source(asset_id,source.text);
            if(!validation.valid)return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",false},
                {"code",validation.code},{"detail",validation.detail},{"assetId",asset_id},{"fingerprint",nullptr},
                {"definition",nullptr},{"dependencies",Json::array()}}.dump();
            const auto parsed=AnimationGraphCodec::parse_json(source.text);
            if(!parsed)return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",false},
                {"code",parsed.code},{"detail",parsed.detail},{"assetId",asset_id},{"fingerprint",nullptr},
                {"definition",nullptr},{"dependencies",Json::array()}}.dump();
            return Json{{"schemaVersion","noemancer.animation-graph-inspection/0.1"},{"valid",true},{"code","ok"},
                {"assetId",asset_id},{"fingerprint",AnimationGraphPatch::fingerprint(*parsed.document)},
                {"definition",Json::parse(AnimationGraphCodec::write_canonical_json(*parsed.document))},
                {"dependencies",AnimationGraphCodec::asset_dependencies(*parsed.document)}}.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.graph.patch",
        .description = "Plan or atomically commit a bounded semantic Animation Graph patch with optimistic fingerprint checks and Runtime reload.",
        .access = "write", .idempotent = true, .supports_dry_run = true, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["assetId","manager","operations"],"properties":{"assetId":{"type":"string","minLength":1},"manager":{"type":"string","minLength":1},"expectedFingerprint":{"type":"string","default":""},"dryRun":{"type":"boolean","default":true},"operations":{"type":"array","minItems":1,"maxItems":256,"items":{"oneOf":[{"type":"object","required":["operation","nodeId","x","y"],"properties":{"operation":{"const":"setNodePosition"},"nodeId":{"type":"string","minLength":1},"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false},{"type":"object","required":["operation","layerId","weight"],"properties":{"operation":{"const":"setLayerWeight"},"layerId":{"type":"string","minLength":1},"weight":{"type":"number","minimum":0,"maximum":1}},"additionalProperties":false},{"type":"object","required":["operation","layerId","parameter"],"properties":{"operation":{"const":"setLayerWeightParameter"},"layerId":{"type":"string","minLength":1},"parameter":{"type":"string"}},"additionalProperties":false},{"type":"object","required":["operation","maskId","jointName","weight"],"properties":{"operation":{"const":"setMaskJointWeight"},"maskId":{"type":"string","minLength":1},"jointName":{"type":"string","minLength":1},"weight":{"type":"number","minimum":0,"maximum":1}},"additionalProperties":false},{"type":"object","required":["operation","nodeId","kind"],"properties":{"operation":{"const":"createNode"},"nodeId":{"type":"string","minLength":1},"kind":{"enum":["clip","state-machine","blend-1d"]},"clipAsset":{"type":"string"},"looping":{"type":"boolean","default":true},"stateMachineAsset":{"type":"string"},"parameter":{"type":"string"},"children":{"type":"array","maxItems":32,"items":{"type":"object","required":["nodeId","threshold"],"properties":{"nodeId":{"type":"string","minLength":1},"threshold":{"type":"number"}},"additionalProperties":false}}},"additionalProperties":false},{"type":"object","required":["operation","nodeId"],"properties":{"operation":{"const":"deleteNode"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false},{"type":"object","required":["operation","blendNodeId","childNodeId","threshold"],"properties":{"operation":{"const":"connectBlend1DChild"},"blendNodeId":{"type":"string","minLength":1},"childNodeId":{"type":"string","minLength":1},"threshold":{"type":"number"}},"additionalProperties":false},{"type":"object","required":["operation","blendNodeId","childNodeId"],"properties":{"operation":{"const":"disconnectBlend1DChild"},"blendNodeId":{"type":"string","minLength":1},"childNodeId":{"type":"string","minLength":1}},"additionalProperties":false}]}}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","dryRun","assetId","source","plan","receipt","sourceReceipt","runtimeReloaded"]})",
        .handler = [this](const std::string_view arguments) {
            if(arguments.size()>animation_graph_patch_max_input_bytes)
                throw std::invalid_argument("Animation Graph patch input exceeds the 1 MiB transaction limit");
            const auto input=parse_object(arguments);const auto asset_id=input.at("assetId").get<std::string>();
            const auto dry_run=input.value("dryRun",true);const auto manager=input.at("manager").get<std::string>();
            const auto failure=[&](const std::string_view code,const std::string_view detail) {return Json{
                {"schemaVersion","noemancer.animation-graph-operation/0.2"},{"success",false},{"code",code},{"detail",detail},
                {"dryRun",dry_run},{"assetId",asset_id},{"source",nullptr},{"plan",nullptr},{"receipt",nullptr},
                {"sourceReceipt",nullptr},{"runtimeReloaded",false}}.dump();};
            const auto* asset=assets_->find(asset_id);
            if(asset==nullptr||!asset->available||(asset->kind!="AnimationGraph"&&!asset->relative_path.ends_with(".animation-graph.json")))
                return failure("animation.graph-not-found","Asset Registry has no available Animation Graph with this ID.");
            const auto bounded_source=assets_->read_animation_graph_source(asset_id);
            if(!bounded_source.success)return failure(bounded_source.code,bounded_source.detail);
            const auto& source=bounded_source.source;
            const auto parsed=AnimationGraphCodec::parse_json(source);if(!parsed)return failure(parsed.code,parsed.detail);
            if(parsed.document->asset_id!=asset_id)return failure("animation.graph-identity-mismatch",
                "Registry assetId and Animation Graph document assetId must match.");
            const auto source_path=assets_->source_path(*asset).generic_string();
            const auto& operation_values=input.at("operations");
            if(operation_values.size()>animation_graph_patch_max_operations)
                throw std::invalid_argument("Animation Graph patch contains more than 256 operations");
            std::vector<AnimationGraphPatchOperation> operations;operations.reserve(operation_values.size());
            for(const auto& value:operation_values) {
                if(!value.is_object())throw std::invalid_argument("Every Animation Graph operation must be an object");
                const auto operation=value.at("operation").get<std::string>();
                const auto require_fields=[&](const std::initializer_list<std::string_view> allowed) {
                    for(const auto& [field,unused]:value.items()) {
                        static_cast<void>(unused);
                        if(std::ranges::find(allowed,std::string_view{field})==allowed.end())
                            throw std::invalid_argument("Unexpected field '"+field+"' for Animation Graph operation "+operation);
                    }
                };
                if(operation=="setNodePosition") {
                    require_fields({"operation","nodeId","x","y"});
                    operations.push_back(AnimationGraphPatchOperation::set_node_position(value.at("nodeId").get<std::string>(),value.at("x").get<float>(),value.at("y").get<float>()));
                } else if(operation=="setLayerWeight") {
                    require_fields({"operation","layerId","weight"});
                    operations.push_back(AnimationGraphPatchOperation::set_layer_weight(value.at("layerId").get<std::string>(),
                        value.at("weight").get<float>()));
                } else if(operation=="setLayerWeightParameter") {
                    require_fields({"operation","layerId","parameter"});
                    operations.push_back(AnimationGraphPatchOperation::set_layer_weight_parameter(value.at("layerId").get<std::string>(),value.at("parameter").get<std::string>()));
                } else if(operation=="setMaskJointWeight") {
                    require_fields({"operation","maskId","jointName","weight"});
                    operations.push_back(AnimationGraphPatchOperation::set_mask_joint_weight(value.at("maskId").get<std::string>(),value.at("jointName").get<std::string>(),
                        value.at("weight").get<float>()));
                }
                else if(operation=="createNode") {
                    require_fields({"operation","nodeId","kind","clipAsset","looping","stateMachineAsset","parameter","children"});
                    const auto kind=value.at("kind").get<std::string>();
                    auto topology=AnimationGraphPatchOperation::create_node(value.at("nodeId").get<std::string>(),kind,
                        value.value("clipAsset",std::string{}),value.value("looping",true),
                        value.value("stateMachineAsset",std::string{}),value.value("parameter",std::string{}));
                    if(value.contains("children")&&value.at("children").size()>animation_graph_patch_max_children)
                        throw std::invalid_argument("Animation Graph createNode contains more than 32 children");
                    if(value.contains("children"))for(const auto& child:value.at("children")) {
                        if(!child.is_object()||child.size()!=2U||!child.contains("nodeId")||!child.contains("threshold"))
                            throw std::invalid_argument("Each createNode child must contain only nodeId and threshold");
                        topology.children.push_back({child.at("nodeId").get<std::string>(),child.at("threshold").get<float>()});
                    }
                    operations.push_back(std::move(topology));
                } else if(operation=="deleteNode") {
                    require_fields({"operation","nodeId"});
                    operations.push_back(AnimationGraphPatchOperation::delete_node(value.at("nodeId").get<std::string>()));
                } else if(operation=="connectBlend1DChild") {
                    require_fields({"operation","blendNodeId","childNodeId","threshold"});
                    operations.push_back(AnimationGraphPatchOperation::connect_blend_1d_child(value.at("blendNodeId").get<std::string>(),
                        value.at("childNodeId").get<std::string>(),value.at("threshold").get<float>()));
                } else if(operation=="disconnectBlend1DChild") {
                    require_fields({"operation","blendNodeId","childNodeId"});
                    operations.push_back(AnimationGraphPatchOperation::disconnect_blend_1d_child(value.at("blendNodeId").get<std::string>(),
                        value.at("childNodeId").get<std::string>()));
                }
                else throw std::invalid_argument("Unsupported Animation Graph operation: "+operation);
            }
            auto document=*parsed.document;const auto plan=AnimationGraphPatch::plan(document,std::move(operations),input.value("expectedFingerprint",std::string{}));
            auto receipt=AnimationGraphPatch::apply(document,plan,dry_run);std::optional<AssetSourceEditReceipt> source_receipt;
            bool runtime_reloaded=false;
            std::string candidate_source;
            if(receipt.success&&plan.changed_operation_count>0U) {
                if(!plan.result)return failure("animation.graph.patch-candidate-missing","A successful patch plan has no candidate document.");
                candidate_source=AnimationGraphCodec::write_canonical_json(*plan.result)+"\n";
                const auto validation=assets_->validate_animation_graph_source(asset_id,candidate_source);
                if(!validation.valid)return failure(validation.code,validation.detail);
            }
            if(receipt.success&&!dry_run&&plan.changed_operation_count>0U) {
                source_receipt=assets_->commit_text_source(asset_id,candidate_source,manager,source);
                if(!source_receipt->success)return failure(source_receipt->code,source_receipt->detail);
                runtime_reloaded=world_->register_animation_graph(document);
                if(!runtime_reloaded) {
                    const auto rollback=assets_->rollback_text_source(
                        source_receipt->transaction_id,"animation.graph.runtime-rollback");
                    return Json{{"schemaVersion","noemancer.animation-graph-operation/0.2"},{"success",false},
                        {"code",rollback.success?"animation.graph-runtime-reload-failed":
                            "animation.graph-runtime-reload-rollback-failed"},
                        {"detail",rollback.success?"Runtime rejected the Graph; its exact source transaction was aborted.":
                            "Runtime rejected the Graph and its exact source transaction could not be aborted."},
                        {"dryRun",false},{"assetId",asset_id},{"source",source_path},
                        {"plan",Json::parse(plan.to_json())},{"receipt",Json::parse(receipt.to_json())},
                        {"sourceReceipt",asset_source_receipt_json(*source_receipt)},
                        {"rollback",asset_source_receipt_json(rollback)},{"runtimeReloaded",false}}.dump();
                }
            }
            return Json{{"schemaVersion","noemancer.animation-graph-operation/0.2"},{"success",receipt.success},{"code",receipt.code},
                {"dryRun",dry_run},{"assetId",asset_id},{"source",source_path},
                {"plan",Json::parse(plan.to_json())},{"receipt",Json::parse(receipt.to_json())},
                {"sourceReceipt",source_receipt?asset_source_receipt_json(*source_receipt):Json(nullptr)},
                {"runtimeReloaded",runtime_reloaded}}.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "input.actions.observe",
        .description = "Return deterministic logical input actions, bindings, values and edge transitions.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","actions"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->input_observation_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "input.source.inject",
        .description = "Inject one normalized logical device source for headless tests, replay and Agent-driven verification.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["source","value"],"properties":{"source":{"type":"string"},"value":{"type":"number","minimum":-1,"maximum":1}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","source","value","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->inject_input_json(input.at("source").get<std::string>(),input.at("value").get<float>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "audio.mixer.observe",
        .description = "Return the logical audio bus graph and active voice state without exposing backend handles.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","backend","buses","voices"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->audio_observation_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name="audio.clip.load",
        .description="Register a Registry audio asset for asynchronous resident or streaming playback.",
        .access="write",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["assetId"],"properties":{"assetId":{"type":"string"},"storage":{"type":"string","enum":["resident","stream"],"default":"resident"}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","valid","code","detail","assetId","contentHash","storage","state","revisionBefore","revisionAfter"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);const auto id=input.at("assetId").get<std::string>();
            const auto* asset=assets_->find(id);if(asset==nullptr||!asset->available||
                (asset->extension!=".wav"&&asset->extension!=".ogg"&&asset->extension!=".flac"&&asset->extension!=".mp3"))
                return Json{{"schemaVersion","noemancer.audio-clip/0.1"},{"valid",false},{"code","audio.asset-unavailable"},{"detail","Asset Registry has no available supported audio source with this ID."},
                    {"assetId",id},{"contentHash",""},{"storage","none"},{"state","rejected"},
                    {"revisionBefore",world_->audio_revision()},{"revisionAfter",world_->audio_revision()}}.dump();
            const auto storage=input.value("storage",std::string("resident"));
            return world_->register_audio_asset_json(id,asset->content_hash,
                storage=="stream"?AudioAssetStorage::stream:AudioAssetStorage::resident);}
    });

    commands_.push_back(CommandDefinition{
        .name = "audio.bus.set",
        .description = "Set a versioned logical mixer bus gain and mute state.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["busId","gain","muted"],"properties":{"busId":{"type":"string"},"gain":{"type":"number","minimum":0,"maximum":4},"muted":{"type":"boolean"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->set_audio_bus_json(input.at("busId").get<std::string>(),input.at("gain").get<float>(),input.at("muted").get<bool>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "audio.voice.play",
        .description = "Create a logical audio voice routed through a named bus; backend playback is profile-dependent.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["assetId"],"properties":{"assetId":{"type":"string"},"busId":{"type":"string","default":"audio.sfx"},"gain":{"type":"number","minimum":0,"default":1},"pitch":{"type":"number","exclusiveMinimum":0,"default":1},"looping":{"type":"boolean","default":false}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","voiceId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->play_audio_json(input.at("assetId").get<std::string>(),input.value("busId",std::string("audio.sfx")),input.value("gain",1.0F),input.value("pitch",1.0F),input.value("looping",false)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "audio.listener.set",
        .description = "Set the engine-owned 3D audio listener pose using plain vectors; no backend handle is exposed.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["position","forward","up"],"properties":{"position":{"type":"object","required":["x","y","z"]},"forward":{"type":"object","required":["x","y","z"]},"up":{"type":"object","required":["x","y","z"]}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->set_audio_listener_json(
            parse_vector3(input.at("position")),parse_vector3(input.at("forward")),parse_vector3(input.at("up"))); }
    });

    commands_.push_back(CommandDefinition{
        .name = "audio.voice.spatial.set",
        .description = "Configure bounded 3D attenuation and equal-power panning for a stable logical audio voice ID.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["voiceId","position"],"properties":{"voiceId":{"type":"integer","minimum":1},"spatial":{"type":"boolean","default":true},"position":{"type":"object","required":["x","y","z"]},"minimumDistance":{"type":"number","exclusiveMinimum":0,"default":1},"maximumDistance":{"type":"number","exclusiveMinimum":0,"default":50},"rolloff":{"type":"number","minimum":0,"default":1}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","voiceId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->set_audio_voice_spatial_json(
            input.at("voiceId").get<std::uint64_t>(),input.value("spatial",true),parse_vector3(input.at("position")),
            input.value("minimumDistance",1.0F),input.value("maximumDistance",50.0F),input.value("rolloff",1.0F)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.events.observe",
        .description = "Return a bounded deterministic gameplay event stream linking input and future animation, physics and VFX consumers.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"maxEvents":{"type":"integer","minimum":1,"maximum":256,"default":32}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","events"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->gameplay_observation_json(input.value("maxEvents",32U)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.character-motor-2d.observe",
        .description = "Observe a 2D character motor's normalized input, ground evidence, chosen control decision, reason, tuning, position and velocity.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"entityId":{"type":"string","default":""}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","coordinateSystem","motors"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->character_motor_2d_observation_json(input.value("entityId",std::string{})); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.camera-follow-2d.observe",
        .description = "Observe a side-scrolling camera's target, dead zone, look-ahead, bounded center, smoothing decision and resulting pose.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"entityId":{"type":"string","default":""}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","follows"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->camera_follow_2d_observation_json(input.value("entityId",std::string{})); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.ability.catalog",
        .description = "Return stable Ability definitions, costs, cooldowns, tag gates, emitted events and linked Effects.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","definitions"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->gameplay_ability_catalog_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.effect.catalog",
        .description = "Return stable Gameplay Effect definitions with durations, typed attribute modifiers, granted tags and animation cues.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","definitions"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->gameplay_effect_catalog_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.ability.observe",
        .description = "Observe bounded plain-data attributes, tags, grants and cooldowns for all Ability actors or one stable entity ID.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"entityId":{"type":"string","default":""}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","actors"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->gameplay_ability_observation_json(input.value("entityId",std::string{})); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.ability.grant",
        .description = "Idempotently grant a catalog Ability to an existing entity through stable IDs.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","abilityId"],"properties":{"entityId":{"type":"string"},"abilityId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","entityId","abilityId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->gameplay_ability_grant_json(input.at("entityId").get<std::string>(),input.at("abilityId").get<std::string>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.ability.activate",
        .description = "Activate a granted Ability, atomically applying tag/cost/cooldown gates, linked Effects and structured events.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","abilityId"],"properties":{"entityId":{"type":"string"},"abilityId":{"type":"string"},"targetId":{"type":"string","default":""}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","entityId","abilityId","targetId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->gameplay_ability_activate_json(
            input.at("entityId").get<std::string>(),input.at("abilityId").get<std::string>(),input.value("targetId",std::string{})); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.effect.apply",
        .description = "Apply one catalog Effect to an existing target and return bounded attribute/tag/event evidence.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["sourceEntityId","targetEntityId","effectId"],"properties":{"sourceEntityId":{"type":"string"},"targetEntityId":{"type":"string"},"effectId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","effectId","sourceEntityId","targetEntityId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->gameplay_effect_apply_json(
            input.at("sourceEntityId").get<std::string>(),input.at("targetEntityId").get<std::string>(),input.at("effectId").get<std::string>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.ability.activate-ray",
        .description = "Resolve a real Jolt ray hit and activate a granted Ability against the stable hit entity in one observable operation.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","abilityId","origin","direction"],"properties":{"entityId":{"type":"string"},"abilityId":{"type":"string"},"origin":{"type":"object","required":["x","y","z"]},"direction":{"type":"object","required":["x","y","z"]}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","entityId","abilityId","hit","activation"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->gameplay_ability_activate_ray_json(
            input.at("entityId").get<std::string>(),input.at("abilityId").get<std::string>(),
            parse_vector3(input.at("origin")),parse_vector3(input.at("direction"))); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.ability.activate-sweep",
        .description = "Sweep a real Jolt sphere while ignoring the source body, then activate a granted Ability against the nearest stable hit entity.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","abilityId","origin","direction","radius"],"properties":{"entityId":{"type":"string"},"abilityId":{"type":"string"},"origin":{"type":"object","required":["x","y","z"]},"direction":{"type":"object","required":["x","y","z"]},"radius":{"type":"number","minimum":0.01,"maximum":10.0}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","entityId","abilityId","shape","hit","activation"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->gameplay_ability_activate_sweep_json(
            input.at("entityId").get<std::string>(),input.at("abilityId").get<std::string>(),
            parse_vector3(input.at("origin")),parse_vector3(input.at("direction")),input.at("radius").get<float>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "network.profile.describe",
        .description = "Describe the optional authoritative networking Profile, replication contract, prediction seam and current transport boundary.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "offline", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","profileId","enabledByDefault","authorityModel","transport"]})",
        .handler = [](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return NetworkReplicationRuntime::profile_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "network.snapshot.preview",
        .description = "Project a bounded canonical server-authoritative replication snapshot from the attached World without runtime handles.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"tick":{"type":"integer","minimum":0,"default":0},"maxEntities":{"type":"integer","minimum":0,"maximum":4096,"default":256}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","serverPeerId","tick","worldRevision","entityCount","entities","truncated"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->network_snapshot_preview_json(
            input.value("tick",0ULL), input.value("maxEntities",256U)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "network.loopback.verify",
        .description = "Run an in-memory authoritative server/client snapshot, delta, prediction and reconciliation verification on the attached World.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","converged","predictionAccepted","reconciliation"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->network_loopback_verify_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "network.transport.verify",
        .description = "Exchange a bounded state datagram and acknowledgement through real kernel UDP sockets on loopback.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "detached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"payloadBytes":{"type":"integer","minimum":1,"maximum":1200,"default":256}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","transport","channel","payloadBytes","roundTripMicroseconds"]})",
        .handler = [](const std::string_view arguments) { const auto input=parse_object(arguments); return verify_udp_loopback_transport_json(input.value("payloadBytes",256U)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.graph.inspect",
        .description = "Return the canonical versioned VFX Graph IR, including GPU-preferred and deterministic CPU-reference contracts.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"graphId":{"type":"string","default":"vfx.debug-impact"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","graphId","valid","code"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->vfx_graph_json(input.value("graphId",std::string("vfx.debug-impact"))); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.gpu-program.inspect",
        .description = "Return the graph-derived cross-backend VFX compute ABI, artifact formats, bindings, dispatch shape, working set and honest activation boundary.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"graphId":{"type":"string","default":"vfx.debug-impact"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","graphId"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->vfx_gpu_program_json(
            input.value("graphId",std::string("vfx.debug-impact"))); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.observe",
        .description = "Return bounded live VFX pool, Data Channel bindings, deterministic digest and particle samples.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"maxParticles":{"type":"integer","minimum":0,"maximum":256,"default":32}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","aliveCount","digest","particles"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->vfx_observation_json(input.value("maxParticles",32U)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.benchmark",
        .description = "Run a bounded deterministic CPU-reference SoA particle workload and return honest throughput, digest and scope evidence.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "bounded",
        .input_schema_json = R"({"type":"object","properties":{"graphId":{"type":"string","default":"vfx.debug-impact"},"particleCount":{"type":"integer","minimum":1,"maximum":65536,"default":16384},"steps":{"type":"integer","minimum":1,"maximum":600,"default":120},"fixedDeltaSeconds":{"type":"number","minimum":0.0001,"maximum":0.1,"default":0.016666667}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","graphId","layout","requestedParticleCount","steps","elapsedMicroseconds","digest","scope"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments);
            return world_->vfx_benchmark_json(input.value("graphId",std::string("vfx.debug-impact")),
                input.value("particleCount",16384U),input.value("steps",120U),input.value("fixedDeltaSeconds",1.0F/60.0F)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.spawn",
        .description = "Spawn one deterministic VFX graph burst at an explicit world position and return a bounded mutation receipt.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["position"],"properties":{"graphId":{"type":"string","default":"vfx.debug-impact"},"position":{"type":"object"},"seed":{"type":"integer","minimum":0,"default":1}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","graphId","position","aliveBefore","aliveAfter","spawned"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); const auto position=parse_vector(input.at("position"),"position");
            return world_->vfx_spawn_json(input.value("graphId",std::string("vfx.debug-impact")),
                {static_cast<float>(position.x),static_cast<float>(position.y),static_cast<float>(position.z)},input.value("seed",1ULL)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.preview",
        .description = "Run a bounded fixed-seed CPU reference preview without mutating the attached World.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"graphId":{"type":"string","default":"vfx.debug-impact"},"seed":{"type":"integer","minimum":0,"default":1},"steps":{"type":"integer","minimum":0,"maximum":3600,"default":30},"fixedDeltaSeconds":{"type":"number","exclusiveMinimum":0,"maximum":0.1,"default":0.016666667},"maxParticles":{"type":"integer","minimum":0,"maximum":256,"default":32}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","graphId"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->vfx_preview_json(
            input.value("graphId",std::string("vfx.debug-impact")), input.value("seed",1ULL), input.value("steps",30U),
            input.value("fixedDeltaSeconds",1.0F/60.0F), input.value("maxParticles",32U)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.graph.patch.plan",
        .description = "Validate a JSON Merge Patch and return a revision-bound immutable VFX Graph plan with changed paths and canonical candidate.",
        .access = "write", .idempotent = true, .supports_dry_run = true, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["graphId","patch","baseRevision"],"properties":{"graphId":{"type":"string"},"patch":{"type":"object"},"baseRevision":{"type":"integer","minimum":0}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","graphId","baseRevision"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->vfx_plan_graph_patch_json(
            input.at("graphId").get<std::string>(), input.at("patch").dump(), input.at("baseRevision").get<std::uint64_t>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.graph.patch.apply",
        .description = "Dry-run or atomically apply an integrity-checked, revision-bound VFX Graph plan.",
        .access = "write", .idempotent = false, .supports_dry_run = true, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["plan"],"properties":{"plan":{"type":"object"},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","dryRun","code","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->vfx_apply_graph_plan_json(
            input.at("plan").dump(), input.at("dryRun").get<bool>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "vfx.graph.undo",
        .description = "Rollback the most recent committed VFX Graph patch when its runtime revision still matches.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["expectedRevision"],"properties":{"expectedRevision":{"type":"integer","minimum":0}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","dryRun","code","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->vfx_undo_graph_json(
            input.at("expectedRevision").get<std::uint64_t>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.prefab.spawn",
        .description = "Instantiate a persisted ECS entity from an existing scene entity template with a new stable ID.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["sourceEntityId","newEntityId","position"],"properties":{"sourceEntityId":{"type":"string"},"newEntityId":{"type":"string"},"displayName":{"type":"string","default":""},"position":{"type":"object"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); const auto position=parse_vector(input.at("position"),"position");
            return world_->spawn_prefab_json(input.at("sourceEntityId").get<std::string>(),input.at("newEntityId").get<std::string>(),
                input.value("displayName",std::string{}),{static_cast<float>(position.x),static_cast<float>(position.y),static_cast<float>(position.z)}); }
    });

    commands_.push_back(CommandDefinition{
        .name="gameplay.prefab.export",
        .description="Export one canonical scene entity as a portable versioned Prefab document with no runtime handles.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","valid","code","sourceSceneGuid","entity"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);return world_->export_prefab_json(input.at("entityId").get<std::string>());}
    });
    commands_.push_back(CommandDefinition{
        .name="gameplay.prefab.instantiate",
        .description="Validate and instantiate a portable Prefab document into the authoritative Scene/ECS World.",
        .access="write",.idempotent=false,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["prefab","newEntityId","position"],"properties":{"prefab":{"type":"object"},"newEntityId":{"type":"string"},"displayName":{"type":"string","default":""},"position":{"type":"object"}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","operation","revisionBefore","revisionAfter"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);const auto position=parse_vector(input.at("position"),"position");
            return world_->instantiate_prefab_json(input.at("prefab").dump(),input.at("newEntityId").get<std::string>(),input.value("displayName",std::string{}),
                {static_cast<float>(position.x),static_cast<float>(position.y),static_cast<float>(position.z)});}
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.entity.despawn",
        .description = "Atomically remove a persisted ECS entity when scene hierarchy preconditions allow it.",
        .access = "write", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->despawn_entity_json(input.at("entityId").get<std::string>()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.save.capture",
        .description = "Capture live durable transforms and managed ScriptState as a versioned portable save document.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","worldRevision","simulationTick","sceneGuid","document","scriptState"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->save_capture_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.save.restore",
        .description = "Validate and restore a portable save envelope or legacy canonical Scene, including managed ScriptState when present.",
        .access = "write", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["document"],"properties":{"document":{"type":"object"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operation","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->save_restore_json(input.at("document").dump()); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.replay.start",
        .description = "Capture an initial save state and start fixed-tick logical input recording, replacing any previous recording.",
        .access = "control", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","recording"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->replay_start_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.replay.stop",
        .description = "Stop recording and return a portable initial state plus fixed-tick ordered logical input replay.",
        .access = "control", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","recording","sampleCount","samples"]})",
        .handler = [this](const std::string_view arguments) { static_cast<void>(parse_object(arguments)); return world_->replay_stop_json(); }
    });

    commands_.push_back(CommandDefinition{
        .name = "gameplay.replay.apply",
        .description = "Restore replay initial state and advance bounded fixed ticks through the normal input, simulation and script paths.",
        .access = "control", .idempotent = false, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["replay"],"properties":{"replay":{"type":"object"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","appliedSamples"]})",
        .handler = [this](const std::string_view arguments) { const auto input=parse_object(arguments); return world_->replay_apply_json(input.at("replay").dump()); }
    });

    commands_.push_back(CommandDefinition{
        .name="scripting.abi.describe",
        .description="Describe the stable C# host ABI, lifecycle callbacks, value boundary, isolation model and forbidden native handles.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","backend","language","targetFramework","valueBoundary","callbacks","status"]})",
        .handler=[this](const std::string_view arguments){static_cast<void>(parse_object(arguments));return world_->scripting_abi_json();}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.instances.observe",
        .description="Return bounded managed-script instance identities and lifecycle state without exposing CLR or native pointers.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","revision","backend","instances"]})",
        .handler=[this](const std::string_view arguments){static_cast<void>(parse_object(arguments));return world_->scripting_observation_json();}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.instance.attach",
        .description="Attach a versioned managed-script instance descriptor to a stable World entity.",
        .access="write",.idempotent=false,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["instanceId","entityId","assemblyAsset","typeName"],"properties":{"instanceId":{"type":"string"},"entityId":{"type":"string"},"assemblyAsset":{"type":"string"},"typeName":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","operation","revision"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);return world_->scripting_attach_json(
            input.at("instanceId").get<std::string>(),input.at("entityId").get<std::string>(),input.at("assemblyAsset").get<std::string>(),input.at("typeName").get<std::string>());}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.lifecycle.invoke",
        .description="Execute a managed lifecycle callback through the stable JSON value boundary and return bounded CoreCLR evidence.",
        .access="control",.idempotent=false,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["instanceId","callback"],"properties":{"instanceId":{"type":"string"},"callback":{"type":"string"},"arguments":{"type":"object","default":{}}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","operation","instanceId","callback","revision","executedManagedCode"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);return world_->scripting_invoke_json(
            input.at("instanceId").get<std::string>(),input.at("callback").get<std::string>(),input.at("arguments").dump());}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.project.observe",
        .description="Return the configured project C# source, compile revision, last diagnostics and output assembly without exposing process handles.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","configured","projectRoot","scriptProject","revision","lastCompile"]})",
        .handler=[this](const std::string_view arguments){static_cast<void>(parse_object(arguments));return world_->scripting_project_observation_json();}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.project.compile",
        .description="Incrementally compile the configured project C# assembly with the pinned SDK and return bounded, source-addressable diagnostics.",
        .access="control",.idempotent=false,.supports_dry_run=false,.runtime_state="attached",.task_kind="bounded-job",
        .input_schema_json=R"({"type":"object","properties":{"configuration":{"type":"string","enum":["Debug","Release"],"default":"Debug"}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","configuration","cacheHit","diagnostics"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);return world_->scripting_project_compile_json(input.value("configuration","Debug"));}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.debug.attach-manifest",
        .description="Return the current CoreCLR process, assembly, symbols and DAP attach request without launching external UI.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","ready","code","targetReady","adapterReady","processId","request"]})",
        .handler=[this](const std::string_view arguments){static_cast<void>(parse_object(arguments));return world_->scripting_debug_attach_json();}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.debug.session.start",
        .description="Start the hidden managed DAP adapter session without exposing adapter paths or transport handles.",
        .access="control",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","operation","session"]})",
        .handler=[this](const std::string_view arguments){static_cast<void>(parse_object(arguments));return world_->scripting_debug_session_start_json();}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.debug.session.status",
        .description="Observe the structured managed DAP session state without raw transport paths or process handles.",
        .access="read",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","operation","session"]})",
        .handler=[this](const std::string_view arguments){static_cast<void>(parse_object(arguments));return world_->scripting_debug_session_status_json();}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.debug.session.request",
        .description="Send one bounded allowlisted DAP request; initialization and launch sequencing remain explicit for adapter compatibility.",
        .access="control",.idempotent=false,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["command"],"properties":{"command":{"type":"string","enum":["initialize","launch","attach","configurationDone","setBreakpoints","continue","pause","next","stepIn","stepOut","threads","stackTrace","disconnect","terminate"]},"arguments":{"type":"object","default":{}},"timeoutMs":{"type":"integer","minimum":1,"maximum":30000,"default":5000}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","operation","command","body"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);return world_->scripting_debug_session_request_json(
            input.at("command").get<std::string>(),input.value("arguments",Json::object()).dump(),input.value("timeoutMs",5000U));}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.debug.session.events",
        .description="Drain ordered DAP events into a bounded, path-sanitized observation stream.",
        .access="read",.idempotent=false,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","eventCount","events"]})",
        .handler=[this](const std::string_view arguments){static_cast<void>(parse_object(arguments));return world_->scripting_debug_session_events_json();}
    });
    commands_.push_back(CommandDefinition{
        .name="scripting.debug.session.stop",
        .description="Disconnect without terminating an attached debuggee, then guarantee that the hidden adapter process is reaped.",
        .access="control",.idempotent=true,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","properties":{"timeoutMs":{"type":"integer","minimum":1,"maximum":30000,"default":2000}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","operation","alreadyStopped"]})",
        .handler=[this](const std::string_view arguments){const auto input=parse_object(arguments);return world_->scripting_debug_session_stop_json(input.value("timeoutMs",2000U));}
    });

    commands_.push_back(CommandDefinition{
        .name = "animation.skeleton.inspect",
        .description = "Return a bounded joint hierarchy and model-space debug pose for one animated entity.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string"},"maxJoints":{"type":"integer","minimum":1,"maximum":64,"default":32}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","entityId","valid","code","joints"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            if (!input.contains("entityId") || !input.at("entityId").is_string())
                throw std::invalid_argument("entityId must be a string");
            const auto max_joints = input.value("maxJoints", 32U);
            if (max_joints < 1U || max_joints > SkeletalPose::maximum_joints)
                throw std::invalid_argument("maxJoints must be between 1 and 64");
            return world_->animation_skeleton_json(input.at("entityId").get<std::string>(), max_joints);
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "editor.inspector.describe",
        .description = "Return the declarative, revision-bound Semantic UI property document for one entity.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","valid","code","entity","sections"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);
            return world_->inspector_document_json(input.at("entityId").get<std::string>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.observe",
        .description = "Return a bounded Semantic UI projection by stable node ID or role without requiring screenshots or renderer handles.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string"},"nodeIds":{"type":"array","default":[]},"roles":{"type":"array","default":[]},"depth":{"type":"integer","minimum":0,"maximum":8,"default":2},"byteBudget":{"type":"integer","minimum":512,"maximum":65536,"default":16384},"cursor":{"type":"integer","minimum":0,"default":0},"includeValues":{"type":"boolean","default":true},"locale":{"type":"string","default":"en-US"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","nodes"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            SemanticUiQuery query;
            query.node_ids = input.value("nodeIds", std::vector<std::string>{});
            query.roles = input.value("roles", std::vector<std::string>{});
            query.depth = input.value("depth", 2U);
            query.byte_budget = input.value("byteBudget", 16U * 1024U);
            query.cursor = input.value("cursor", 0U);
            query.include_values = input.value("includeValues", true);
            return world_->semantic_ui_observation_json(input.at("entityId").get<std::string>(), query,
                                                        input.value("locale", std::string("en-US")));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.delta",
        .description = "Return bounded, coalesced Semantic UI node changes since a known World revision; request focused resync when history is unavailable.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","sinceRevision"],"properties":{"entityId":{"type":"string"},"sinceRevision":{"type":"integer","minimum":0},"baseFingerprint":{"type":"string","default":""},"byteBudget":{"type":"integer","minimum":512,"maximum":65536,"default":8192},"cursor":{"type":"integer","minimum":0,"default":0},"includeValues":{"type":"boolean","default":true},"locale":{"type":"string","default":"en-US"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","document","resyncRequired","changes"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            SemanticUiDeltaQuery query;
            query.byte_budget = input.value("byteBudget", 8U * 1024U);
            query.cursor = input.value("cursor", 0U);
            query.include_values = input.value("includeValues", true);
            query.base_fingerprint = input.value("baseFingerprint", std::string{});
            return world_->semantic_ui_delta_json(
                input.at("entityId").get<std::string>(), input.at("sinceRevision").get<std::uint64_t>(), query,
                input.value("locale", std::string("en-US")));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.project.observe",
        .description = "Return the active project's runtime-bound HUD as a bounded Semantic UI projection, using the same document rendered for the player.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"nodeIds":{"type":"array","default":[]},"roles":{"type":"array","default":[]},"depth":{"type":"integer","minimum":0,"maximum":8,"default":2},"byteBudget":{"type":"integer","minimum":512,"maximum":65536,"default":16384},"cursor":{"type":"integer","minimum":0,"default":0},"includeValues":{"type":"boolean","default":true},"locale":{"type":"string","default":"en-US"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","nodes"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);SemanticUiQuery query;
            query.node_ids=input.value("nodeIds",std::vector<std::string>{});query.roles=input.value("roles",std::vector<std::string>{});
            query.depth=input.value("depth",2U);query.byte_budget=input.value("byteBudget",16U*1024U);
            query.cursor=input.value("cursor",0U);query.include_values=input.value("includeValues",true);
            return semantic_ui_query_json(world_->semantic_ui_project_document_json(input.value("locale",std::string("en-US"))),query);
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "editor.context.observe",
        .description = "Observe the bounded visible Editor project, scene, Edit/Play authority, selection, focus and recent transaction context.",
        .access = "read", .idempotent = true, .supports_dry_run = false,
        .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","authority","project","scene","selection","focus","transaction"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            if(editor_context_observe_)return editor_context_observe_();
            return Json{{"schemaVersion","noemancer.editor-context/0.1"},{"revision",0},
                {"valid",false},{"code","editor.context-detached"},
                {"detail","This command requires the live interactive Editor authority."},
                {"authority","unavailable"},{"project",nullptr},{"scene",nullptr},
                {"selection",nullptr},{"focus",nullptr},{"transaction",nullptr}}.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "editor.context.intent",
        .description = "Apply one revision-bound visible Editor selection or panel-focus intent through the existing Editor authority.",
        .access = "write", .idempotent = false, .supports_dry_run = true,
        .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["expectedRevision"],"properties":{"expectedRevision":{"type":"integer","minimum":1},"dryRun":{"type":"boolean","default":false},"selection":{"type":"object","properties":{"entityIds":{"type":"array","maxItems":128,"items":{"type":"string","minLength":1,"maxLength":128}},"primaryEntityId":{"type":"string","maxLength":128},"assetId":{"type":"string","maxLength":128}},"additionalProperties":false},"focus":{"type":"object","properties":{"panelId":{"type":"string","maxLength":128},"activeTabId":{"type":["string","null"],"maxLength":128}},"additionalProperties":false}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","detail","revisionBefore","revisionAfter","context"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            if(editor_context_apply_intent_)return editor_context_apply_intent_(arguments);
            return Json{{"schemaVersion","noemancer.editor-context-receipt/0.1"},
                {"success",false},{"code","editor.context-detached"},
                {"detail","This command requires the live interactive Editor authority."},
                {"revisionBefore",0},{"revisionAfter",0},{"context",nullptr}}.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.project.source.observe",
        .description = "Observe the project-attached canonical HUD source, reusable components, revision, diagnostics and authoring history without materializing runtime binding state.",
        .access = "read", .idempotent = true, .supports_dry_run = false,
        .runtime_state = "project-attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","revision","fingerprint","document","diagnostics"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            if(project_ui_authoring_)return project_ui_authoring_->observe_json();
            return Json{{"schemaVersion",project_ui_authoring_schema},{"valid",false},
                {"code","ui.project-session-detached"},{"revision",0},{"fingerprint",""},
                {"document",nullptr},{"diagnostics",Json::array({Json{{"severity","error"},
                    {"code","ui.project-session-detached"},{"path","/"},
                    {"message","Start the Agent session with --project to attach a Project UI source authority."}}})}}.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.project.source.edit",
        .description = "Apply one bounded revision-checked Project UI source edit through the project-attached transactional authority; supports nodes, reusable components, design tokens and undo/redo.",
        .access = "write", .idempotent = false, .supports_dry_run = true,
        .runtime_state = "project-attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["operation","baseRevision"],"properties":{"operation":{"type":"string","enum":["add","update","remove","reparent","reorder","design-tokens","add-reusable","update-reusable","remove-reusable","undo","redo"]},"baseRevision":{"type":"integer","minimum":1},"dryRun":{"type":"boolean","default":false},"id":{"type":"string","maxLength":128},"nodeId":{"type":"string","maxLength":128},"parentId":{"type":"string","maxLength":128},"role":{"type":"string","maxLength":128},"label":{"type":"string","maxLength":4096},"siblingIndex":{"type":"integer","minimum":0,"maximum":4096},"componentRef":{},"binding":{},"actions":{},"state":{},"presentation":{},"value":{},"designTokens":{},"declarationId":{"type":"string","maxLength":128},"declaration":{}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","changed","persisted","operation","code","detail","revision","fingerprint","document","observation","diagnostics"]})",
        .handler = [this](const std::string_view arguments) {
            if(!project_ui_authoring_)return Json{{"schemaVersion",project_ui_authoring_schema},
                {"success",false},{"changed",false},{"persisted",false},{"operation","command"},
                {"code","ui.project-session-detached"},{"detail","Start the Agent session with --project before authoring Project UI."},
                {"revision",0},{"fingerprint",""},{"candidateFingerprint",""},{"document",nullptr},
                {"observation",nullptr},{"canUndo",false},{"canRedo",false},{"diagnostics",Json::array()}}.dump();
            auto result=Json::parse(project_ui_authoring_->dispatch_json(arguments),nullptr,false);
            if(result.is_discarded()||!result.is_object())
                throw std::runtime_error("Project UI authoring returned an invalid receipt");
            const auto input=Json::parse(arguments,nullptr,false);
            const auto committed=result.value("success",false)&&result.value("changed",false)&&
                !input.value("dryRun",false);
            result["runtimeHotApplyAttempted"]=committed;
            result["runtimeHotApplied"]=committed&&
                world_->configure_project_hud(project_ui_authoring_->session().source_json());
            return result.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.project.action.invoke",
        .description = "Validate or invoke one action declared by the canonical project UI document through the same script callback authority used by Retained UI.",
        .access = "write", .idempotent = false, .supports_dry_run = true,
        .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["nodeId","actionId","baseRevision"],"properties":{"nodeId":{"type":"string","minLength":1,"maxLength":128},"actionId":{"type":"string","minLength":1,"maxLength":128},"baseRevision":{"type":"integer","minimum":0},"eventKind":{"type":"string","enum":["invoke","value-changed"],"default":"invoke"},"value":{},"dryRun":{"type":"boolean","default":false},"source":{"type":"string","minLength":1,"maxLength":96,"default":"agent.ui.project.action"},"sequence":{"type":"integer","minimum":0,"default":0}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","documentRevision","nodeId","actionId","eventKind","dryRun"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);
            if(!input.contains("nodeId")||!input.at("nodeId").is_string()||
               !input.contains("actionId")||!input.at("actionId").is_string()||
               !input.contains("baseRevision")||!input.at("baseRevision").is_number_unsigned())
                throw std::invalid_argument("nodeId, actionId and unsigned baseRevision are required");
            return world_->project_ui_action_invoke_json(
                input.at("nodeId").get<std::string>(),input.at("actionId").get<std::string>(),
                input.value("eventKind",std::string("invoke")),
                input.contains("value")?input.at("value").dump():std::string("null"),
                input.at("baseRevision").get<std::uint64_t>(),input.value("dryRun",false),
                input.value("source",std::string("agent.ui.project.action")),
                input.value("sequence",0ULL));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.retained.preview",
        .description = "Project an entity Inspector through RmlUi 6.2 and return retained DOM layout plus renderer-neutral draw evidence.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId"],"properties":{"entityId":{"type":"string"},"width":{"type":"integer","minimum":320,"maximum":4096,"default":960},"height":{"type":"integer","minimum":240,"maximum":4096,"default":720},"densityScale":{"type":"number","minimum":0.5,"maximum":4.0,"default":1.0},"locale":{"type":"string","default":"en-US"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return world_->retained_ui_preview_json(
                input.at("entityId").get<std::string>(), input.value("width", 960U), input.value("height", 720U),
                input.value("densityScale", 1.0F), input.value("locale", std::string("en-US")));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.resources.inspect",
        .description = "Inspect resource-backed Design Tokens and locale/fallback catalogs, including the hot-reload fingerprint.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "offline", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"locale":{"type":"string","default":"en-US"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","locale","themeLoaded","stylesheetLoaded","requestedMessagesLoaded","fallbackMessagesLoaded","resourceRevision","reloadModel"]})",
        .handler = [](const std::string_view arguments) { const auto input=parse_object(arguments);
            return semantic_ui_resource_status_json(input.value("locale",std::string("en-US"))); }
    });

    commands_.push_back(CommandDefinition{
        .name = "ui.text.inspect",
        .description = "Inspect platform fonts and IME readiness; optionally return a bounded HarfBuzz/ICU glyph-run, BiDi and line-break plan.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "offline", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"locale":{"type":"string","default":"en-US"},"text":{"type":"string","maxLength":65536,"default":""},"fontPath":{"type":"string","default":""},"fontSize":{"type":"number","minimum":4,"maximum":512,"default":16}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","locale","requiredScript","platformFallbackFaces","textInput","shaping","segmentation"]})",
        .handler = [](const std::string_view arguments) { const auto input=parse_object(arguments);
            return retained_ui_text_capabilities_json(input.value("locale",std::string("en-US")),
                input.value("text",std::string{}),input.value("fontPath",std::string{}),input.value("fontSize",16.0F)); }
    });

    commands_.push_back(CommandDefinition{
        .name = "physics.ray-cast",
        .description = "Cast a bounded world-space ray against the active Jolt scene and return a stable entity identity.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["origin","direction"],"properties":{"origin":{"type":"object"},"direction":{"type":"object"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","backend","origin","direction","hit","result"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            const auto origin = parse_vector(input.at("origin"), "origin");
            const auto direction = parse_vector(input.at("direction"), "direction");
            return world_->physics_ray_cast_json(
                {static_cast<float>(origin.x), static_cast<float>(origin.y), static_cast<float>(origin.z)},
                {static_cast<float>(direction.x), static_cast<float>(direction.y), static_cast<float>(direction.z)});
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "physics.sphere-sweep",
        .description = "Sweep a bounded world-space sphere against the active Jolt scene and return stable entity, contact and penetration evidence.",
        .access = "read", .idempotent = true, .supports_dry_run = false, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["origin","direction","radius"],"properties":{"origin":{"type":"object","required":["x","y","z"]},"direction":{"type":"object","required":["x","y","z"]},"radius":{"type":"number","minimum":0.01,"maximum":10.0},"ignoredEntityId":{"type":"string","default":""}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","backend","shape","origin","direction","hit","result"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);
            return world_->physics_sphere_sweep_json(parse_vector3(input.at("origin")),parse_vector3(input.at("direction")),
                input.at("radius").get<float>(),input.value("ignoredEntityId",std::string{}));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.transform.plan",
        .description = "Create an immutable revision-bound Transform change plan without mutating World state.",
        .access = "write",
        .idempotent = true,
        .supports_dry_run = true,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","baseRevision","position"],"properties":{"entityId":{"type":"string"},"baseRevision":{"type":"integer","minimum":0},"manager":{"type":"string","default":"agent.default"},"position":{"type":"object"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","planId","contentHash","baseRevision","predictedDelta"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            const auto position = parse_vector(input.at("position"), "position");
            const auto plan = world_->plan_transform_update(
                input.at("entityId").get<std::string>(),
                {static_cast<float>(position.x), static_cast<float>(position.y), static_cast<float>(position.z)},
                input.at("baseRevision").get<std::uint64_t>(),
                input.at("manager").get<std::string>());
            return World::change_plan_json(plan);
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.property.plan",
        .description = "Create an immutable revision-bound component property change plan from the shared Inspector Schema.",
        .access = "write", .idempotent = true, .supports_dry_run = true, .runtime_state = "attached", .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","property","value","baseRevision"],"properties":{"entityId":{"type":"string"},"property":{"type":"string"},"value":{},"baseRevision":{"type":"integer","minimum":0},"manager":{"type":"string","default":"agent.default"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","planId","contentHash","baseRevision","predictedDelta"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);
            return World::property_change_plan_json(world_->plan_property_update(input.at("entityId").get<std::string>(),
                input.at("property").get<std::string>(),input.at("value").dump(),input.at("baseRevision").get<std::uint64_t>(),
                input.at("manager").get<std::string>()));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.change.apply",
        .description = "Dry-run or atomically apply an immutable change plan after integrity and revision checks.",
        .access = "write",
        .idempotent = false,
        .supports_dry_run = true,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["plan"],"properties":{"plan":{"type":"object"},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","dryRun","code","operationId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            const auto& plan=input.at("plan");
            const auto receipt = plan.value("operation", "transform.set") == "property.set"
                ? world_->apply_property_plan(parse_property_plan(plan), input.at("dryRun").get<bool>())
                : world_->apply_transform_plan(parse_transform_plan(plan), input.at("dryRun").get<bool>());
            return World::action_receipt_json(receipt);
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.undo",
        .description = "Undo the last compatible committed World change in the current engine session.",
        .access = "write",
        .idempotent = false,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["expectedRevision"],"properties":{"expectedRevision":{"type":"integer","minimum":0},"manager":{"type":"string","default":"agent.default"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operationId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return World::action_receipt_json(world_->undo(
                input.at("expectedRevision").get<std::uint64_t>(),
                input.at("manager").get<std::string>()));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "world.redo",
        .description = "Reapply the last reverted World change in the current engine session.",
        .access = "write",
        .idempotent = false,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["expectedRevision"],"properties":{"expectedRevision":{"type":"integer","minimum":0},"manager":{"type":"string","default":"agent.default"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","operationId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return World::action_receipt_json(world_->redo(
                input.at("expectedRevision").get<std::uint64_t>(),
                input.at("manager").get<std::string>()));
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "scene.entity.edit",
        .description = "Dry-run or commit a revision-bound entity or component edit against the canonical scene document.",
        .access = "write",
        .idempotent = false,
        .supports_dry_run = true,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["operation","baseRevision"],"properties":{"operation":{"type":"string","enum":["create","duplicate","rename","reparent","delete","add-component","remove-component"]},"entityId":{"type":"string","default":""},"newEntityId":{"type":"string","default":""},"displayName":{"type":"string","default":""},"parentEntityId":{"type":"string","default":""},"component":{"type":"string","default":""},"recursive":{"type":"boolean","default":false},"baseRevision":{"type":"integer","minimum":0},"manager":{"type":"string","default":"agent.default"},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","dryRun","code","operation","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return world_->edit_scene_entity_json(input.at("operation").get<std::string>(),
                input.at("entityId").get<std::string>(),input.at("newEntityId").get<std::string>(),
                input.at("displayName").get<std::string>(),input.at("parentEntityId").get<std::string>(),
                input.at("component").get<std::string>(),
                input.at("recursive").get<bool>(),input.at("baseRevision").get<std::uint64_t>(),
                input.at("manager").get<std::string>(),input.at("dryRun").get<bool>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "scene.export",
        .description = "Return the canonical Git-friendly scene document for the current World session.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schema","sceneGuid","name","entities"]})",
        .handler = [this](const std::string_view arguments) {
            static_cast<void>(parse_object(arguments));
            return world_->canonical_scene_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "scene.transform.edit",
        .description = "Dry-run or atomically commit position, normalized quaternion rotation and scale through the canonical scene transaction path.",
        .access = "write",
        .idempotent = false,
        .supports_dry_run = true,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["entityId","position","rotationQuaternion","scale","baseRevision"],"properties":{"entityId":{"type":"string"},"position":{"type":"object","required":["x","y","z"],"properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false},"rotationQuaternion":{"type":"object","required":["x","y","z","w"],"properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"w":{"type":"number"}},"additionalProperties":false},"scale":{"type":"object","required":["x","y","z"],"properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false},"baseRevision":{"type":"integer","minimum":0},"manager":{"type":"string","default":"agent.default"},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","dryRun","code","entityId","revisionBefore","revisionAfter"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);const auto& p=input.at("position");const auto& q=input.at("rotationQuaternion");const auto& s=input.at("scale");
            return world_->edit_transform_json(input.at("entityId").get<std::string>(),Transform{
                p.at("x").get<float>(),p.at("y").get<float>(),p.at("z").get<float>(),
                s.at("x").get<float>(),s.at("y").get<float>(),s.at("z").get<float>(),
                q.at("x").get<float>(),q.at("y").get<float>(),q.at("z").get<float>(),q.at("w").get<float>()},
                input.at("baseRevision").get<std::uint64_t>(),input.at("manager").get<std::string>(),input.at("dryRun").get<bool>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "scene.save",
        .description = "Persist the current canonical scene document back to its absolute project source path.",
        .access = "write",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"source":{"type":"string","default":""},"overwrite":{"type":"boolean","default":false}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","source","revision"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);
            const auto source=input.at("source").get<std::string>();
            return source.empty()?world_->save_scene_to_source_json():world_->save_scene_as_source_json(source,input.at("overwrite").get<bool>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "scene.open",
        .description = "Open and validate an absolute canonical scene source, replacing the current edit World.",
        .access = "write",
        .idempotent = false,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["source"],"properties":{"source":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","code","source","revision"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input=parse_object(arguments);
            return world_->open_scene_from_source_json(input.at("source").get<std::string>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "asset.registry",
        .description = "Return the deterministic project asset registry with identity, hash, license and import status.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"refresh":{"type":"boolean","default":false}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","assetCount","errorCount","assets"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            if (input.at("refresh").get<bool>()) static_cast<void>(assets_->refresh());
            return assets_->registry_json();
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "asset.query",
        .description = "Query focused asset metadata by text, kind, import state and semantic tags.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"text":{"type":"string","default":""},"kind":{"type":"string","default":""},"importState":{"type":"string","default":""},"tags":{"type":"array","default":[]},"cursor":{"type":"integer","minimum":0,"default":0},"limit":{"type":"integer","minimum":1,"maximum":256,"default":64}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","revision","total","cursor","assets"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            AssetQuery query{
                .text = input.at("text").get<std::string>(),
                .kind = input.at("kind").get<std::string>(),
                .import_state = input.at("importState").get<std::string>(),
                .cursor = input.at("cursor").get<std::size_t>(),
                .limit = input.at("limit").get<std::size_t>()
            };
            for (const auto& tag : input.at("tags")) query.tags.push_back(tag.get<std::string>());
            return assets_->query_json(query);
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "asset.cook.plan",
        .description = "Create a deterministic dry-run Cook manifest with source hashes, importers and cache addresses.",
        .access = "write",
        .idempotent = true,
        .supports_dry_run = true,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["assetIds"],"properties":{"assetIds":{"type":"array"},"targetProfile":{"type":"string","default":"windows-x64-debug"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","planId","contentHash","targetProfile","registryRevision","inputs","errors"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            std::vector<std::string> ids;
            for (const auto& id : input.at("assetIds")) ids.push_back(id.get<std::string>());
            return assets_->cook_plan_json(ids, input.at("targetProfile").get<std::string>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "asset.inspect",
        .description = "Inspect imported scene metadata and verify normalized geometry, PBR materials, skins, animation and bounds.",
        .access = "read",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["assetId"],"properties":{"assetId":{"type":"string"}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","valid","code","asset","importedMetadata"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return assets_->inspect_json(input.at("assetId").get<std::string>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name="asset.tile-palette.autotile",
        .description="Preview or atomically update one Tile's semantic autotile group and exact four-neighbor frame variants.",
        .access="write",.idempotent=true,.supports_dry_run=true,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["assetId","tileId","autotileGroup","variants","manager"],"properties":{"assetId":{"type":"string"},"tileId":{"type":"string"},"autotileGroup":{"type":"string"},"variants":{"type":"array"},"manager":{"type":"string"},"expectedFingerprint":{"type":"string","default":""},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","dryRun","assetId","source","plan","receipt","sourceReceipt"]})",
        .handler=[this](const std::string_view arguments) {
            const auto input=parse_object(arguments);const auto asset_id=input.at("assetId").get<std::string>();const auto* asset=assets_->find(asset_id);
            const auto failure=[&](const std::string& code,const std::string& detail) {return Json{{"schemaVersion","noemancer.tile-palette-autotile-operation/0.1"},
                {"success",false},{"code",code},{"detail",detail},{"dryRun",input.at("dryRun")},{"assetId",asset_id},{"source",nullptr},
                {"plan",nullptr},{"receipt",nullptr},{"sourceReceipt",nullptr}}.dump();};
            if(asset==nullptr||!asset->available||!asset->relative_path.ends_with(".tile-palette.json"))
                return failure("tilemap.palette-unavailable","Asset Registry has no available Tile Palette with this ID.");
            const auto source_path=assets_->source_path(*asset);std::ifstream stream(source_path,std::ios::binary);
            const std::string source{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};stream.close();
            const auto parsed=TilemapAssetCodec::parse_palette_json(source);if(!parsed)return failure("tilemap.palette-invalid","Tile Palette failed strict parsing.");
            std::vector<TileAutotileVariant> variants;variants.reserve(input.at("variants").size());
            for(const auto& value:input.at("variants")) {
                if(!value.is_object()||!value.contains("mask")||!value.contains("frame"))throw std::invalid_argument("Each variant requires mask and frame");
                variants.push_back({value.at("mask").get<std::uint8_t>(),value.at("frame").get<std::string>()});
            }
            auto document=*parsed.document;const auto manager=input.at("manager").get<std::string>();
            const auto plan=TilemapAssetCodec::plan_autotile_update(document,input.at("tileId").get<std::string>(),
                input.at("autotileGroup").get<std::string>(),std::move(variants),manager,input.at("expectedFingerprint").get<std::string>());
            const auto dry_run=input.at("dryRun").get<bool>();const auto receipt=TilemapAssetCodec::apply_palette_edit(document,plan,dry_run);
            std::optional<AssetSourceEditReceipt> source_receipt;if(receipt.success&&!dry_run) {
                source_receipt=assets_->commit_text_source(asset_id,TilemapAssetCodec::write_palette_canonical_json(document)+"\n",manager);
                if(!source_receipt->success)return failure(source_receipt->code,source_receipt->detail);
                static_cast<void>(world_->register_tile_palette(document));
            }
            return Json{{"schemaVersion","noemancer.tile-palette-autotile-operation/0.1"},{"success",receipt.success},{"code",receipt.code},
                {"dryRun",dry_run},{"assetId",asset_id},{"source",source_path.generic_string()},
                {"plan",Json::parse(TilemapAssetCodec::palette_edit_plan_json(plan))},{"receipt",Json::parse(TilemapAssetCodec::stroke_apply_json(receipt))},
                {"sourceReceipt",source_receipt?asset_source_receipt_json(*source_receipt):Json(nullptr)}}.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name="asset.tilemap.stroke",
        .description="Preview or atomically commit a bounded multi-cell paint/erase stroke using world-cell coordinates, stable Tile IDs and an optimistic source fingerprint.",
        .access="write",.idempotent=true,.supports_dry_run=true,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["assetId","layerId","edits","manager"],"properties":{"assetId":{"type":"string"},"layerId":{"type":"string"},"edits":{"type":"array"},"manager":{"type":"string"},"expectedFingerprint":{"type":"string","default":""},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","dryRun","assetId","source","plan","receipt"]})",
        .handler=[this](const std::string_view arguments) {
            const auto input=parse_object(arguments);const auto asset_id=input.at("assetId").get<std::string>();
            const auto* asset=assets_->find(asset_id);
            const auto failure=[&](const std::string& code,const std::string& detail) {
                return Json{{"schemaVersion","noemancer.tilemap-stroke-operation/0.1"},{"success",false},{"code",code},
                    {"detail",detail},{"dryRun",input.at("dryRun")},{"assetId",asset_id},{"source",nullptr},{"plan",nullptr},{"receipt",nullptr}}.dump();
            };
            if(asset==nullptr||!asset->available||!asset->relative_path.ends_with(".tilemap.json"))
                return failure("tilemap.asset-unavailable","Asset Registry has no available Tilemap with this ID.");
            const auto source_path=assets_->source_path(*asset);std::ifstream map_stream(source_path,std::ios::binary);
            const std::string map_source{std::istreambuf_iterator<char>(map_stream),std::istreambuf_iterator<char>()};
            map_stream.close();
            const auto parsed_map=TilemapAssetCodec::parse_tilemap_json(map_source);
            if(!parsed_map)return failure("tilemap.source-invalid","Tilemap source failed strict parsing.");
            const auto* palette_asset=assets_->find(parsed_map.document->palette_asset);
            if(palette_asset==nullptr||!palette_asset->available)return failure("tilemap.palette-unavailable","Referenced Tile Palette is unavailable.");
            std::ifstream palette_stream(assets_->source_path(*palette_asset),std::ios::binary);
            const std::string palette_source{std::istreambuf_iterator<char>(palette_stream),std::istreambuf_iterator<char>()};
            palette_stream.close();
            const auto parsed_palette=TilemapAssetCodec::parse_palette_json(palette_source);
            if(!parsed_palette)return failure("tilemap.palette-invalid","Referenced Tile Palette failed strict parsing.");
            std::vector<TilemapCellEdit> edits;edits.reserve(input.at("edits").size());
            for(const auto& value:input.at("edits")) {
                if(!value.is_object()||!value.contains("x")||!value.contains("y")||!value.contains("operation"))
                    throw std::invalid_argument("Each edit requires x, y and operation");
                const auto operation=value.at("operation").get<std::string>();
                if(operation!="paint"&&operation!="erase")throw std::invalid_argument("edit operation must be paint or erase");
                std::optional<std::string> tile_id;
                if(operation=="paint") {
                    if(!value.contains("tileId")||!value.at("tileId").is_string())throw std::invalid_argument("paint edit requires tileId");
                    tile_id=value.at("tileId").get<std::string>();
                }
                edits.push_back({value.at("x").get<std::int32_t>(),value.at("y").get<std::int32_t>(),std::move(tile_id),
                    value.value("flipX",false),value.value("flipY",false)});
            }
            auto document=*parsed_map.document;
            const auto plan=TilemapAssetCodec::plan_stroke(*parsed_palette.document,document,input.at("layerId").get<std::string>(),
                std::move(edits),input.at("manager").get<std::string>(),input.at("expectedFingerprint").get<std::string>());
            const auto dry_run=input.at("dryRun").get<bool>();const auto receipt=TilemapAssetCodec::apply_stroke(document,plan,dry_run);
            std::optional<AssetSourceEditReceipt> source_receipt;
            if(receipt.success&&!dry_run) {
                const auto canonical=TilemapAssetCodec::write_tilemap_canonical_json(document)+"\n";
                source_receipt=assets_->commit_text_source(asset_id,canonical,input.at("manager").get<std::string>());
                if(!source_receipt->success)return failure(source_receipt->code,source_receipt->detail);
                static_cast<void>(world_->register_tile_palette(*parsed_palette.document));static_cast<void>(world_->register_tilemap_asset(document));
            }
            return Json{{"schemaVersion","noemancer.tilemap-stroke-operation/0.1"},{"success",receipt.success},{"code",receipt.code},
                {"dryRun",dry_run},{"assetId",asset_id},{"source",source_path.generic_string()},
                {"plan",Json::parse(TilemapAssetCodec::stroke_plan_json(plan))},{"receipt",Json::parse(TilemapAssetCodec::stroke_apply_json(receipt))},
                {"sourceReceipt",source_receipt?asset_source_receipt_json(*source_receipt):Json(nullptr)}}.dump();
        }
    });

    commands_.push_back(CommandDefinition{
        .name="asset.tilemap.region",
        .description="Preview or atomically commit a rectangle or bounded occupied-cell flood fill without forcing the caller to enumerate cells; edit detail is progressively disclosed.",
        .access="write",.idempotent=true,.supports_dry_run=true,.runtime_state="attached",.task_kind="immediate",
        .input_schema_json=R"({"type":"object","required":["assetId","layerId","shape","first","operation","manager"],"properties":{"assetId":{"type":"string"},"layerId":{"type":"string"},"shape":{"type":"string"},"first":{"type":"object"},"second":{"type":"object"},"operation":{"type":"string"},"tileId":{"type":"string","default":""},"flipX":{"type":"boolean","default":false},"flipY":{"type":"boolean","default":false},"manager":{"type":"string"},"expectedFingerprint":{"type":"string","default":""},"includeEdits":{"type":"boolean","default":false},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","dryRun","shape","assetId","source","plan","receipt","sourceReceipt"]})",
        .handler=[this](const std::string_view arguments) {
            const auto input=parse_object(arguments);const auto asset_id=input.at("assetId").get<std::string>();const auto shape=input.at("shape").get<std::string>();
            const auto failure=[&](const std::string& code,const std::string& detail) {return Json{{"schemaVersion","noemancer.tilemap-region-operation/0.1"},
                {"success",false},{"code",code},{"detail",detail},{"dryRun",input.at("dryRun")},{"shape",shape},{"assetId",asset_id},
                {"source",nullptr},{"plan",nullptr},{"receipt",nullptr},{"sourceReceipt",nullptr}}.dump();};
            if(shape!="rectangle"&&shape!="flood")return failure("tilemap.region-shape","shape must be rectangle or flood.");
            const auto operation=input.at("operation").get<std::string>();if(operation!="paint"&&operation!="erase")
                return failure("tilemap.region-operation","operation must be paint or erase.");
            std::optional<std::string> tile_id;if(operation=="paint") {const auto id=input.at("tileId").get<std::string>();
                if(id.empty())return failure("tilemap.region-tile-required","paint requires a non-empty tileId.");tile_id=id;}
            const auto coordinate=[&](const Json& value,const std::string_view name) {
                if(!value.contains("x")||!value.contains("y")||!value.at("x").is_number_integer()||!value.at("y").is_number_integer())
                    throw std::invalid_argument(std::string(name)+" requires integer x and y");
                return std::pair{value.at("x").get<std::int32_t>(),value.at("y").get<std::int32_t>()};};
            const auto first=coordinate(input.at("first"),"first");if(shape=="rectangle"&&!input.contains("second"))
                return failure("tilemap.region-second-required","rectangle requires second.");
            const auto second=shape=="rectangle"?coordinate(input.at("second"),"second"):first;
            const auto* asset=assets_->find(asset_id);if(asset==nullptr||!asset->available||!asset->relative_path.ends_with(".tilemap.json"))
                return failure("tilemap.asset-unavailable","Asset Registry has no available Tilemap with this ID.");
            const auto source_path=assets_->source_path(*asset);std::ifstream map_stream(source_path,std::ios::binary);
            const std::string map_source{std::istreambuf_iterator<char>(map_stream),std::istreambuf_iterator<char>()};map_stream.close();
            const auto parsed_map=TilemapAssetCodec::parse_tilemap_json(map_source);if(!parsed_map)return failure("tilemap.source-invalid","Tilemap source failed strict parsing.");
            const auto* palette_asset=assets_->find(parsed_map.document->palette_asset);if(palette_asset==nullptr||!palette_asset->available)
                return failure("tilemap.palette-unavailable","Referenced Tile Palette is unavailable.");
            std::ifstream palette_stream(assets_->source_path(*palette_asset),std::ios::binary);
            const std::string palette_source{std::istreambuf_iterator<char>(palette_stream),std::istreambuf_iterator<char>()};palette_stream.close();
            const auto parsed_palette=TilemapAssetCodec::parse_palette_json(palette_source);if(!parsed_palette)
                return failure("tilemap.palette-invalid","Referenced Tile Palette failed strict parsing.");
            auto document=*parsed_map.document;const auto layer_id=input.at("layerId").get<std::string>();const auto manager=input.at("manager").get<std::string>();
            const auto expected=input.at("expectedFingerprint").get<std::string>();const auto flip_x=input.at("flipX").get<bool>(),flip_y=input.at("flipY").get<bool>();
            const auto plan=shape=="rectangle"?TilemapAssetCodec::plan_rectangle(*parsed_palette.document,document,layer_id,first.first,first.second,
                second.first,second.second,tile_id,flip_x,flip_y,manager,expected):
                TilemapAssetCodec::plan_flood_fill(*parsed_palette.document,document,layer_id,first.first,first.second,tile_id,flip_x,flip_y,manager,expected);
            const auto dry_run=input.at("dryRun").get<bool>();const auto receipt=TilemapAssetCodec::apply_stroke(document,plan,dry_run);
            std::optional<AssetSourceEditReceipt> source_receipt;if(receipt.success&&!dry_run) {const auto canonical=TilemapAssetCodec::write_tilemap_canonical_json(document)+"\n";
                source_receipt=assets_->commit_text_source(asset_id,canonical,manager);if(!source_receipt->success)return failure(source_receipt->code,source_receipt->detail);
                static_cast<void>(world_->register_tile_palette(*parsed_palette.document));static_cast<void>(world_->register_tilemap_asset(document));}
            auto plan_json=Json::parse(TilemapAssetCodec::stroke_plan_json(plan));const auto edit_count=plan_json.at("edits").size();
            if(!input.at("includeEdits").get<bool>()&&edit_count>64U) {Json preview=Json::array();for(std::size_t index=0;index<64;++index)preview.push_back(plan_json.at("edits").at(index));plan_json["edits"]=std::move(preview);}
            plan_json["editCount"]=edit_count;plan_json["editsTruncated"]=plan_json.at("edits").size()<edit_count;
            return Json{{"schemaVersion","noemancer.tilemap-region-operation/0.1"},{"success",receipt.success},{"code",receipt.code},{"dryRun",dry_run},
                {"shape",shape},{"assetId",asset_id},{"source",source_path.generic_string()},{"plan",std::move(plan_json)},
                {"receipt",Json::parse(TilemapAssetCodec::stroke_apply_json(receipt))},{"sourceReceipt",source_receipt?asset_source_receipt_json(*source_receipt):Json(nullptr)}}.dump();
        }
    });

    const auto register_tilemap_history_command=[this](const std::string& name,const bool undo) {
        commands_.push_back(CommandDefinition{
            .name=name,.description=undo?"Undo the latest reversible text-asset edit after verifying the source was not externally changed."
                :"Redo the latest reversible text-asset edit after verifying the source was not externally changed.",
            .access="write",.idempotent=false,.supports_dry_run=false,.runtime_state="attached",.task_kind="immediate",
            .input_schema_json=R"({"type":"object","properties":{"manager":{"type":"string","default":"agent.asset-history"}},"additionalProperties":false})",
            .output_schema_json=R"({"type":"object","required":["schemaVersion","success","code","detail","assetId","source","manager","registryRevision","transactionId","runtimeReloaded"]})",
            .handler=[this,undo](const std::string_view arguments) {
                const auto input=validate_object(arguments,R"({"type":"object","properties":{"manager":{"type":"string","default":"agent.asset-history"}},"additionalProperties":false})");
                const auto receipt=undo?assets_->undo_text_source(input.at("manager").get<std::string>()):
                    assets_->redo_text_source(input.at("manager").get<std::string>());
                auto result=asset_source_receipt_json(receipt);result["schemaVersion"]="noemancer.asset-source-history/0.1";
                result["runtimeReloaded"]=receipt.success&&!receipt.asset_id.empty()?reload_authorable_asset_runtime(*world_,*assets_,receipt.asset_id):false;
                return result.dump();
            }
        });
    };
    register_tilemap_history_command("asset.source.undo",true);
    register_tilemap_history_command("asset.source.redo",false);

    commands_.push_back(CommandDefinition{
        .name = "asset.cook.apply",
        .description = "Validate or apply an immutable Cook plan into the content-addressed generated cache.",
        .access = "write",
        .idempotent = true,
        .supports_dry_run = true,
        .runtime_state = "attached",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","required":["plan"],"properties":{"plan":{"type":"object"},"dryRun":{"type":"boolean","default":true}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["schemaVersion","success","dryRun","code","operationId","planId","registryRevision","cacheHits","cacheMisses","artifacts","errors"]})",
        .handler = [this](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            return assets_->apply_cook_plan_json(
                input.at("plan").dump(),
                input.at("dryRun").get<bool>());
        }
    });

    commands_.push_back(CommandDefinition{
        .name = "run.headless",
        .description = "Run a bounded deterministic headless simulation.",
        .access = "control",
        .idempotent = true,
        .supports_dry_run = false,
        .runtime_state = "offline",
        .task_kind = "immediate",
        .input_schema_json = R"({"type":"object","properties":{"frames":{"type":"integer","minimum":1,"maximum":600,"default":3}},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["frames","entities","fixedDeltaSeconds"]})",
        .handler = [](const std::string_view arguments) {
            const auto input = parse_object(arguments);
            const auto frames = input.at("frames").get<std::uint32_t>();

            EngineHost host;
            host.register_default_modules();
            if (!host.initialize(true)) {
                throw std::runtime_error(std::string(host.last_error()));
            }
            World world;
            static_cast<void>(world.load_scene(make_bootstrap_scene_document()));
            constexpr double fixed_delta_seconds = 1.0 / 60.0;
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                host.run_frame(fixed_delta_seconds, [&world] {
                    world.tick(static_cast<float>(fixed_delta_seconds));
                });
            }

            const Json result = {
                {"frames", frames},
                {"entities", world.entity_count()},
                {"fixedDeltaSeconds", fixed_delta_seconds},
                {"engineFrame", host.frame_index()},
                {"moduleCount", host.statuses().size()}
            };
            return result.dump();
        }
    });
}

std::string CommandRegistry::manifest_json() const {
    Json tools = Json::array();
    for (const auto& command : commands_) {
        tools.push_back({
            {"name", command.name},
            {"description", command.description},
            {"access", command.access},
            {"idempotent", command.idempotent},
            {"supportsDryRun", command.supports_dry_run},
            {"runtimeState", command.runtime_state},
            {"taskKind", command.task_kind},
            {"inputSchema", parse_schema(command.input_schema_json)},
            {"outputSchema", parse_schema(command.output_schema_json)}
        });
    }

    const Json manifest = {
        {"protocolVersion", "0.2"},
        {"tools", std::move(tools)}
    };
    return manifest.dump();
}

CommandInvocation CommandRegistry::invoke(
    const std::string_view name,
    const std::string_view arguments_json) const {
    for (const auto& command : commands_) {
        if (command.name != name) {
            continue;
        }
        try {
            const auto validated_arguments = validate_object(
                arguments_json,
                command.input_schema_json);
            const auto result = command.handler(validated_arguments.dump());
            const auto validated_result = validate_object(
                result,
                command.output_schema_json);
            if (validated_result.contains("success") &&
                validated_result.at("success").is_boolean() &&
                !validated_result.at("success").get<bool>()) {
                return CommandInvocation{
                    .exit_code = 6,
                    .output_json = make_action_failure_envelope(
                        command.name,
                        command.access,
                        validated_result.dump())
                };
            }
            return CommandInvocation{
                .exit_code = 0,
                .output_json = make_success_envelope(
                    command.name,
                    command.access,
                    validated_result.dump())
            };
        } catch (const nlohmann::json::exception& error) {
            return CommandInvocation{
                .exit_code = 3,
                .output_json = make_error_envelope(command.name, "invalid_json", error.what())
            };
        } catch (const std::invalid_argument& error) {
            return CommandInvocation{
                .exit_code = 4,
                .output_json = make_error_envelope(command.name, "invalid_arguments", error.what())
            };
        } catch (const std::exception& error) {
            return CommandInvocation{
                .exit_code = 10,
                .output_json = make_error_envelope(command.name, "internal_error", error.what())
            };
        }
    }

    return CommandInvocation{
        .exit_code = 5,
        .output_json = make_error_envelope(name, "unknown_tool", "No tool with that name is registered")
    };
}

} // namespace noemancer
