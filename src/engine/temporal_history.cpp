#include "engine/temporal_history.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace noemancer {
namespace {

constexpr TemporalHistoryResetMask known_reset_mask =
    temporal_history_reset_mask(TemporalHistoryResetReason::first_frame) |
    temporal_history_reset_mask(TemporalHistoryResetReason::frame_discontinuity) |
    temporal_history_reset_mask(TemporalHistoryResetReason::extent_changed) |
    temporal_history_reset_mask(TemporalHistoryResetReason::format_changed) |
    temporal_history_reset_mask(TemporalHistoryResetReason::camera_changed) |
    temporal_history_reset_mask(TemporalHistoryResetReason::projection_changed) |
    temporal_history_reset_mask(TemporalHistoryResetReason::profile_changed) |
    temporal_history_reset_mask(TemporalHistoryResetReason::quality_changed) |
    temporal_history_reset_mask(TemporalHistoryResetReason::manual) |
    temporal_history_reset_mask(TemporalHistoryResetReason::policy_disabled) |
    temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid);

struct ResetReasonName final {
    TemporalHistoryResetReason reason;
    std::string_view name;
};

constexpr std::array<ResetReasonName, 11U> reset_reason_names{{
    {TemporalHistoryResetReason::first_frame, "first-frame"},
    {TemporalHistoryResetReason::frame_discontinuity, "frame-discontinuity"},
    {TemporalHistoryResetReason::extent_changed, "extent-changed"},
    {TemporalHistoryResetReason::format_changed, "format-changed"},
    {TemporalHistoryResetReason::camera_changed, "camera-changed"},
    {TemporalHistoryResetReason::projection_changed, "projection-changed"},
    {TemporalHistoryResetReason::profile_changed, "profile-changed"},
    {TemporalHistoryResetReason::quality_changed, "quality-changed"},
    {TemporalHistoryResetReason::manual, "manual"},
    {TemporalHistoryResetReason::policy_disabled, "policy-disabled"},
    {TemporalHistoryResetReason::output_invalid, "output-invalid"},
}};

struct IdentityField final {
    std::string_view path;
    const std::string* value;
};

std::string bounded_text(std::string value, const std::size_t limit) {
    if (value.size() <= limit) return value;
    if (limit <= 3U) return value.substr(0U, limit);
    value.resize(limit - 3U);
    value += "...";
    return value;
}

TemporalHistoryDiagnostic diagnostic(std::string code, std::string detail,
                                     const TemporalHistoryConsumer consumer,
                                     const std::uint64_t revision) {
    return {bounded_text(std::move(code), temporal_history_max_diagnostic_bytes),
            bounded_text(std::move(detail), temporal_history_max_diagnostic_bytes),
            consumer, revision};
}

void add_diagnostic(std::vector<TemporalHistoryDiagnostic>& diagnostics,
                    TemporalHistoryDiagnostic value) {
    if (diagnostics.size() >= temporal_history_max_diagnostics) return;
    diagnostics.push_back(std::move(value));
}

