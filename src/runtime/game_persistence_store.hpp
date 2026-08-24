#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace noemancer {

enum class PersistedDocumentKind { save, replay };

struct PersistenceStoreReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::string project_id;
    std::string slot_id;
    std::filesystem::path path;
    std::string document_json;
};

class GamePersistenceStore final {
public:
    GamePersistenceStore(std::filesystem::path user_data_root,std::string project_id);
    [[nodiscard]] PersistenceStoreReceipt write(PersistedDocumentKind kind,std::string_view slot_id,
                                                 std::string_view document_json) const;
    [[nodiscard]] PersistenceStoreReceipt read(PersistedDocumentKind kind,std::string_view slot_id) const;
    [[nodiscard]] std::string observe_json() const;
private:
    [[nodiscard]] std::filesystem::path slot_path(PersistedDocumentKind kind,std::string_view slot_id) const;
    std::filesystem::path root_;
    std::string project_id_;
};

} // namespace noemancer
