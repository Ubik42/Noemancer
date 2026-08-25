#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Engine-owned, renderer-neutral facts and planning contract for a future
// hardware ray-tracing path.  Runtime adapters may translate this data to
// D3D12/Vulkan objects, but native handles and third-party types never cross
// this boundary.  This contract describes capability and build ownership;
// it does not claim a complete ray-traced lighting or GI implementation.
inline constexpr std::string_view raytracing_capability_contract_schema =
    "noemancer.raytracing-capability-contract/0.1";
inline constexpr std::uint32_t raytracing_capability_max_geometry_count = 4096U;
inline constexpr std::uint32_t raytracing_capability_max_instance_count = 65536U;
inline constexpr std::uint64_t raytracing_capability_max_geometry_vertices =
    16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t raytracing_capability_max_geometry_indices =
    48ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t raytracing_capability_max_geometry_primitives =
    16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t raytracing_capability_max_memory_bytes =
    1ULL << 40U;
inline constexpr float raytracing_capability_max_world_value = 1.0e9F;
inline constexpr std::size_t raytracing_capability_max_text_bytes = 512U;
inline constexpr std::size_t raytracing_capability_max_diagnostics = 64U;
inline constexpr std::size_t raytracing_capability_max_evidence_items = 256U;

enum class RayTracingBackend : std::uint8_t {
    d3d12 = 0U,
    vulkan = 1U,

    D3D12 = d3d12,
    Vulkan = vulkan,
};

enum class RayTracingSupportState : std::uint8_t {
    invalid = 0U,
    unsupported = 1U,
    supported = 2U,

    Invalid = invalid,
    Unsupported = unsupported,
    Supported = supported,
};

enum class RayTracingBuildOperation : std::uint8_t {
    build = 0U,
    update = 1U,

    Build = build,
    Update = update,
};

enum class RayTracingCompactionPolicy : std::uint8_t {
    disabled = 0U,
    if_supported = 1U,
    required = 2U,

    Disabled = disabled,
    IfSupported = if_supported,
    Required = required,
};

enum class RayTracingQueueRole : std::uint8_t {
    graphics = 0U,
    compute = 1U,

    Graphics = graphics,
    Compute = compute,
};

enum class RayTracingRasterFallbackMode : std::uint8_t {
    forward_pbr = 0U,
    disabled = 1U,

    ForwardPbr = forward_pbr,
    Disabled = disabled,
};

enum class RayTracingUnsupportedReason : std::uint8_t {
    none = 0U,
    invalid_schema = 1U,
    invalid_backend = 2U,
    invalid_capability_facts = 3U,
    device_unsupported = 4U,
    acceleration_structure_unsupported = 5U,
    ray_tracing_pipeline_unsupported = 6U,
    update_unsupported = 7U,
    compaction_unsupported = 8U,
    update_compaction_conflict = 9U,
    invalid_policy = 10U,
    invalid_identity = 11U,
    invalid_geometry = 12U,
    invalid_instance = 13U,
    missing_geometry = 14U,
    count_limit = 15U,
    invalid_budget = 16U,
    budget_exceeded = 17U,
    queue_unsupported = 18U,
    synchronization_unsupported = 19U,

    None = none,
    InvalidSchema = invalid_schema,
    InvalidBackend = invalid_backend,
    InvalidCapabilityFacts = invalid_capability_facts,
    DeviceUnsupported = device_unsupported,
    AccelerationStructureUnsupported = acceleration_structure_unsupported,
    RayTracingPipelineUnsupported = ray_tracing_pipeline_unsupported,
    UpdateUnsupported = update_unsupported,
    CompactionUnsupported = compaction_unsupported,
    UpdateCompactionConflict = update_compaction_conflict,
    InvalidPolicy = invalid_policy,
    InvalidIdentity = invalid_identity,
    InvalidGeometry = invalid_geometry,
    InvalidInstance = invalid_instance,
    MissingGeometry = missing_geometry,
    CountLimit = count_limit,
    InvalidBudget = invalid_budget,
    BudgetExceeded = budget_exceeded,
    QueueUnsupported = queue_unsupported,
    SynchronizationUnsupported = synchronization_unsupported,
};