void append_json_string(std::string& output, const std::string_view value) {
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(hex[(character >> 4U) & 0x0fU]);
                output.push_back(hex[character & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_uint(std::string& output, const std::uint64_t value) {
    std::array<char, 32U> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec == std::errc{}) output.append(buffer.data(), converted.ptr);
    else output += "0";
}

void append_bool(std::string& output, const bool value) {
    output += value ? "true" : "false";
}

void append_key(std::string& output, const std::string_view key, bool& first) {
    if (!first) output.push_back(',');
    append_json_string(output, key);
    output.push_back(':');
    first = false;
}

std::string reason_hex(const TemporalHistoryResetMask value) {
    std::array<char, 16U> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                         16);
    if (converted.ec != std::errc{}) return "0";
    return std::string(buffer.data(), converted.ptr);
}

bool identity_field_valid(const IdentityField& field,
                          std::vector<TemporalHistoryDiagnostic>& diagnostics,
                          const TemporalHistoryConsumer consumer,
                          const std::uint64_t revision) {
    if (!field.value->empty() && field.value->size() <= temporal_history_max_identity_bytes)
        return true;
    add_diagnostic(diagnostics,
                   diagnostic("temporal-history.invalid-identity",
                              std::string(field.path) +
                                  " must be a non-empty identity of at most 128 bytes.",
                              consumer, revision));
    return false;
}

std::vector<TemporalHistoryDiagnostic> validate_identity(
    const TemporalHistoryIdentity& identity,
    const TemporalHistoryConsumer consumer,
    const std::uint64_t revision) {
    std::vector<TemporalHistoryDiagnostic> diagnostics;
    if (identity.extent.width == 0U || identity.extent.height == 0U ||
        identity.extent.width > temporal_history_max_extent ||
        identity.extent.height > temporal_history_max_extent) {
        add_diagnostic(diagnostics,
                       diagnostic("temporal-history.invalid-extent",
                                  "extent width and height must be in [1,16384].",
                                  consumer, revision));
    }
    const std::array<IdentityField, 5U> fields{{
        {"format", &identity.format},
        {"cameraIdentity", &identity.camera_identity},
        {"projectionIdentity", &identity.projection_identity},
        {"profileIdentity", &identity.profile_identity},
        {"qualityIdentity", &identity.quality_identity},
    }};
    for (const auto& field : fields) identity_field_valid(field, diagnostics, consumer, revision);
    return diagnostics;
}

bool reset_mask_valid(const TemporalHistoryResetMask value) noexcept {
    return (value & ~known_reset_mask) == 0U;
}

std::string identity_material(const TemporalHistoryIdentity& identity) {
    std::string material;
    material.reserve(512U);
    material += std::to_string(identity.extent.width);
    material.push_back('/');
    material += std::to_string(identity.extent.height);
    material.push_back('\n');
    material += identity.format;
    material.push_back('\n');
    material += std::to_string(identity.frame_index);
    material.push_back('\n');
    material += identity.camera_identity;
    material.push_back('\n');
    material += identity.projection_identity;
    material.push_back('\n');
    material += identity.profile_identity;
    material.push_back('\n');
    material += identity.quality_identity;
    return material;
}

std::string fnv1a_hex(const std::string_view value) {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result(16U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned>((result.size() - 1U - index) * 4U);
        result[index] = hex[(hash >> shift) & 0x0fU];
    }
    return result;
}

std::string plan_id(const TemporalHistoryConsumer consumer,
                    const std::uint64_t base_revision,
                    const TemporalHistoryIdentity& identity,
                    const bool history_enabled,
                    const bool use_previous,
                    const bool previous_valid,
                    const TemporalHistoryResetMask reasons) {
    std::string material;
    material.reserve(640U);
    material += "plan/1\n";
    material += temporal_history_consumer_name(consumer);
    material.push_back('\n');
    material += std::to_string(base_revision);
    material.push_back('\n');
    material += identity_material(identity);
    material.push_back('\n');
    material += history_enabled ? "enabled" : "disabled";
    material.push_back('\n');
    material += use_previous ? "use-previous" : "no-previous";
    material.push_back('\n');
    material += previous_valid ? "previous-valid" : "previous-invalid";
    material.push_back('\n');
    material += std::to_string(reasons);
    return "temporal-history-plan/1:fnv1a64:" + fnv1a_hex(material);
}

void append_identity(std::string& output, const TemporalHistoryIdentity& identity) {
    output.push_back('{');
    bool first = true;
    append_key(output, "extent", first);
    output.push_back('{');
    bool extent_first = true;
    append_key(output, "width", extent_first);
    append_uint(output, identity.extent.width);
    append_key(output, "height", extent_first);
    append_uint(output, identity.extent.height);
    output.push_back('}');
    append_key(output, "format", first);
    append_json_string(output, identity.format);
    append_key(output, "frameIndex", first);
    append_uint(output, identity.frame_index);
    append_key(output, "cameraIdentity", first);
    append_json_string(output, identity.camera_identity);
    append_key(output, "projectionIdentity", first);
    append_json_string(output, identity.projection_identity);
    append_key(output, "profileIdentity", first);
    append_json_string(output, identity.profile_identity);
    append_key(output, "qualityIdentity", first);
    append_json_string(output, identity.quality_identity);
    output.push_back('}');
}

void append_reason_names(std::string& output, const TemporalHistoryResetMask reasons) {
    output.push_back('[');
    bool first = true;
    for (const auto& entry : reset_reason_names) {
        if ((reasons & temporal_history_reset_mask(entry.reason)) == 0U) continue;
        if (!first) output.push_back(',');
        append_json_string(output, entry.name);
        first = false;
    }
    output.push_back(']');
}

void append_state(std::string& output, const TemporalHistoryState& state) {
    output.push_back('{');
    bool first = true;
    append_key(output, "consumer", first);
    append_json_string(output, temporal_history_consumer_name(state.consumer));
    append_key(output, "revision", first);
    append_uint(output, state.revision);
    append_key(output, "lifecycle", first);
    append_json_string(output, state.lifecycle == TemporalHistoryLifecycle::begun ? "begun" : "idle");
    append_key(output, "currentValid", first);
    append_bool(output, state.current_valid);
    append_key(output, "previousValid", first);
    append_bool(output, state.previous_valid);
    append_key(output, "current", first);
    append_identity(output, state.current);
    append_key(output, "previous", first);
    append_identity(output, state.previous);
    append_key(output, "lastResetReasons", first);
    append_reason_names(output, state.last_reset_reasons);
    append_key(output, "commitCount", first);
    append_uint(output, state.commit_count);
    append_key(output, "resetCount", first);
    append_uint(output, state.reset_count);
    append_key(output, "activeTransactionId", first);
    append_json_string(output, state.active_transaction_id);
    output.push_back('}');
}

std::string canonical_state_json(const TemporalHistoryAuthority& authority) {
    const auto states = authority.states();
    std::string output;
    output.reserve(6'000U);
    output.push_back('{');
    bool first = true;
    append_key(output, "schema", first);
    append_json_string(output, temporal_history_schema);
    append_key(output, "consumers", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < states.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_state(output, states[index]);
    }
    output.push_back(']');
    output.push_back('}');
    return output;
}

TemporalHistoryPlan invalid_plan(const TemporalHistoryConsumer consumer,
                                 const std::uint64_t revision,
                                 std::string code,
                                 std::string detail) {
    TemporalHistoryPlan result;
    result.code = std::move(code);
    result.detail = std::move(detail);
    add_diagnostic(result.diagnostics,
                   diagnostic(result.code, result.detail, consumer, revision));
    return result;
}

} // namespace

std::string_view temporal_history_consumer_name(
    const TemporalHistoryConsumer consumer) noexcept {
    switch (consumer) {
    case TemporalHistoryConsumer::taa: return "taa";
    case TemporalHistoryConsumer::ssr: return "ssr";
    case TemporalHistoryConsumer::ssgi: return "ssgi";
    case TemporalHistoryConsumer::rtgi: return "rtgi";
    }
    return "unknown";
}

bool temporal_history_consumer_valid(const TemporalHistoryConsumer consumer) noexcept {
    return static_cast<std::uint8_t>(consumer) < temporal_history_consumer_count;
}

std::optional<TemporalHistoryConsumer> temporal_history_consumer_from_string(
    const std::string_view value) noexcept {
    if (value == "taa") return TemporalHistoryConsumer::taa;
    if (value == "ssr") return TemporalHistoryConsumer::ssr;
    if (value == "ssgi") return TemporalHistoryConsumer::ssgi;
    if (value == "rtgi") return TemporalHistoryConsumer::rtgi;
    return std::nullopt;
}

std::vector<std::string> temporal_history_reset_reason_names(
    const TemporalHistoryResetMask reasons) {
    std::vector<std::string> result;
    result.reserve(reset_reason_names.size());
    for (const auto& entry : reset_reason_names) {
        if ((reasons & temporal_history_reset_mask(entry.reason)) != 0U)
            result.emplace_back(entry.name);
    }
    if ((reasons & ~known_reset_mask) != 0U)
        result.emplace_back("unknown-0x" + reason_hex(reasons & ~known_reset_mask));
    return result;
}

std::string temporal_history_reset_reason_text(const TemporalHistoryResetMask reasons) {
    const auto names = temporal_history_reset_reason_names(reasons);
    if (names.empty()) return "none";
    std::string result;
    for (const auto& name : names) {
        if (!result.empty()) result.push_back('|');
        result += name;
    }
    return result;
}

TemporalHistoryAuthority::TemporalHistoryAuthority() {
    for (std::size_t index = 0U; index < slots_.size(); ++index) {
        slots_[index].state.consumer = static_cast<TemporalHistoryConsumer>(index);
    }
}

TemporalHistoryPlan TemporalHistoryAuthority::plan(
    const TemporalHistoryRequest& request) const {
    if (!temporal_history_consumer_valid(request.consumer))
        return invalid_plan(request.consumer, 0U, "temporal-history.invalid-consumer",
                            "The requested temporal history consumer is not supported.");

    const auto index = static_cast<std::size_t>(request.consumer);
    const auto& slot = slots_[index];
    const auto& current = slot.state;
    if (slot.active_plan.has_value())
        return invalid_plan(request.consumer, current.revision,
                            "temporal-history.transaction-active",
                            "A frame transaction is already begun for this consumer.");

    TemporalHistoryPlan result;
    result.consumer = request.consumer;
    result.base_revision = current.revision;
    result.identity = request.identity;
    result.history_enabled = request.history_enabled;
    result.previous_valid = current.current_valid;

    result.diagnostics = validate_identity(request.identity, request.consumer, current.revision);
    if (!result.diagnostics.empty()) {
        result.code = "temporal-history.invalid-identity";
        result.detail = "The temporal history identity is outside the bounded plain-data contract.";
        return result;
    }
    if (request.expected_revision.has_value() &&
        *request.expected_revision != current.revision) {
        result.code = "temporal-history.revision-conflict";
        result.detail = "The request revision does not match the consumer's current revision.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer, current.revision));
        return result;
    }

    if (!request.history_enabled) {
        result.reset_reasons = temporal_history_reset_mask(
            TemporalHistoryResetReason::policy_disabled);
        result.previous_valid = false;
    } else if (!current.current_valid) {
        result.reset_reasons = temporal_history_reset_mask(
            TemporalHistoryResetReason::first_frame);
        result.previous_valid = false;
    } else {
        const auto& previous_identity = current.current;
        const bool sequential = previous_identity.frame_index != std::numeric_limits<std::uint64_t>::max() &&
                                request.identity.frame_index == previous_identity.frame_index + 1U;
        if (!sequential)
            result.reset_reasons |= temporal_history_reset_mask(
                TemporalHistoryResetReason::frame_discontinuity);
        if (request.identity.extent != previous_identity.extent)
            result.reset_reasons |= temporal_history_reset_mask(
                TemporalHistoryResetReason::extent_changed);
        if (request.identity.format != previous_identity.format)
            result.reset_reasons |= temporal_history_reset_mask(
                TemporalHistoryResetReason::format_changed);
        if (request.identity.camera_identity != previous_identity.camera_identity)
            result.reset_reasons |= temporal_history_reset_mask(
                TemporalHistoryResetReason::camera_changed);
        if (request.identity.projection_identity != previous_identity.projection_identity)
            result.reset_reasons |= temporal_history_reset_mask(
                TemporalHistoryResetReason::projection_changed);
        if (request.identity.profile_identity != previous_identity.profile_identity)
            result.reset_reasons |= temporal_history_reset_mask(
                TemporalHistoryResetReason::profile_changed);
        if (request.identity.quality_identity != previous_identity.quality_identity)
            result.reset_reasons |= temporal_history_reset_mask(
                TemporalHistoryResetReason::quality_changed);
        result.previous_valid = result.reset_reasons == 0U && current.current_valid;
    }
    result.use_previous = request.history_enabled && result.previous_valid &&
                          result.reset_reasons == 0U;
    result.plan_id = plan_id(result.consumer, result.base_revision, result.identity,
                             result.history_enabled, result.use_previous,
                             result.previous_valid, result.reset_reasons);
    result.valid = true;
    result.code = result.reset_reasons == 0U ? "ok" : "temporal-history.reset-required";
    result.detail = result.reset_reasons == 0U
                        ? "The previous history is compatible and may be sampled."
                        : "The previous history must be rejected for the reported reset reasons.";
    return result;
}

TemporalHistoryBeginResult TemporalHistoryAuthority::begin(
    const TemporalHistoryPlan& input_plan) {
    TemporalHistoryBeginResult result;
    result.consumer = input_plan.consumer;
    if (!input_plan.valid) {
        result.code = "temporal-history.invalid-plan";
        result.detail = "Cannot begin an invalid temporal history plan.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, input_plan.consumer,
                                  input_plan.base_revision));
        return result;
    }
    if (!temporal_history_consumer_valid(input_plan.consumer)) {
        result.code = "temporal-history.invalid-consumer";
        result.detail = "The plan names an unsupported consumer.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, input_plan.consumer, 0U));
        return result;
    }

    auto& slot = slots_[static_cast<std::size_t>(input_plan.consumer)];
    if (slot.active_plan.has_value()) {
        result.code = "temporal-history.transaction-active";
        result.detail = "A frame transaction is already begun for this consumer.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, input_plan.consumer,
                                  slot.state.revision));
        return result;
    }
    if (slot.state.revision != input_plan.base_revision) {
        result.code = "temporal-history.revision-conflict";
        result.detail = "The plan was created against an older consumer revision.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, input_plan.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }

    TemporalHistoryRequest request;
    request.consumer = input_plan.consumer;
    request.identity = input_plan.identity;
    request.history_enabled = input_plan.history_enabled;
    request.expected_revision = input_plan.base_revision;
    const auto expected = plan(request);
    if (!expected.valid || expected.plan_id != input_plan.plan_id ||
        expected.reset_reasons != input_plan.reset_reasons ||
        expected.use_previous != input_plan.use_previous ||
        expected.previous_valid != input_plan.previous_valid) {
        result.code = "temporal-history.plan-mismatch";
        result.detail = "The plan payload no longer matches the current deterministic plan.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, input_plan.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }

    slot.active_plan = expected;
    slot.state.lifecycle = TemporalHistoryLifecycle::begun;
    slot.state.active_transaction_id = expected.plan_id;
    result.success = true;
    result.code = "ok";
    result.detail = "Temporal history frame transaction begun.";
    result.transaction_id = expected.plan_id;
    result.base_revision = expected.base_revision;
    result.use_previous = expected.use_previous;
    result.previous_valid = expected.previous_valid;
    result.reset_reasons = expected.reset_reasons;
    return result;
}

TemporalHistoryCommitResult TemporalHistoryAuthority::commit(
    const TemporalHistoryCommitRequest& request) {
    TemporalHistoryCommitResult result;
    result.consumer = request.consumer;
    if (!temporal_history_consumer_valid(request.consumer)) {
        result.code = "temporal-history.invalid-consumer";
        result.detail = "The requested temporal history consumer is not supported.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer, 0U));
        return result;
    }

    auto& slot = slots_[static_cast<std::size_t>(request.consumer)];
    result.revision = slot.state.revision;
    if (!slot.active_plan.has_value()) {
        result.code = "temporal-history.no-active-transaction";
        result.detail = "Commit requires a begun transaction for this consumer.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }
    if (request.transaction_id.empty() ||
        request.transaction_id != slot.state.active_transaction_id) {
        result.code = "temporal-history.transaction-mismatch";
        result.detail = "The commit transaction identity does not match the begun plan.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }
    if (request.expected_revision.has_value() &&
        *request.expected_revision != slot.state.revision) {
        result.code = "temporal-history.revision-conflict";
        result.detail = "The commit revision does not match the begun transaction.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }
    if (!reset_mask_valid(request.output_reset_reasons)) {
        result.code = "temporal-history.invalid-reset-reasons";
        result.detail = "The commit contains an unknown reset-reason bit.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }
    if (slot.state.revision == std::numeric_limits<std::uint64_t>::max()) {
        result.code = "temporal-history.revision-overflow";
        result.detail = "The consumer revision cannot advance beyond uint64 max.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }

    const auto active_plan = *slot.active_plan;
    const auto old_current = slot.state.current;
    const auto old_current_valid = slot.state.current_valid;
    auto reset_reasons = active_plan.reset_reasons | request.output_reset_reasons;
    if (!request.produced_history)
        reset_reasons |= temporal_history_reset_mask(TemporalHistoryResetReason::output_invalid);

    slot.state.revision += 1U;
    slot.state.commit_count += 1U;
    slot.state.last_reset_reasons = reset_reasons;
    if (request.produced_history && active_plan.history_enabled) {
        slot.state.previous = old_current;
        slot.state.previous_valid = old_current_valid && active_plan.reset_reasons == 0U &&
                                    request.output_reset_reasons == 0U;
        slot.state.current = active_plan.identity;
        slot.state.current_valid = true;
    } else {
        slot.state.previous = old_current;
        slot.state.previous_valid = false;
        slot.state.current = active_plan.identity;
        slot.state.current_valid = false;
    }
    if (reset_reasons != 0U) slot.state.reset_count += 1U;
    slot.state.lifecycle = TemporalHistoryLifecycle::idle;
    slot.state.active_transaction_id.clear();
    slot.active_plan.reset();

    result.success = true;
    result.code = "ok";
    result.detail = "Temporal history frame transaction committed.";
    result.revision = slot.state.revision;
    result.current_valid = slot.state.current_valid;
    result.previous_valid = slot.state.previous_valid;
    result.reset_reasons = reset_reasons;
    return result;
}

TemporalHistoryResetResult TemporalHistoryAuthority::reset(
    const TemporalHistoryResetRequest& request) {
    TemporalHistoryResetResult result;
    result.consumer = request.consumer;
    if (!temporal_history_consumer_valid(request.consumer)) {
        result.code = "temporal-history.invalid-consumer";
        result.detail = "The requested temporal history consumer is not supported.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer, 0U));
        return result;
    }
    auto& slot = slots_[static_cast<std::size_t>(request.consumer)];
    result.revision = slot.state.revision;
    if (slot.active_plan.has_value()) {
        result.code = "temporal-history.transaction-active";
        result.detail = "Reset cannot interrupt a begun transaction; commit or abandon it first.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }
    if (request.expected_revision.has_value() &&
        *request.expected_revision != slot.state.revision) {
        result.code = "temporal-history.revision-conflict";
        result.detail = "The reset revision does not match the consumer's current revision.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }
    if (!reset_mask_valid(request.reasons)) {
        result.code = "temporal-history.invalid-reset-reasons";
        result.detail = "The reset contains an unknown reset-reason bit.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }
    if (slot.state.revision == std::numeric_limits<std::uint64_t>::max()) {
        result.code = "temporal-history.revision-overflow";
        result.detail = "The consumer revision cannot advance beyond uint64 max.";
        add_diagnostic(result.diagnostics,
                       diagnostic(result.code, result.detail, request.consumer,
                                  slot.state.revision));
        record_diagnostics(result.diagnostics);
        return result;
    }

    const auto reasons = request.reasons == 0U
                             ? temporal_history_reset_mask(TemporalHistoryResetReason::manual)
                             : request.reasons;
    slot.state.revision += 1U;
    slot.state.current_valid = false;
    slot.state.previous_valid = false;
    slot.state.last_reset_reasons = reasons;
    slot.state.reset_count += 1U;
    slot.state.lifecycle = TemporalHistoryLifecycle::idle;
    slot.state.active_transaction_id.clear();

    result.success = true;
    result.code = "ok";
    result.detail = "Temporal history was explicitly reset.";
    result.revision = slot.state.revision;
    result.reset_reasons = reasons;
    return result;
}

TemporalHistoryState TemporalHistoryAuthority::state(
    const TemporalHistoryConsumer consumer) const noexcept {
    if (!temporal_history_consumer_valid(consumer)) {
        TemporalHistoryState result;
        result.consumer = consumer;
        return result;
    }
    return slots_[static_cast<std::size_t>(consumer)].state;
}

std::array<TemporalHistoryState, temporal_history_consumer_count>
TemporalHistoryAuthority::states() const {
    std::array<TemporalHistoryState, temporal_history_consumer_count> result{};
    for (std::size_t index = 0U; index < result.size(); ++index)
        result[index] = slots_[index].state;
    return result;
}

void TemporalHistoryAuthority::record_diagnostics(
    const std::vector<TemporalHistoryDiagnostic>& diagnostics) {
    for (const auto& value : diagnostics) {
        if (diagnostics_.size() >= temporal_history_max_diagnostics) {
            diagnostics_truncated_ = true;
            break;
        }
        diagnostics_.push_back(value);
    }
}

const std::vector<TemporalHistoryDiagnostic>& TemporalHistoryAuthority::diagnostics() const noexcept {
    return diagnostics_;
}

void TemporalHistoryAuthority::clear_diagnostics() noexcept {
    diagnostics_.clear();
    diagnostics_truncated_ = false;
}

std::string TemporalHistoryAuthority::canonical_evidence() const {
    std::string output = canonical_state_json(*this);
    if (output.empty() || output.back() != '}') return output;
    output.pop_back();
    bool first = false;
    append_key(output, "diagnostics", first);
    output.push_back('[');
    for (std::size_t index = 0U; index < diagnostics_.size(); ++index) {
        if (index != 0U) output.push_back(',');
        const auto& value = diagnostics_[index];
        output.push_back('{');
        bool item_first = true;
        append_key(output, "code", item_first);
        append_json_string(output, value.code);
        append_key(output, "detail", item_first);
        append_json_string(output, value.detail);
        append_key(output, "consumer", item_first);
        append_json_string(output, temporal_history_consumer_name(value.consumer));
        append_key(output, "revision", item_first);
        append_uint(output, value.revision);
        output.push_back('}');
    }
    output.push_back(']');
    append_key(output, "diagnosticsTruncated", first);
    append_bool(output, diagnostics_truncated_);
    output.push_back('}');
    return output;
}

std::string TemporalHistoryAuthority::fingerprint() const {
    return "fnv1a64:" + fnv1a_hex(canonical_state_json(*this));
}

std::string temporal_history_canonical_evidence(
    const TemporalHistoryAuthority& authority) {
    return authority.canonical_evidence();
}

std::string temporal_history_fingerprint(const TemporalHistoryAuthority& authority) {
    return authority.fingerprint();
}

} // namespace noemancer
