#include "engine/network_replication.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

Json vector_json(const NetworkVector3& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

NetworkVector3 parse_vector(const Json& value) {
    if (!value.is_object()) return {};
    return {value.value("x", 0.0), value.value("y", 0.0), value.value("z", 0.0)};
}

Json quaternion_json(const NetworkQuaternion& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}, {"w", value.w}};
}

NetworkQuaternion parse_quaternion(const Json& value) {
    if (!value.is_object()) return {};
    return {value.value("x", 0.0), value.value("y", 0.0), value.value("z", 0.0), value.value("w", 1.0)};
}

Json replica_json(const NetworkEntityReplica& replica) {
    return {{"netEntityId", replica.net_entity_id}, {"entityId", replica.entity_id},
        {"authority", {{"mode", "server"}, {"peerId", replica.authority_peer_id}}},
        {"sourceRevision", replica.source_revision}, {"transform", {{"position", vector_json(replica.position)},
            {"rotationQuaternion", quaternion_json(replica.rotation)}, {"scale", vector_json(replica.scale)}}},
        {"velocity", {{"linear", vector_json(replica.linear_velocity)}}}};
}

NetworkEntityReplica parse_replica(const Json& value) {
    NetworkEntityReplica replica;
    replica.net_entity_id = value.at("netEntityId").get<std::string>();
    replica.entity_id = value.at("entityId").get<std::string>();
    replica.authority_peer_id = value.at("authority").at("peerId").get<std::string>();
    replica.source_revision = value.value("sourceRevision", 0ULL);
    replica.position = parse_vector(value.value("transform", Json::object()).value("position", Json::object()));
    replica.rotation = parse_quaternion(value.value("transform", Json::object()).value("rotationQuaternion", Json::object()));
    replica.scale = parse_vector(value.value("transform", Json::object()).value("scale", Json{{"x",1.0},{"y",1.0},{"z",1.0}}));
    replica.linear_velocity = parse_vector(value.value("velocity", Json::object()).value("linear", Json::object()));
    return replica;
}

std::unordered_map<std::string, Json> entities_by_net_id(const Json& document) {
    std::unordered_map<std::string, Json> result;
    for (const auto& entity : document.at("entities")) result.emplace(entity.at("netEntityId").get<std::string>(), entity);
    return result;
}

std::string state_digest(const std::unordered_map<std::string, NetworkEntityReplica>& replicas) {
    std::vector<std::string> ids;
    ids.reserve(replicas.size());
    for (const auto& [id, unused] : replicas) { static_cast<void>(unused); ids.push_back(id); }
    std::ranges::sort(ids);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& id : ids) {
        const auto canonical = replica_json(replicas.at(id)).dump();
        for (const unsigned char character : canonical) { hash ^= character; hash *= 1099511628211ULL; }
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

Json receipt(const bool success, const std::string_view code, const std::uint64_t tick,
             const std::uint64_t revision_before, const std::uint64_t revision_after) {
    return {{"schemaVersion", "noemancer.network-receipt/0.1"}, {"success", success}, {"code", code},
        {"tick", tick}, {"revisionBefore", revision_before}, {"revisionAfter", revision_after}};
}

} // namespace

NetworkReplicationRuntime::NetworkReplicationRuntime(std::string local_peer_id, const bool authoritative_server)
    : local_peer_id_(std::move(local_peer_id)), authoritative_server_(authoritative_server) {
    if (local_peer_id_.empty()) local_peer_id_ = authoritative_server_ ? "server.local" : "client.local";
}

std::string NetworkReplicationRuntime::stable_net_entity_id(const std::string_view entity_id) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : entity_id) { hash ^= character; hash *= 1099511628211ULL; }
    std::ostringstream output;
    output << "net.entity." << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string NetworkReplicationRuntime::capture_snapshot_json(const std::uint64_t tick,
                                                             const std::uint64_t world_revision,
                                                             const std::vector<WorldEntityView>& entities) const {
    std::vector<Json> replicated;
    for (const auto& entity : entities) {
        if (!entity.transform) continue;
        NetworkEntityReplica replica;
        replica.net_entity_id = stable_net_entity_id(entity.id);
        replica.entity_id = entity.id;
        replica.authority_peer_id = local_peer_id_;
        replica.source_revision = entity.revision;
        replica.position = {entity.transform->x, entity.transform->y, entity.transform->z};
        replica.rotation = {entity.transform->rotation_x, entity.transform->rotation_y,
            entity.transform->rotation_z, entity.transform->rotation_w};
        replica.scale = {entity.transform->scale_x, entity.transform->scale_y, entity.transform->scale_z};
        if (entity.velocity) replica.linear_velocity = {entity.velocity->x, entity.velocity->y, entity.velocity->z};
        replicated.push_back(replica_json(replica));
    }
    std::ranges::sort(replicated, {}, [](const Json& value) { return value.at("netEntityId").get<std::string>(); });
    return Json{{"schemaVersion", "noemancer.network-snapshot/0.1"}, {"profile", "optional-authoritative-server"},
        {"serverPeerId", local_peer_id_}, {"tick", tick}, {"worldRevision", world_revision},
        {"entityCount", replicated.size()}, {"entities", Json(std::move(replicated))}}.dump();
}