[[nodiscard]] std::string_view raytracing_backend_name(
    RayTracingBackend backend) noexcept;
[[nodiscard]] std::optional<RayTracingBackend> raytracing_backend_from_string(
    std::string_view value) noexcept;
[[nodiscard]] bool raytracing_backend_valid(RayTracingBackend backend) noexcept;

[[nodiscard]] std::string_view raytracing_support_state_name(
    RayTracingSupportState state) noexcept;
[[nodiscard]] std::string_view raytracing_build_operation_name(
    RayTracingBuildOperation operation) noexcept;
[[nodiscard]] std::string_view raytracing_compaction_policy_name(
    RayTracingCompactionPolicy policy) noexcept;
[[nodiscard]] std::string_view raytracing_queue_role_name(
    RayTracingQueueRole role) noexcept;
[[nodiscard]] std::string_view raytracing_raster_fallback_mode_name(
    RayTracingRasterFallbackMode mode) noexcept;
[[nodiscard]] std::string_view raytracing_unsupported_reason_name(
    RayTracingUnsupportedReason reason) noexcept;

[[nodiscard]] bool raytracing_support_state_valid(
    RayTracingSupportState state) noexcept;
[[nodiscard]] bool raytracing_build_operation_valid(
    RayTracingBuildOperation operation) noexcept;
[[nodiscard]] bool raytracing_compaction_policy_valid(
    RayTracingCompactionPolicy policy) noexcept;
[[nodiscard]] bool raytracing_queue_role_valid(RayTracingQueueRole role) noexcept;
[[nodiscard]] bool raytracing_raster_fallback_mode_valid(
    RayTracingRasterFallbackMode mode) noexcept;

struct RayTracingBackendFacts final {
    std::string schema{std::string(raytracing_capability_contract_schema)};
    RayTracingBackend backend{RayTracingBackend::d3d12};
    bool device_supported{};
    bool acceleration_structure_supported{};
    bool ray_tracing_pipeline_supported{};
    bool update_supported{};
    bool compaction_supported{};
    bool compute_build_supported{};
    bool compute_trace_supported{};
    bool cross_queue_synchronization_supported{};
    std::uint32_t max_recursion_depth{1U};
    std::uint32_t max_instance_count{1024U};
    std::uint64_t max_result_bytes{256ULL * 1024ULL * 1024ULL};
};

struct RayTracingBuildPolicy final {
    std::string schema{std::string(raytracing_capability_contract_schema)};
    bool allow_updates{true};
    RayTracingCompactionPolicy compaction{RayTracingCompactionPolicy::if_supported};
    bool allow_raster_fallback{true};
    RayTracingRasterFallbackMode raster_fallback_mode{
        RayTracingRasterFallbackMode::forward_pbr};
};

struct RayTracingMemoryBudget final {
    // Budgets are caller-owned reservations.  The plan reports conservative
    // required bytes separately; it never silently grows these reservations.
    std::uint64_t scratch_bytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t result_bytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t compaction_scratch_bytes{64ULL * 1024ULL * 1024ULL};
};

struct RayTracingQueueRequirements final {
    RayTracingQueueRole build_queue{RayTracingQueueRole::compute};
    RayTracingQueueRole trace_queue{RayTracingQueueRole::graphics};
    bool require_acceleration_structure_barrier{true};
    bool require_build_to_trace_fence{true};
};

struct RayTracingGeometryInput final {
    // Stable semantic identity, never an asset pointer or native resource
    // handle.  Revision changes intentionally invalidate the previous BLAS
    // build identity while preserving the logical geometry ID.
    std::string geometry_id;
    std::uint64_t revision{1U};
    std::uint64_t vertex_count{3U};
    std::uint64_t index_count{3U};
    std::uint64_t primitive_count{1U};
    std::array<float, 3U> bounds_min{-1.0F, -1.0F, -1.0F};
    std::array<float, 3U> bounds_max{1.0F, 1.0F, 1.0F};
    bool update_requested{};
};

