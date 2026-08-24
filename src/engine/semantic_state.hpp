#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noemancer {

struct SourceAnchor final {
    std::string uri;
    std::string json_pointer;
};

struct SemanticRef final {
    std::string id;
    std::string path;
    std::string type;
    std::string schema_ref;
    std::string display_name;
    std::uint64_t revision{};
};

struct SemanticConvention final {
    std::string name;
    std::string value_type;
    std::string description;
    std::string unit;
    std::string coordinate_space;
    std::string stability;
    bool sensitive{};
};

struct ObservationQuery final {
    std::vector<std::string> entity_ids;
    std::vector<std::string> fields;
    std::size_t depth{1};
    std::size_t byte_budget{16 * 1024};
    std::size_t cursor{};
};

struct SemanticVector3 final {
    double x{};
    double y{};
    double z{};
};

struct SemanticDelta final {
    std::uint64_t revision_before{};
    std::uint64_t revision_after{};
    std::string entity_id;
    std::string field;
    SemanticVector3 before;
    SemanticVector3 after;
    std::string before_value_json;
    std::string after_value_json;
    std::string manager;
    bool undoable{};
};

struct PropertyChangePlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string content_hash;
    std::string manager;
    std::string entity_id;
    std::string property;
    std::uint64_t base_revision{};
    std::string before_value_json;
    std::string after_value_json;
};

struct TransformChangePlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string content_hash;
    std::string manager;
    std::string entity_id;
    std::uint64_t base_revision{};
    SemanticVector3 before;
    SemanticVector3 after;
};

struct ActionReceipt final {
    bool success{};
    bool dry_run{};
    std::string code;
    std::string detail;
    std::string operation_id;
    std::string plan_id;
    std::uint64_t revision_before{};
    std::uint64_t revision_after{};
    std::optional<SemanticDelta> delta;
};

class SemanticConventionRegistry final {
public:
    SemanticConventionRegistry();

    [[nodiscard]] const std::vector<SemanticConvention>& conventions() const noexcept;
    [[nodiscard]] std::string schema_json() const;

private:
    std::vector<SemanticConvention> conventions_;
};

} // namespace noemancer