std::string NetworkReplicationRuntime::delta_json(const std::string_view baseline_snapshot_json,
                                                  const std::string_view target_snapshot_json) {
    try {
        const auto baseline = Json::parse(baseline_snapshot_json);
        const auto target = Json::parse(target_snapshot_json);
        if (baseline.value("schemaVersion", std::string{}) != "noemancer.network-snapshot/0.1" ||
            target.value("schemaVersion", std::string{}) != "noemancer.network-snapshot/0.1") throw std::invalid_argument("unsupported snapshot schema");
        const auto before = entities_by_net_id(baseline);
        const auto after = entities_by_net_id(target);
        std::vector<Json> added;
        std::vector<Json> changed;
        std::vector<std::string> removed;
        for (const auto& [id, state] : after) {
            const auto found = before.find(id);
            if (found == before.end()) added.push_back(state);
            else if (found->second != state) changed.push_back(state);
        }
        for (const auto& [id, unused] : before) { static_cast<void>(unused); if (!after.contains(id)) removed.push_back(id); }
        const auto by_id = [](const Json& left, const Json& right) { return left.at("netEntityId").get<std::string>() < right.at("netEntityId").get<std::string>(); };
        std::ranges::sort(added, by_id);
        std::ranges::sort(changed, by_id);
        std::ranges::sort(removed);
        return Json{{"schemaVersion", "noemancer.network-delta/0.1"}, {"valid", true}, {"code", "ok"},
            {"serverPeerId", target.at("serverPeerId")}, {"baseTick", baseline.at("tick")}, {"targetTick", target.at("tick")},
            {"worldRevision", target.at("worldRevision")}, {"added", Json(std::move(added))}, {"changed", Json(std::move(changed))},
            {"removed", Json(std::move(removed))}}.dump();
    } catch (const std::exception& error) {
        return Json{{"schemaVersion", "noemancer.network-delta/0.1"}, {"valid", false},
            {"code", "network.delta.invalid-snapshot"}, {"detail", error.what()}}.dump();
    }
}

std::string NetworkReplicationRuntime::apply_snapshot_json(const std::string_view snapshot_json) {
    const auto revision_before = revision_;
    try {
        const auto snapshot = Json::parse(snapshot_json);
        if (snapshot.value("schemaVersion", std::string{}) != "noemancer.network-snapshot/0.1")
            return receipt(false, "network.snapshot.unsupported-schema", tick_, revision_before, revision_).dump();
        std::unordered_map<std::string, NetworkEntityReplica> next;
        for (const auto& value : snapshot.at("entities")) {
            auto replica = parse_replica(value);
            if (replica.net_entity_id != stable_net_entity_id(replica.entity_id))
                return receipt(false, "network.entity.identity-mismatch", tick_, revision_before, revision_).dump();
            next.emplace(replica.net_entity_id, std::move(replica));
        }
        replicas_ = std::move(next);
        tick_ = snapshot.at("tick").get<std::uint64_t>();
        ++revision_;
        auto result = receipt(true, "ok", tick_, revision_before, revision_);
        result["entityCount"] = replicas_.size();
        result["digest"] = state_digest(replicas_);
        return result.dump();
    } catch (const std::exception& error) {
        auto result = receipt(false, "network.snapshot.invalid", tick_, revision_before, revision_);
        result["detail"] = error.what();
        return result.dump();
    }
}

std::string NetworkReplicationRuntime::apply_delta_json(const std::string_view delta_document_json) {
    const auto revision_before = revision_;
    try {
        const auto delta = Json::parse(delta_document_json);
        if (!delta.value("valid", false) || delta.value("schemaVersion", std::string{}) != "noemancer.network-delta/0.1")
            return receipt(false, "network.delta.invalid", tick_, revision_before, revision_).dump();
        if (delta.at("baseTick").get<std::uint64_t>() != tick_)
            return receipt(false, "network.delta.baseline-mismatch", tick_, revision_before, revision_).dump();
        auto next = replicas_;
        for (const auto& id : delta.at("removed")) next.erase(id.get<std::string>());
        for (const auto* collection : {&delta.at("added"), &delta.at("changed")}) {
            for (const auto& value : *collection) {
                auto replica = parse_replica(value);
                if (replica.net_entity_id != stable_net_entity_id(replica.entity_id))
                    return receipt(false, "network.entity.identity-mismatch", tick_, revision_before, revision_).dump();
                next.insert_or_assign(replica.net_entity_id, std::move(replica));
            }
        }
        replicas_ = std::move(next);
        tick_ = delta.at("targetTick").get<std::uint64_t>();
        ++revision_;
        auto result = receipt(true, "ok", tick_, revision_before, revision_);
        result["entityCount"] = replicas_.size();
        result["digest"] = state_digest(replicas_);
        return result.dump();
    } catch (const std::exception& error) {
        auto result = receipt(false, "network.delta.invalid", tick_, revision_before, revision_);
        result["detail"] = error.what();
        return result.dump();
    }
}

