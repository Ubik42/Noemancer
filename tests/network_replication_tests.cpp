#include "engine/network_replication.hpp"
#include "engine/network_transport.hpp"
#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>

int main() {
    using Json = nlohmann::json;
    noemancer::World world;
    if (!world.load_scene(noemancer::make_bootstrap_scene_document()).success) return 1;
    noemancer::NetworkReplicationRuntime server("server.test", true);
    auto views = world.entity_views();
    const auto baseline = server.capture_snapshot_json(10, world.revision(), views);
    auto reversed = views;
    std::ranges::reverse(reversed);
    if (baseline != server.capture_snapshot_json(10, world.revision(), reversed)) {
        std::cerr << "Network snapshot depends on ECS iteration order\n";
        return 2;
    }
    const auto baseline_document = Json::parse(baseline);
    if (baseline_document.at("schemaVersion") != "noemancer.network-snapshot/0.1" ||
        baseline_document.at("entityCount").get<std::size_t>() < 4 ||
        baseline_document.at("entities").at(0).at("authority").at("peerId") != "server.test" ||
        !baseline_document.at("entities").at(0).at("transform").contains("rotationQuaternion") ||
        !baseline_document.at("entities").at(0).at("transform").contains("scale")) return 3;

    noemancer::NetworkReplicationRuntime client("client.test", false);
    const auto snapshot_receipt = Json::parse(client.apply_snapshot_json(baseline));
    if (!snapshot_receipt.at("success") || snapshot_receipt.at("tick") != 10) return 4;

    const auto replicated_view = std::ranges::find_if(views, [](const noemancer::WorldEntityView& view) { return view.transform.has_value(); });
    if (replicated_view == views.end()) return 5;
    const auto replicated_entity_id = replicated_view->id;
    const auto net_id = noemancer::NetworkReplicationRuntime::stable_net_entity_id(replicated_entity_id);
    if (!client.predict_position(net_id, 1, {0.25, 0.0, 0.0}) ||
        !client.predict_position(net_id, 2, {0.5, 0.0, 0.0})) return 6;
    if (Json::parse(client.observe_json()).at("pendingPredictionCount") != 2) return 7;

    const auto changed_view = std::ranges::find(views, replicated_entity_id, &noemancer::WorldEntityView::id);
    if (changed_view == views.end() || !changed_view->transform) return 8;
    changed_view->transform->x += 1.0F;
    changed_view->revision += 1;
    const auto removed_view = std::ranges::find_if(views, [&replicated_entity_id](const noemancer::WorldEntityView& view) {
        return view.transform.has_value() && view.id != replicated_entity_id;
    });
    if (removed_view == views.end()) return 9;
    views.erase(removed_view);
    const auto target = server.capture_snapshot_json(11, world.revision() + 1, views);
    const auto delta = Json::parse(noemancer::NetworkReplicationRuntime::delta_json(baseline, target));
    if (!delta.at("valid") || delta.at("changed").size() != 1 || delta.at("removed").size() != 1) return 9;

    noemancer::NetworkReplicationRuntime delta_client("client.delta", false);
    static_cast<void>(delta_client.apply_snapshot_json(baseline));
    const auto delta_receipt = Json::parse(delta_client.apply_delta_json(delta.dump()));
    if (!delta_receipt.at("success") || delta_receipt.at("tick") != 11) return 10;
    noemancer::NetworkReplicationRuntime reference("client.reference", false);
    static_cast<void>(reference.apply_snapshot_json(target));
    if (Json::parse(delta_client.observe_json(0)).at("digest") != Json::parse(reference.observe_json(0)).at("digest")) return 11;
    const auto stale_delta = Json::parse(delta_client.apply_delta_json(delta.dump()));
    if (stale_delta.at("success") || stale_delta.at("code") != "network.delta.baseline-mismatch") return 12;

    const auto reconciliation = Json::parse(client.reconcile_json(target, 1));
    if (!reconciliation.at("success") || reconciliation.at("acknowledgedPredictionCount") != 1 ||
        reconciliation.at("replayedPredictionCount") != 1) return 13;

    auto tampered = baseline_document;
    tampered["entities"][0]["netEntityId"] = "net.entity.tampered";
    const auto tampered_receipt = Json::parse(delta_client.apply_snapshot_json(tampered.dump()));
    if (tampered_receipt.at("success") || tampered_receipt.at("code") != "network.entity.identity-mismatch") return 14;

    const auto profile = Json::parse(noemancer::NetworkReplicationRuntime::profile_json());
    if (profile.at("enabledByDefault") != false || profile.at("transport").at("implemented") != true ||
        profile.at("transport").at("controlChannel") != "tcp-length-prefixed-json" ||
        profile.at("server").at("headlessCompatible") != true || !profile.at("server").at("twoProcessRegression")) return 15;
    const auto transport=Json::parse(noemancer::verify_udp_loopback_transport_json(512));
    if(!transport.at("valid")||transport.at("transport")!="udp-loopback"||transport.at("payloadBytes")!=512||
        !transport.at("kernelSocket")) return 16;
    return 0;
}