struct RayTracingInstanceInput final {
    // Stable instance identity is distinct from geometry identity.  Multiple
    // instances may reference one geometry while retaining deterministic
    // update ordering.
    std::string instance_id;
    std::string geometry_id;
    std::uint64_t revision{1U};
    std::array<float, 16U> world_transform{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    bool update_requested{};
};

struct RayTracingCapabilityRequest final {
    std::string schema{std::string(raytracing_capability_contract_schema)};
    RayTracingBackendFacts backend;
    RayTracingBuildPolicy policy;
    RayTracingMemoryBudget budget;
    RayTracingQueueRequirements queues;
    std::vector<RayTracingGeometryInput> geometries;
    std::vector<RayTracingInstanceInput> instances;
};

struct RayTracingDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct RayTracingBudgetReport final {
    std::uint64_t required_scratch_bytes{};
    std::uint64_t required_result_bytes{};
    std::uint64_t required_compaction_scratch_bytes{};
    std::uint64_t available_scratch_bytes{};
    std::uint64_t available_result_bytes{};
    std::uint64_t available_compaction_scratch_bytes{};
    bool fits{};
};

struct RayTracingQueuePlan final {
    RayTracingQueueRole build_queue{RayTracingQueueRole::graphics};
    RayTracingQueueRole trace_queue{RayTracingQueueRole::graphics};
    bool requires_acceleration_structure_barrier{};
    bool requires_build_to_trace_fence{};
    bool requires_queue_ownership_transfer{};
    bool supported{};
};

struct RayTracingRasterFallback final {
    bool active{};
    RayTracingRasterFallbackMode mode{RayTracingRasterFallbackMode::forward_pbr};
    std::string reason;
};

struct RayTracingBlasPlan final {
    std::string geometry_id;
    std::uint64_t revision{};
    RayTracingBuildOperation operation{RayTracingBuildOperation::build};
    bool compaction_requested{};
    bool compaction_planned{};
    std::uint64_t result_bytes{};
    std::uint64_t scratch_bytes{};
    std::uint64_t compaction_scratch_bytes{};
};

struct RayTracingInstancePlan final {
    std::string instance_id;
    std::string geometry_id;
    std::uint64_t revision{};
    RayTracingBuildOperation operation{RayTracingBuildOperation::build};
};

struct RayTracingTlasPlan final {
    RayTracingBuildOperation operation{RayTracingBuildOperation::build};
    std::uint32_t instance_count{};
    std::uint64_t result_bytes{};
    std::uint64_t scratch_bytes{};
};

struct RayTracingCapabilityPlan final {
    std::string schema{std::string(raytracing_capability_contract_schema)};
    RayTracingBackend backend{RayTracingBackend::d3d12};
    RayTracingBackendFacts backend_facts;
    RayTracingSupportState state{RayTracingSupportState::invalid};
    RayTracingUnsupportedReason unsupported_reason{
        RayTracingUnsupportedReason::none};
    bool valid{};
    bool supported{};
    bool raster_fallback_active{};
    RayTracingBuildPolicy policy;
    RayTracingMemoryBudget budget;
    RayTracingBudgetReport budget_report;
    RayTracingQueuePlan queues;
    RayTracingRasterFallback raster_fallback;
    std::vector<RayTracingBlasPlan> blas;
    std::vector<RayTracingInstancePlan> instances;
    RayTracingTlasPlan tlas;
    std::vector<RayTracingDiagnostic> diagnostics;
};

[[nodiscard]] std::vector<RayTracingDiagnostic>
validate_raytracing_capability_request(
    const RayTracingCapabilityRequest& request);

[[nodiscard]] RayTracingCapabilityPlan
evaluate_raytracing_capability_contract(
    const RayTracingCapabilityRequest& request);

using RayTracingCapabilityEvaluation = RayTracingCapabilityPlan;

[[nodiscard]] std::string raytracing_capability_canonical_request(
    const RayTracingCapabilityRequest& request);
[[nodiscard]] std::string raytracing_capability_canonical_evidence(
    const RayTracingCapabilityPlan& plan);
[[nodiscard]] std::string raytracing_capability_fingerprint(
    const RayTracingCapabilityPlan& plan);

} // namespace noemancer
