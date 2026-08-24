#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct AssetRecord final {
    std::string id;
    std::string display_name;
    std::string kind;
    std::string uri;
    std::string source_root;
    std::string relative_path;
    std::string extension;
    std::string content_hash;
    std::string hash_provenance;
    std::string license;
    std::string redistribution;
    std::string import_state;
    std::uintmax_t source_bytes{};
    bool optional{};
    bool available{};
    // Authored asset intent. These are deliberately registry-owned scalar
    // fields; runtime streaming implementations must not leak into the
    // project-facing asset contract.
    std::string streaming_mode{"stream"};
    std::string streaming_importance{"normal"};
    std::uint32_t streaming_priority{500U};
    std::vector<std::string> tags;
    std::vector<std::string> dependencies;
};

struct AssetQuery final {
    std::string text;
    std::string kind;
    std::string import_state;
    std::vector<std::string> tags;
    std::size_t cursor{};
    std::size_t limit{64};
};

struct AssetSourceEditReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::string asset_id;
    std::string source;
    std::string manager;
    std::uint64_t registry_revision{};
    std::uint64_t transaction_id{};
};

struct AnimationGraphSourceResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string source;
};

struct AnimationGraphSourceValidation final {
    bool valid{};
    std::string code;
    std::string detail;
};

class AssetRegistry final {
public:
    explicit AssetRegistry(std::filesystem::path asset_root = default_asset_root());

    [[nodiscard]] bool refresh();
    [[nodiscard]] bool add_root(std::filesystem::path asset_root);
    [[nodiscard]] const std::vector<AssetRecord>& records() const noexcept;
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept;
    [[nodiscard]] const AssetRecord* find(std::string_view asset_id) const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::string registry_json() const;
    [[nodiscard]] std::string query_json(const AssetQuery& query) const;
    [[nodiscard]] std::string inspect_json(std::string_view asset_id) const;
    [[nodiscard]] std::string cook_plan_json(
        const std::vector<std::string>& asset_ids,
        std::string_view target_profile) const;
    [[nodiscard]] std::string apply_cook_plan_json(
        std::string_view plan_json,
        bool dry_run) const;
    [[nodiscard]] const std::filesystem::path& asset_root() const noexcept;
    [[nodiscard]] const std::vector<std::filesystem::path>& asset_roots() const noexcept;
    [[nodiscard]] std::filesystem::path source_path(const AssetRecord& asset) const;
    [[nodiscard]] AnimationGraphSourceResult read_animation_graph_source(
        std::string_view asset_id) const;
    [[nodiscard]] AnimationGraphSourceValidation validate_animation_graph_source(
        std::string_view asset_id,std::string_view source) const;
    [[nodiscard]] AssetSourceEditReceipt commit_text_source(std::string_view asset_id,
        std::string_view replacement,std::string_view manager,
        std::string_view expected_source = {});
    [[nodiscard]] AssetSourceEditReceipt undo_text_source(std::string_view manager);
    [[nodiscard]] AssetSourceEditReceipt redo_text_source(std::string_view manager);
    [[nodiscard]] AssetSourceEditReceipt rollback_text_source(std::uint64_t transaction_id,
        std::string_view manager);
    [[nodiscard]] bool can_undo_text_source() const noexcept;
    [[nodiscard]] bool can_redo_text_source() const noexcept;

    [[nodiscard]] static std::filesystem::path default_asset_root();

private:
    struct SourceEdit final {
        std::string asset_id;
        std::filesystem::path source;
        std::string before;
        std::string after;
        std::string manager;
        std::uint64_t transaction_id{};
    };
    [[nodiscard]] AssetSourceEditReceipt apply_source_history(SourceEdit edit,bool undo,std::string_view manager);
    std::filesystem::path asset_root_;
    std::vector<std::filesystem::path> asset_roots_;
    std::vector<AssetRecord> records_;
    std::vector<std::string> errors_;
    std::vector<SourceEdit> source_undo_stack_;
    std::vector<SourceEdit> source_redo_stack_;
    std::uint64_t revision_{};
    std::uint64_t next_source_transaction_id_{1};
};

} // namespace noemancer
