#pragma once

#include "engine/world.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noemancer {

struct NetworkVector3 final {
    double x{};
    double y{};
    double z{};
};

struct NetworkQuaternion final {
    double x{};
    double y{};
    double z{};
    double w{1.0};
};

struct NetworkEntityReplica final {
    std::string net_entity_id;
    std::string entity_id;
    std::string authority_peer_id;
    std::uint64_t source_revision{};
    NetworkVector3 position;
    NetworkQuaternion rotation;
    NetworkVector3 scale{1.0, 1.0, 1.0};
    NetworkVector3 linear_velocity;
};

class NetworkReplicationRuntime final {
public:
    NetworkReplicationRuntime(std::string local_peer_id, bool authoritative_server);

    [[nodiscard]] std::string capture_snapshot_json(std::uint64_t tick, std::uint64_t world_revision,
                                                    const std::vector<WorldEntityView>& entities) const;
    [[nodiscard]] static std::string delta_json(std::string_view baseline_snapshot_json,
                                                std::string_view target_snapshot_json);
    [[nodiscard]] std::string apply_snapshot_json(std::string_view snapshot_json);
    [[nodiscard]] std::string apply_delta_json(std::string_view delta_document_json);
    [[nodiscard]] bool predict_position(std::string_view net_entity_id, std::uint64_t input_sequence,
                                        NetworkVector3 displacement);
    [[nodiscard]] std::string reconcile_json(std::string_view authoritative_snapshot_json,
                                             std::uint64_t acknowledged_input_sequence);
    [[nodiscard]] std::string observe_json(std::size_t max_entities = 64) const;
    [[nodiscard]] static std::string profile_json();
    [[nodiscard]] static std::string stable_net_entity_id(std::string_view entity_id);

private:
    struct PendingPrediction final {
        std::uint64_t input_sequence{};
        std::string net_entity_id;
        NetworkVector3 displacement;
    };

    std::string local_peer_id_;
    bool authoritative_server_{};
    std::uint64_t tick_{};
    std::uint64_t revision_{1};
    std::unordered_map<std::string, NetworkEntityReplica> replicas_;
    std::vector<PendingPrediction> pending_predictions_;
};

} // namespace noemancer