bool NetworkReplicationRuntime::predict_position(const std::string_view net_entity_id,
                                                 const std::uint64_t input_sequence,
                                                 const NetworkVector3 displacement) {
    if (authoritative_server_ || input_sequence == 0) return false;
    const auto found = replicas_.find(std::string(net_entity_id));
    if (found == replicas_.end()) return false;
    found->second.position.x += displacement.x;
    found->second.position.y += displacement.y;
    found->second.position.z += displacement.z;
    pending_predictions_.push_back({input_sequence, std::string(net_entity_id), displacement});
    ++revision_;
    return true;
}

std::string NetworkReplicationRuntime::reconcile_json(const std::string_view authoritative_snapshot_json,
                                                      const std::uint64_t acknowledged_input_sequence) {
    const auto revision_before = revision_;
    const auto before = replicas_;
    const auto apply = Json::parse(apply_snapshot_json(authoritative_snapshot_json));
    if (!apply.value("success", false)) return apply.dump();
    const auto pending_before = pending_predictions_.size();
    std::erase_if(pending_predictions_, [acknowledged_input_sequence](const PendingPrediction& prediction) {
        return prediction.input_sequence <= acknowledged_input_sequence;
    });
    for (const auto& prediction : pending_predictions_) {
        const auto found = replicas_.find(prediction.net_entity_id);
        if (found == replicas_.end()) continue;
        found->second.position.x += prediction.displacement.x;
        found->second.position.y += prediction.displacement.y;
        found->second.position.z += prediction.displacement.z;
    }
    double maximum_correction = 0.0;
    for (const auto& [id, state] : replicas_) {
        const auto found = before.find(id);
        if (found == before.end()) continue;
        const auto dx = state.position.x - found->second.position.x;
        const auto dy = state.position.y - found->second.position.y;
        const auto dz = state.position.z - found->second.position.z;
        maximum_correction = std::max(maximum_correction, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    ++revision_;
    return Json{{"schemaVersion", "noemancer.network-reconciliation/0.1"}, {"success", true}, {"code", "ok"},
        {"acknowledgedInputSequence", acknowledged_input_sequence}, {"acknowledgedPredictionCount", pending_before - pending_predictions_.size()},
        {"replayedPredictionCount", pending_predictions_.size()}, {"maximumCorrection", maximum_correction},
        {"tick", tick_}, {"revisionBefore", revision_before}, {"revisionAfter", revision_},
        {"digest", state_digest(replicas_)}}.dump();
}

std::string NetworkReplicationRuntime::observe_json(const std::size_t max_entities) const {
    std::vector<std::string> ids;
    ids.reserve(replicas_.size());
    for (const auto& [id, unused] : replicas_) { static_cast<void>(unused); ids.push_back(id); }
    std::ranges::sort(ids);
    Json entities = Json::array();
    const auto count = std::min(max_entities, ids.size());
    for (std::size_t index = 0; index < count; ++index) entities.push_back(replica_json(replicas_.at(ids[index])));
    return Json{{"schemaVersion", "noemancer.network-observation/0.1"}, {"profile", "optional-authoritative-server"},
        {"localPeerId", local_peer_id_}, {"role", authoritative_server_ ? "server" : "client"},
        {"tick", tick_}, {"revision", revision_}, {"entityCount", replicas_.size()},
        {"pendingPredictionCount", pending_predictions_.size()}, {"digest", state_digest(replicas_)},
        {"truncated", count < ids.size()}, {"entities", std::move(entities)}}.dump();
}

std::string NetworkReplicationRuntime::profile_json() {
    return Json{{"schemaVersion", "noemancer.network-profile/0.1"}, {"profileId", "network.optional-authoritative"},
        {"enabledByDefault", false}, {"authorityModel", "server-authoritative"},
        {"replication", {"snapshot", "delta", "stable-net-entity-id"}},
        {"prediction", {"client-position-seam", "input-sequence-ack", "replay-after-reconcile"}},
        {"transport", Json{{"implemented", true},{"controlChannel","tcp-length-prefixed-json"},
            {"maximumControlBytes",65536},{"stateChannel", "udp-bounded-datagram"},
            {"maximumDatagramBytes",1200},{"kernelLoopbackVerified",true}}},
        {"server", Json{{"headlessCompatible", true},{"sessionBudget",Json{{"minimum",1},{"maximum",64}}},
            {"persistentSocketLoop",true},{"twoProcessRegression",true}}},
        {"releaseImpact", "excluded-when-profile-disabled"}}.dump();
}

} // namespace noemancer
