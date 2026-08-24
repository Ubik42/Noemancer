#include "runtime/game_persistence_store.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>

int main() {
    const auto root=std::filesystem::temp_directory_path()/"noemancer-game-persistence-store-test";
    std::filesystem::remove_all(root);
    noemancer::GamePersistenceStore store(root,"game.persistence-fixture");
    const auto save=nlohmann::json{{"schemaVersion","noemancer.save-game/0.2"},{"sceneGuid","scene.fixture"}}.dump();
    const auto replay=nlohmann::json{{"schemaVersion","noemancer.input-replay/0.2"},{"samples",nlohmann::json::array()}}.dump();
    const auto save_written=store.write(noemancer::PersistedDocumentKind::save,"autosave",save);
    const auto save_loaded=store.read(noemancer::PersistedDocumentKind::save,"autosave");
    const auto replay_written=store.write(noemancer::PersistedDocumentKind::replay,"run-01",replay);
    const auto replay_loaded=store.read(noemancer::PersistedDocumentKind::replay,"run-01");
    const auto replaced=store.write(noemancer::PersistedDocumentKind::save,"autosave",save);
    const auto invalid_id=store.write(noemancer::PersistedDocumentKind::save,"../escape",save);
    const auto wrong_schema=store.write(noemancer::PersistedDocumentKind::save,"wrong",replay);
    const auto observation=nlohmann::json::parse(store.observe_json());
    if(!save_written.success||!save_loaded.success||save_loaded.document_json!=save||!replay_written.success||
       !replay_loaded.success||replay_loaded.document_json!=replay||!replaced.success||invalid_id.success||
       invalid_id.code!="persistence.invalid-id"||wrong_schema.success||wrong_schema.code!="persistence.document-schema-invalid"||
       observation.at("schemaVersion")!="noemancer.persistence-store/0.1"||observation.at("kinds").size()!=2U) {
        std::cerr<<observation.dump(2)<<'\n';std::filesystem::remove_all(root);return 1;
    }
    std::filesystem::remove_all(root);return 0;
}
