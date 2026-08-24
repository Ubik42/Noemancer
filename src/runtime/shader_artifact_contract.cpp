#include "runtime/shader_artifact_contract.hpp"

#include "engine/content_hash.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_shader_count = 65536U;
constexpr std::size_t maximum_stem_bytes = 256U;
constexpr std::size_t maximum_entrypoint_bytes = 256U;
constexpr std::uint32_t maximum_resource_count = 65535U;

std::string lower_ascii(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        const auto unsigned_character = static_cast<unsigned char>(character);
        result.push_back(static_cast<char>(std::tolower(unsigned_character)));
    }
    return result;
}

bool is_hex(const char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

bool valid_sha256(const std::string_view value) noexcept {
    if (value.size() != 71U || value.substr(0U, 7U) != "sha256:") return false;
    for (std::size_t index = 7U; index < value.size(); ++index)
        if (!is_hex(value[index])) return false;
    return true;
}

bool valid_stem(const std::string_view value) noexcept {
    if (value.empty() || value.size() > maximum_stem_bytes || value == "." || value == "..") return false;
    if (value.find('\0') != std::string_view::npos || value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos)
        return false;
    return true;
}

std::optional<std::filesystem::path> safe_relative_path(const std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos || value.front() == '/' ||
        value.front() == '\\' || (value.size() >= 2U && std::isalpha(static_cast<unsigned char>(value[0])) &&
            value[1] == ':'))
        return std::nullopt;

    std::size_t segment_begin = 0U;
    while (segment_begin <= value.size()) {
        const auto separator = value.find_first_of("/\\", segment_begin);
        const auto segment_end = separator == std::string_view::npos ? value.size() : separator;
        const auto segment = value.substr(segment_begin, segment_end - segment_begin);
        if (segment == "..") return std::nullopt;
        if (separator == std::string_view::npos) break;
        segment_begin = separator + 1U;
    }

    try {
        const auto path = std::filesystem::path(std::string(value));
        if (path.empty() || path == std::filesystem::path(".") || path.is_absolute() ||
            path.has_root_name() || path.has_root_directory())
            return std::nullopt;
        for (const auto& component : path) {
            if (component == std::filesystem::path("..")) return std::nullopt;
        }
        const auto normalized = path.lexically_normal();
        if (normalized.empty() || normalized == std::filesystem::path(".")) return std::nullopt;
        return normalized;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

const Json* find_field(const Json& object, const std::initializer_list<std::string_view> names) {
    if (!object.is_object()) return nullptr;
    for (const auto name : names) {
        const auto iterator = object.find(std::string(name));
        if (iterator != object.end()) return &*iterator;
    }
    return nullptr;
}

struct ParseFailure final {
    bool failed{};
    ShaderArtifactErrorCode code{ShaderArtifactErrorCode::ok};
    std::string detail;
};

void fail(ParseFailure& failure, const ShaderArtifactErrorCode code, std::string detail) {
    if (failure.failed) return;
    failure.failed = true;
    failure.code = code;
    failure.detail = std::move(detail);
}

bool required_string(const Json& object, const std::initializer_list<std::string_view> names,
    const std::string_view label, std::string& output, ParseFailure& failure) {
    const auto* value = find_field(object, names);
    if (value == nullptr) {
        fail(failure, ShaderArtifactErrorCode::missing_field,
            "Shader artifact manifest is missing required field '" + std::string(label) + "'.");
        return false;
    }
    if (!value->is_string()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field '" + std::string(label) + "' must be a string.");
        return false;
    }
    output = value->get<std::string>();
    return true;
}

bool optional_string(const Json& object, const std::initializer_list<std::string_view> names,
    const std::string_view label, std::string& output, bool& present, ParseFailure& failure) {
    const auto* value = find_field(object, names);
    if (value == nullptr) {
        present = false;
        return true;
    }
    present = true;
    if (!value->is_string()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field '" + std::string(label) + "' must be a string.");
        return false;
    }
    output = value->get<std::string>();
    return true;
}

bool optional_hash(const Json& object, const std::initializer_list<std::string_view> names,
    const std::string_view label, std::string& output, bool& present, ParseFailure& failure) {
    if (!optional_string(object, names, label, output, present, failure)) return false;
    if (present && !valid_sha256(output)) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field '" + std::string(label) + "' is not a SHA-256 value.");
        return false;
    }
    return true;
}

bool required_uint32(const Json& object, const std::initializer_list<std::string_view> names,
    const std::string_view label, const std::uint32_t maximum, std::uint32_t& output,
    ParseFailure& failure) {
    const auto* value = find_field(object, names);
    if (value == nullptr) {
        fail(failure, ShaderArtifactErrorCode::missing_field,
            "Shader artifact manifest is missing required field '" + std::string(label) + "'.");
        return false;
    }
    if (!value->is_number_unsigned() && !value->is_number_integer()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field '" + std::string(label) + "' must be a non-negative integer.");
        return false;
    }
    try {
        const auto integer = value->get<std::int64_t>();
        if (integer < 0 || static_cast<std::uint64_t>(integer) > maximum) {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact manifest field '" + std::string(label) + "' is outside its bounded range.");
            return false;
        }
        output = static_cast<std::uint32_t>(integer);
        return true;
    } catch (const std::exception&) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field '" + std::string(label) + "' is not a valid integer.");
        return false;
    }
}

bool optional_uint32(const Json& object, const std::initializer_list<std::string_view> names,
    const std::string_view label, const std::uint32_t maximum, std::uint32_t& output,
    bool& present, ParseFailure& failure) {
    const auto* value = find_field(object, names);
    if (value == nullptr) {
        present = false;
        return true;
    }
    present = true;
    if (!value->is_number_unsigned() && !value->is_number_integer()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field '" + std::string(label) + "' must be a non-negative integer.");
        return false;
    }
    try {
        const auto integer = value->get<std::int64_t>();
        if (integer < 0 || static_cast<std::uint64_t>(integer) > maximum) {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact manifest field '" + std::string(label) + "' is outside its bounded range.");
            return false;
        }
        output = static_cast<std::uint32_t>(integer);
        return true;
    } catch (const std::exception&) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field '" + std::string(label) + "' is not a valid integer.");
        return false;
    }
}

struct ParsedArtifact final {
    std::string path_text;
    std::filesystem::path relative_path;
    std::uintmax_t bytes{};
    std::string hash;
};

struct ParsedShader final {
    std::string stem;
    ShaderArtifactStage stage{ShaderArtifactStage::vertex};
    std::string entrypoint;
    ShaderResourceContract resources{};
    std::string source_hash;
    std::optional<ParsedArtifact> dxil;
    std::optional<ParsedArtifact> spv;
};

struct BinaryReadResult final {
    bool success{};
    ShaderArtifactErrorCode code{ShaderArtifactErrorCode::ok};
    std::string detail;
    std::vector<std::byte> bytes;
};

BinaryReadResult read_binary(const std::filesystem::path& path, const std::size_t maximum,
    const bool manifest) {
    BinaryReadResult result;
    const auto open_code = manifest ? ShaderArtifactErrorCode::manifest_open_failed :
                                     ShaderArtifactErrorCode::artifact_open_failed;
    const auto size_code = manifest ? ShaderArtifactErrorCode::manifest_too_large :
                                     ShaderArtifactErrorCode::artifact_too_large;
    const auto read_code = manifest ? ShaderArtifactErrorCode::manifest_read_failed :
                                     ShaderArtifactErrorCode::artifact_read_failed;

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        result.code = open_code;
        result.detail = manifest ? "Unable to open shader artifact manifest." :
                                   "Unable to open the selected shader artifact.";
        return result;
    }
    const auto end = input.tellg();
    if (end < 0) {
        result.code = read_code;
        result.detail = manifest ? "Unable to determine shader artifact manifest size." :
                                   "Unable to determine selected shader artifact size.";
        return result;
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > static_cast<std::uintmax_t>(maximum)) {
        result.code = size_code;
        result.detail = manifest ? "Shader artifact manifest exceeds the bounded parser size." :
                                   "Selected shader artifact exceeds the bounded load size.";
        return result;
    }
    result.bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!result.bytes.empty()) {
        input.read(reinterpret_cast<char*>(result.bytes.data()), static_cast<std::streamsize>(result.bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(result.bytes.size()) || input.bad()) {
            result.code = read_code;
            result.detail = manifest ? "Shader artifact manifest could not be read completely." :
                                       "Selected shader artifact could not be read completely.";
            result.bytes.clear();
            return result;
        }
    }
    std::error_code stat_error;
    const auto final_size = std::filesystem::file_size(path, stat_error);
    if (stat_error || final_size != size) {
        result.code = read_code;
        result.detail = manifest ? "Shader artifact manifest changed while it was being read." :
                                   "Selected shader artifact changed while it was being read.";
        result.bytes.clear();
        return result;
    }
    result.success = true;
    result.code = ShaderArtifactErrorCode::ok;
    result.detail = manifest ? "Shader artifact manifest bytes loaded." : "Shader artifact bytes loaded.";
    return result;
}

std::string hash_bytes(const std::vector<std::byte>& bytes) {
    const auto result = sha256_bytes(std::span<const std::byte>(bytes.data(), bytes.size()));
    return result.success ? result.value : std::string{};
}

bool extract_source_hash(const Json& object, std::string& output, bool& present, ParseFailure& failure) {
    std::string direct;
    bool direct_present = false;
    if (!optional_hash(object, {"sourceHash", "sourceSha256"}, "sourceHash", direct, direct_present, failure))
        return false;

    std::string nested;
    bool nested_present = false;
    const auto* source = find_field(object, {"source", "sourceContract"});
    if (source != nullptr) {
        if (source->is_string()) {
            nested = source->get<std::string>();
            nested_present = true;
            if (!valid_sha256(nested)) {
                fail(failure, ShaderArtifactErrorCode::invalid_field,
                    "Shader artifact manifest source is not a SHA-256 value.");
                return false;
            }
        } else if (source->is_object()) {
            if (!optional_hash(*source, {"sha256", "sourceHash", "sourceSha256", "hash"},
                    "source.sha256", nested, nested_present, failure))
                return false;
        } else {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact manifest field 'source' must be an object or SHA-256 string.");
            return false;
        }
    }

    if (direct_present && nested_present && direct != nested) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest source hash fields disagree.");
        return false;
    }
    present = direct_present || nested_present;
    output = direct_present ? direct : nested;
    return true;
}

bool parse_resources(const Json& entry, ShaderResourceContract& output, ParseFailure& failure) {
    const auto has_resource_counts = entry.contains("resourceCounts");
    const auto has_resources = entry.contains("resources");
    if (has_resource_counts && has_resources) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact entry must use one resource count object.");
        return false;
    }
    const Json* counts = nullptr;
    if (has_resource_counts) counts = &entry.at("resourceCounts");
    else if (has_resources) counts = &entry.at("resources");
    else counts = &entry;
    if (!counts->is_object()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact resource counts must be an object.");
        return false;
    }

    if (!required_uint32(*counts, {"uniformBuffers", "uniform_buffers", "uniformBufferCount", "numUniformBuffers"},
            "uniformBuffers", maximum_resource_count, output.uniform_buffers, failure) ||
        !required_uint32(*counts, {"samplers", "samplerCount", "numSamplers"}, "samplers",
            maximum_resource_count, output.samplers, failure))
        return false;

    bool generic_present = false;
    bool read_only_present = false;
    bool read_write_present = false;
    bool storage_textures_present = false;
    if (!optional_uint32(*counts, {"storageBuffers", "storage_buffers", "storageBufferCount", "numStorageBuffers"},
            "storageBuffers", maximum_resource_count, output.storage_buffers, generic_present, failure) ||
        !optional_uint32(*counts, {"readOnlyStorageBuffers", "readonlyStorageBuffers", "read_only_storage_buffers",
                "numReadonlyStorageBuffers"}, "readOnlyStorageBuffers", maximum_resource_count,
            output.read_only_storage_buffers, read_only_present, failure) ||
        !optional_uint32(*counts, {"readWriteStorageBuffers", "readwriteStorageBuffers", "read_write_storage_buffers",
                "numReadwriteStorageBuffers"}, "readWriteStorageBuffers", maximum_resource_count,
            output.read_write_storage_buffers, read_write_present, failure) ||
        !optional_uint32(*counts, {"storageTextures", "storage_textures", "storageTextureCount", "numStorageTextures"},
            "storageTextures", maximum_resource_count, output.storage_textures, storage_textures_present, failure))
        return false;
    static_cast<void>(storage_textures_present);

    if (!generic_present && !read_only_present && !read_write_present) {
        fail(failure, ShaderArtifactErrorCode::missing_field,
            "Shader artifact resource counts are missing 'storageBuffers'.");
        return false;
    }
    if (!generic_present && (read_only_present != read_write_present)) {
        fail(failure, ShaderArtifactErrorCode::missing_field,
            "Shader artifact resource counts must include both storage-buffer access counts.");
        return false;
    }
    if (read_only_present || read_write_present) {
        const auto total = static_cast<std::uint64_t>(output.read_only_storage_buffers) +
            static_cast<std::uint64_t>(output.read_write_storage_buffers);
        if (total > maximum_resource_count ||
            (total != 0U && generic_present && output.storage_buffers != 0U && output.storage_buffers != total)) {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact storage-buffer resource counts disagree.");
            return false;
        }
        if (total != 0U || !generic_present) output.storage_buffers = static_cast<std::uint32_t>(total);
    }
    return true;
}

bool parse_artifact(const Json& value, ParsedArtifact& output, ParseFailure& failure) {
    if (!value.is_object()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact backend entry must be an object.");
        return false;
    }
    if (!required_string(value, {"path", "relativePath", "artifactPath"}, "artifact.path",
            output.path_text, failure))
        return false;
    const auto path = safe_relative_path(output.path_text);
    if (!path) {
        fail(failure, ShaderArtifactErrorCode::unsafe_path,
            "Shader artifact path must be relative and must not contain traversal components.");
        return false;
    }
    output.relative_path = *path;

    std::uint64_t bytes{};
    const auto* byte_value = find_field(value, {"bytes", "size"});
    if (byte_value == nullptr) {
        fail(failure, ShaderArtifactErrorCode::missing_field,
            "Shader artifact backend entry is missing required field 'bytes'.");
        return false;
    }
    if (!byte_value->is_number_unsigned() && !byte_value->is_number_integer()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact backend field 'bytes' must be a non-negative integer.");
        return false;
    }
    try {
        const auto integer = byte_value->get<std::int64_t>();
        if (integer <= 0 || static_cast<std::uint64_t>(integer) > ShaderArtifactContract::default_max_artifact_bytes) {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact backend field 'bytes' is outside its bounded range.");
            return false;
        }
        bytes = static_cast<std::uint64_t>(integer);
    } catch (const std::exception&) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact backend field 'bytes' is not a valid integer.");
        return false;
    }
    output.bytes = static_cast<std::uintmax_t>(bytes);
    if (!required_string(value, {"sha256", "artifactHash", "hash"}, "artifact.sha256", output.hash, failure))
        return false;
    if (!valid_sha256(output.hash)) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact backend field 'sha256' is not a SHA-256 value.");
        return false;
    }
    return true;
}

bool parse_backend_artifacts(const Json& entry, ParsedShader& output, ParseFailure& failure) {
    const auto* artifacts = find_field(entry, {"artifacts"});
    if (artifacts != nullptr && !artifacts->is_object()) {
        fail(failure, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact entry field 'artifacts' must be an object.");
        return false;
    }
    if (artifacts == nullptr) artifacts = &entry;

    std::size_t artifact_count = 0U;
    for (const auto& [key, value] : artifacts->items()) {
        const auto backend = shader_artifact_backend_from_string(key);
        if (!backend || *backend == ShaderArtifactBackend::automatic) {
            // When artifacts was omitted, ordinary shader fields are ignored;
            // an explicit artifacts object is strict about backend names.
            if (find_field(entry, {"artifacts"}) != nullptr) {
                fail(failure, ShaderArtifactErrorCode::invalid_field,
                    "Shader artifact manifest contains an unsupported backend '" + key + "'.");
                return false;
            }
            continue;
        }
        ParsedArtifact parsed;
        if (!parse_artifact(value, parsed, failure)) return false;
        if (*backend == ShaderArtifactBackend::dxil) {
            if (output.dxil) {
                fail(failure, ShaderArtifactErrorCode::invalid_field,
                    "Shader artifact entry contains duplicate DXIL artifacts.");
                return false;
            }
            output.dxil = std::move(parsed);
        } else {
            if (output.spv) {
                fail(failure, ShaderArtifactErrorCode::invalid_field,
                    "Shader artifact entry contains duplicate SPIR-V artifacts.");
                return false;
            }
            output.spv = std::move(parsed);
        }
        ++artifact_count;
    }
    if (artifact_count == 0U) {
        fail(failure, ShaderArtifactErrorCode::missing_field,
            "Shader artifact entry must contain at least one DXIL or SPIR-V artifact.");
        return false;
    }
    return true;
}

std::filesystem::path executable_directory() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(512U, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) return {};
        if (static_cast<std::size_t>(length) + 1U < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768U) return {};
        buffer.resize(buffer.size() * 2U, L'\0');
    }
#elif defined(__linux__)
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#else
    std::error_code error;
    return std::filesystem::current_path(error);
#endif
}

bool exists_directory(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& target) {
    const auto relative = target.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& component : relative)
        if (component == std::filesystem::path("..")) return false;
    return true;
}

} // namespace

struct ShaderArtifactContract::Impl final {
    std::filesystem::path manifest_path;
    std::filesystem::path artifact_root;
    ShaderArtifactBackend selected_backend{ShaderArtifactBackend::automatic};
    bool valid{};
    ShaderArtifactErrorCode error{ShaderArtifactErrorCode::not_loaded};
    std::string code;
    std::string detail;
    std::string manifest_hash;
    std::string source_hash;
    std::unordered_map<std::string, ParsedShader> shaders;
};

namespace {

void set_impl_failure(ShaderArtifactContract::Impl& impl, const ShaderArtifactErrorCode code,
    std::string detail) {
    impl.valid = false;
    impl.error = code;
    impl.code = std::string(shader_artifact_error_code_name(code));
    impl.detail = std::move(detail);
}

std::shared_ptr<const ShaderArtifactContract::Impl> make_empty_impl() {
    auto impl = std::make_shared<ShaderArtifactContract::Impl>();
    impl->code = std::string(shader_artifact_error_code_name(ShaderArtifactErrorCode::not_loaded));
    impl->detail = "No shader artifact manifest has been loaded.";
    return impl;
}

std::shared_ptr<const ShaderArtifactContract::Impl> parse_manifest(
    const std::filesystem::path& requested_path, const ShaderArtifactBackend backend) {
    auto impl = std::make_shared<ShaderArtifactContract::Impl>();
    auto manifest_path = requested_path;
    if (manifest_path.empty()) manifest_path = default_shader_artifact_root() / "shader-artifact-manifest.json";
    std::error_code absolute_error;
    const auto absolute_path = std::filesystem::absolute(manifest_path, absolute_error);
    impl->manifest_path = absolute_error ? manifest_path : absolute_path.lexically_normal();
    impl->artifact_root = impl->manifest_path.parent_path();
    impl->selected_backend = backend;
    if (backend != ShaderArtifactBackend::automatic && backend != ShaderArtifactBackend::dxil &&
        backend != ShaderArtifactBackend::spv) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::backend_invalid,
            "The selected shader artifact backend is not supported by this contract.");
        return impl;
    }

    const auto manifest_read = read_binary(impl->manifest_path,
        ShaderArtifactContract::default_max_manifest_bytes, true);
    if (!manifest_read.success) {
        set_impl_failure(*impl, manifest_read.code, manifest_read.detail);
        return impl;
    }
    impl->manifest_hash = hash_bytes(manifest_read.bytes);
    if (impl->manifest_hash.empty()) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::manifest_read_failed,
            "Unable to compute the shader artifact manifest SHA-256.");
        return impl;
    }

    std::string manifest_text;
    if (!manifest_read.bytes.empty()) {
        manifest_text.assign(reinterpret_cast<const char*>(manifest_read.bytes.data()), manifest_read.bytes.size());
    }
    Json root;
    try {
        root = Json::parse(manifest_text);
    } catch (const std::exception& error) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::manifest_parse_failed,
            std::string("Unable to parse shader artifact manifest JSON: ") + error.what());
        return impl;
    }
    if (!root.is_object()) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::manifest_not_object,
            "Shader artifact manifest root must be a JSON object.");
        return impl;
    }

    ParseFailure failure;
    std::string schema;
    if (!required_string(root, {"schema", "schemaVersion"}, "schema", schema, failure)) {
        set_impl_failure(*impl, failure.code, failure.detail);
        return impl;
    }
    if (schema != shader_artifact_manifest_schema) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::unknown_schema,
            "Unsupported shader artifact manifest schema '" + schema + "'.");
        return impl;
    }

    std::string declared_manifest_hash;
    bool declared_manifest_hash_present = false;
    if (!optional_hash(root, {"manifestHash", "manifestSha256"}, "manifestHash",
            declared_manifest_hash, declared_manifest_hash_present, failure)) {
        set_impl_failure(*impl, failure.code, failure.detail);
        return impl;
    }
    if (declared_manifest_hash_present) {
        auto without_hash = root;
        without_hash.erase("manifestHash");
        without_hash.erase("manifestSha256");
        const auto canonical = without_hash.dump();
        const auto canonical_bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(canonical.data()), canonical.size());
        const auto canonical_hash = sha256_bytes(canonical_bytes);
        if (declared_manifest_hash != impl->manifest_hash &&
            (!canonical_hash.success || declared_manifest_hash != canonical_hash.value)) {
            set_impl_failure(*impl, ShaderArtifactErrorCode::manifest_hash_mismatch,
                "Shader artifact manifest declared hash does not match its bytes.");
            return impl;
        }
    }

    bool root_source_present = false;
    if (!extract_source_hash(root, impl->source_hash, root_source_present, failure)) {
        set_impl_failure(*impl, failure.code, failure.detail);
        return impl;
    }

    const auto shader_iterator = root.find("shaders");
    if (shader_iterator == root.end()) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::missing_field,
            "Shader artifact manifest is missing required field 'shaders'.");
        return impl;
    }
    if (!shader_iterator->is_array() || shader_iterator->size() > maximum_shader_count) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest field 'shaders' must be a bounded array.");
        return impl;
    }
    std::uint32_t declared_shader_count{};
    bool shader_count_present = false;
    if (!optional_uint32(root, {"shaderCount", "artifactCount"}, "shaderCount",
            static_cast<std::uint32_t>(maximum_shader_count), declared_shader_count,
            shader_count_present, failure)) {
        set_impl_failure(*impl, failure.code, failure.detail);
        return impl;
    }
    if (shader_count_present && declared_shader_count != shader_iterator->size()) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest shaderCount does not match the shaders array.");
        return impl;
    }

    for (std::size_t index = 0U; index < shader_iterator->size(); ++index) {
        const auto& value = (*shader_iterator)[index];
        if (!value.is_object()) {
            set_impl_failure(*impl, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact manifest shader entry must be an object.");
            return impl;
        }
        ParsedShader shader;
        if (!required_string(value, {"stem"}, "stem", shader.stem, failure)) break;
        if (!valid_stem(shader.stem)) {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact shader stem is empty or contains a path separator.");
            break;
        }
        if (impl->shaders.contains(shader.stem)) {
            fail(failure, ShaderArtifactErrorCode::duplicate_stem,
                "Shader artifact manifest contains duplicate stem '" + shader.stem + "'.");
            break;
        }

        std::string stage;
        if (!required_string(value, {"stage"}, "stage", stage, failure)) break;
        const auto parsed_stage = shader_artifact_stage_from_string(stage);
        if (!parsed_stage) {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact shader stage '" + stage + "' is unsupported.");
            break;
        }
        shader.stage = *parsed_stage;
        if (!required_string(value, {"entrypoint", "entryPoint"}, "entrypoint", shader.entrypoint, failure)) break;
        if (shader.entrypoint.empty() || shader.entrypoint.size() > maximum_entrypoint_bytes) {
            fail(failure, ShaderArtifactErrorCode::invalid_field,
                "Shader artifact entrypoint is empty or exceeds its bounded length.");
            break;
        }
        if (!parse_resources(value, shader.resources, failure)) break;

        bool entry_source_present = false;
        std::string entry_source_hash;
        if (!extract_source_hash(value, entry_source_hash, entry_source_present, failure)) break;
        shader.source_hash = entry_source_present ? entry_source_hash : impl->source_hash;
        if (shader.source_hash.empty()) {
            fail(failure, ShaderArtifactErrorCode::missing_field,
                "Shader artifact shader entry is missing required field 'sourceHash'.");
            break;
        }

        if (!parse_backend_artifacts(value, shader, failure)) break;
        impl->shaders.emplace(shader.stem, std::move(shader));
    }
    if (failure.failed) {
        set_impl_failure(*impl, failure.code, failure.detail);
        return impl;
    }
    if (impl->shaders.empty()) {
        set_impl_failure(*impl, ShaderArtifactErrorCode::invalid_field,
            "Shader artifact manifest must contain at least one shader entry.");
        return impl;
    }
    impl->valid = true;
    impl->error = ShaderArtifactErrorCode::ok;
    impl->code = std::string(shader_artifact_error_code_name(ShaderArtifactErrorCode::ok));
    impl->detail = "Shader artifact manifest parsed and validated.";
    static_cast<void>(root_source_present);
    return impl;
}

std::shared_ptr<const ShaderArtifactContract::Impl> make_impl(
    const std::filesystem::path& path, const std::string_view backend) {
    const auto parsed = shader_artifact_backend_from_string(backend);
    if (!parsed) {
        auto impl = std::make_shared<ShaderArtifactContract::Impl>();
        impl->manifest_path = path;
        impl->artifact_root = path.parent_path();
        impl->selected_backend = ShaderArtifactBackend::automatic;
        set_impl_failure(*impl, ShaderArtifactErrorCode::backend_invalid,
            "Unknown shader artifact backend '" + std::string(backend) + "'.");
        return impl;
    }
    return parse_manifest(path, *parsed);
}

ShaderArtifactLoadResult load_failure(const ShaderArtifactErrorCode code, std::string detail) {
    ShaderArtifactLoadResult result;
    result.success = false;
    result.code = std::string(shader_artifact_error_code_name(code));
    result.detail = std::move(detail);
    return result;
}

bool valid_request_stem(const std::string_view stem) {
    return valid_stem(stem);
}

std::optional<ShaderArtifactStage> request_stage(
    const std::variant<ShaderArtifactStage, std::string>& selector) {
    if (std::holds_alternative<ShaderArtifactStage>(selector)) return std::get<ShaderArtifactStage>(selector);
    return shader_artifact_stage_from_string(std::get<std::string>(selector));
}

bool valid_request_resources(const ShaderResourceContract& resources) {
    const auto total = static_cast<std::uint64_t>(resources.read_only_storage_buffers) +
        static_cast<std::uint64_t>(resources.read_write_storage_buffers);
    if (total > maximum_resource_count) return false;
    if ((resources.read_only_storage_buffers != 0U || resources.read_write_storage_buffers != 0U) &&
        resources.storage_buffers != 0U && resources.storage_buffers != total)
        return false;
    return resources.uniform_buffers <= maximum_resource_count && resources.samplers <= maximum_resource_count &&
        resources.storage_buffers <= maximum_resource_count && resources.storage_textures <= maximum_resource_count;
}

bool resources_match(const ShaderResourceContract& requested, const ShaderResourceContract& expected) {
    if (requested.uniform_buffers != expected.uniform_buffers || requested.samplers != expected.samplers ||
        requested.storage_textures != expected.storage_textures)
        return false;
    const auto requested_pair = requested.read_only_storage_buffers != 0U ||
        requested.read_write_storage_buffers != 0U;
    if (requested_pair) {
        if (requested.read_only_storage_buffers != expected.read_only_storage_buffers ||
            requested.read_write_storage_buffers != expected.read_write_storage_buffers)
            return false;
        return requested.storage_buffers == 0U || requested.storage_buffers == expected.storage_buffers;
    }
    return requested.storage_buffers == expected.storage_buffers;
}

const ParsedArtifact* selected_artifact(const ParsedShader& shader, const ShaderArtifactBackend backend) {
    if (backend == ShaderArtifactBackend::dxil) return shader.dxil ? &*shader.dxil : nullptr;
    if (backend == ShaderArtifactBackend::spv) return shader.spv ? &*shader.spv : nullptr;
    return nullptr;
}

std::optional<std::filesystem::path> resolved_artifact_path(
    const ShaderArtifactContract::Impl& impl, const ParsedArtifact& artifact) {
    std::error_code absolute_error;
    const auto absolute_root = std::filesystem::absolute(impl.artifact_root, absolute_error).lexically_normal();
    if (absolute_error || absolute_root.empty()) return std::nullopt;
    const auto lexical_path = (absolute_root / artifact.relative_path).lexically_normal();
    if (!path_is_within(absolute_root, lexical_path)) return std::nullopt;

    std::error_code canonical_error;
    const auto canonical_root = std::filesystem::weakly_canonical(absolute_root, canonical_error);
    const auto canonical_path = std::filesystem::weakly_canonical(lexical_path, canonical_error);
    if (!canonical_error && !path_is_within(canonical_root, canonical_path)) return std::nullopt;
    return lexical_path;
}

ShaderArtifactBackend resolve_backend(const ShaderArtifactContract::Impl& impl,
    const ParsedShader& shader, const ShaderArtifactBackend requested) {
    if (requested != ShaderArtifactBackend::automatic) return requested;
    if (impl.selected_backend != ShaderArtifactBackend::automatic) return impl.selected_backend;
    if (shader.dxil) return ShaderArtifactBackend::dxil;
    if (shader.spv) return ShaderArtifactBackend::spv;
    return ShaderArtifactBackend::automatic;
}

ShaderArtifactLoadResult load_from_impl(const ShaderArtifactContract::Impl& impl,
    const ShaderArtifactRequest& request, const ShaderArtifactBackend requested_backend) {
    if (!impl.valid) return load_failure(impl.error, impl.detail);
    if (!valid_request_stem(request.stem)) {
        return load_failure(ShaderArtifactErrorCode::request_invalid,
            "Shader artifact request stem is empty or contains a path separator.");
    }
    const auto stage = request_stage(request.stage);
    if (!stage) {
        return load_failure(ShaderArtifactErrorCode::request_invalid,
            "Shader artifact request stage must be vertex, fragment, or compute.");
    }
    if (!valid_request_resources(request.resources)) {
        return load_failure(ShaderArtifactErrorCode::request_invalid,
            "Shader artifact request resource counts are outside their bounded contract.");
    }

    const auto iterator = impl.shaders.find(request.stem);
    if (iterator == impl.shaders.end()) {
        return load_failure(ShaderArtifactErrorCode::shader_not_found,
            "Shader artifact manifest has no shader stem '" + request.stem + "'.");
    }
    const auto& shader = iterator->second;
    if (shader.stage != *stage) {
        return load_failure(ShaderArtifactErrorCode::stage_mismatch,
            "Shader artifact request stage does not match the manifest entry for '" + request.stem + "'.");
    }
    if (!resources_match(request.resources, shader.resources)) {
        return load_failure(ShaderArtifactErrorCode::resource_mismatch,
            "Shader artifact request resource counts do not match the manifest entry for '" + request.stem + "'.");
    }

    const auto backend = resolve_backend(impl, shader, requested_backend);
    if (backend == ShaderArtifactBackend::automatic) {
        return load_failure(ShaderArtifactErrorCode::backend_unsupported,
            "Shader artifact manifest has no DXIL or SPIR-V backend for '" + request.stem + "'.");
    }
    const auto* artifact = selected_artifact(shader, backend);
    if (artifact == nullptr) {
        return load_failure(ShaderArtifactErrorCode::backend_unsupported,
            "Requested shader artifact backend '" + std::string(shader_artifact_backend_name(backend)) +
            "' is not present for '" + request.stem + "'.");
    }
    const auto path = resolved_artifact_path(impl, *artifact);
    if (!path) {
        return load_failure(ShaderArtifactErrorCode::unsafe_path,
            "Selected shader artifact path escapes the manifest directory.");
    }
    const auto read = read_binary(*path, ShaderArtifactContract::default_max_artifact_bytes, false);
    if (!read.success) return load_failure(read.code, read.detail);
    if (static_cast<std::uintmax_t>(read.bytes.size()) != artifact->bytes) {
        return load_failure(ShaderArtifactErrorCode::artifact_size_mismatch,
            "Selected shader artifact byte count does not match the manifest.");
    }
    const auto actual_hash = hash_bytes(read.bytes);
    if (actual_hash.empty() || actual_hash != artifact->hash) {
        return load_failure(ShaderArtifactErrorCode::artifact_hash_mismatch,
            "Selected shader artifact SHA-256 does not match the manifest.");
    }

    ShaderArtifactLoadResult result;
    result.success = true;
    result.code = std::string(shader_artifact_error_code_name(ShaderArtifactErrorCode::ok));
    result.detail = "Shader artifact bytes loaded and verified against the manifest.";
    result.bytes = read.bytes;
    result.artifact_bytes = static_cast<std::uintmax_t>(result.bytes.size());
    result.stem = shader.stem;
    result.stage = std::string(shader_artifact_stage_name(shader.stage));
    result.format = std::string(shader_artifact_backend_name(backend));
    result.entrypoint = shader.entrypoint;
    result.resources = shader.resources;
    result.manifest_hash = impl.manifest_hash;
    result.source_hash = shader.source_hash;
    result.artifact_hash = actual_hash;
    return result;
}

} // namespace

std::string_view shader_artifact_stage_name(const ShaderArtifactStage stage) noexcept {
    switch (stage) {
    case ShaderArtifactStage::vertex: return "vertex";
    case ShaderArtifactStage::fragment: return "fragment";
    case ShaderArtifactStage::compute: return "compute";
    }
    return "unknown";
}

std::optional<ShaderArtifactStage> shader_artifact_stage_from_string(const std::string_view value) noexcept {
    const auto lowered = lower_ascii(value);
    if (lowered == "vertex" || lowered == "vert" || lowered == "vs" || lowered == "vs_6_0")
        return ShaderArtifactStage::vertex;
    if (lowered == "fragment" || lowered == "frag" || lowered == "pixel" || lowered == "ps" || lowered == "ps_6_0")
        return ShaderArtifactStage::fragment;
    if (lowered == "compute" || lowered == "comp" || lowered == "cs" || lowered == "cs_6_0")
        return ShaderArtifactStage::compute;
    return std::nullopt;
}

std::string_view shader_artifact_backend_name(const ShaderArtifactBackend backend) noexcept {
    switch (backend) {
    case ShaderArtifactBackend::automatic: return "auto";
    case ShaderArtifactBackend::dxil: return "dxil";
    case ShaderArtifactBackend::spv: return "spv";
    }
    return "unknown";
}

std::optional<ShaderArtifactBackend> shader_artifact_backend_from_string(const std::string_view value) noexcept {
    const auto lowered = lower_ascii(value);
    if (lowered.empty() || lowered == "auto" || lowered == "automatic") return ShaderArtifactBackend::automatic;
    if (lowered == "dxil") return ShaderArtifactBackend::dxil;
    if (lowered == "spv" || lowered == "spirv" || lowered == "spir-v") return ShaderArtifactBackend::spv;
    return std::nullopt;
}

std::string_view shader_artifact_error_code_name(const ShaderArtifactErrorCode code) noexcept {
    switch (code) {
    case ShaderArtifactErrorCode::ok: return "ok";
    case ShaderArtifactErrorCode::not_loaded: return "shader-artifact.not-loaded";
    case ShaderArtifactErrorCode::manifest_open_failed: return "shader-artifact.manifest-open-failed";
    case ShaderArtifactErrorCode::manifest_too_large: return "shader-artifact.manifest-too-large";
    case ShaderArtifactErrorCode::manifest_read_failed: return "shader-artifact.manifest-read-failed";
    case ShaderArtifactErrorCode::manifest_parse_failed: return "shader-artifact.manifest-parse-failed";
    case ShaderArtifactErrorCode::manifest_not_object: return "shader-artifact.manifest-not-object";
    case ShaderArtifactErrorCode::unknown_schema: return "shader-artifact.unknown-schema";
    case ShaderArtifactErrorCode::missing_field: return "shader-artifact.missing-field";
    case ShaderArtifactErrorCode::invalid_field: return "shader-artifact.invalid-field";
    case ShaderArtifactErrorCode::manifest_hash_mismatch: return "shader-artifact.manifest-hash-mismatch";
    case ShaderArtifactErrorCode::duplicate_stem: return "shader-artifact.duplicate-stem";
    case ShaderArtifactErrorCode::unsafe_path: return "shader-artifact.unsafe-path";
    case ShaderArtifactErrorCode::request_invalid: return "shader-artifact.request-invalid";
    case ShaderArtifactErrorCode::shader_not_found: return "shader-artifact.shader-not-found";
    case ShaderArtifactErrorCode::stage_mismatch: return "shader-artifact.stage-mismatch";
    case ShaderArtifactErrorCode::resource_mismatch: return "shader-artifact.resource-mismatch";
    case ShaderArtifactErrorCode::backend_invalid: return "shader-artifact.backend-invalid";
    case ShaderArtifactErrorCode::backend_unsupported: return "shader-artifact.backend-unsupported";
    case ShaderArtifactErrorCode::artifact_open_failed: return "shader-artifact.artifact-open-failed";
    case ShaderArtifactErrorCode::artifact_too_large: return "shader-artifact.artifact-too-large";
    case ShaderArtifactErrorCode::artifact_read_failed: return "shader-artifact.artifact-read-failed";
    case ShaderArtifactErrorCode::artifact_size_mismatch: return "shader-artifact.artifact-size-mismatch";
    case ShaderArtifactErrorCode::artifact_hash_mismatch: return "shader-artifact.artifact-hash-mismatch";
    }
    return "shader-artifact.invalid-error-code";
}

std::filesystem::path default_shader_artifact_root() {
    const auto base = executable_directory();
    if (!base.empty()) {
        const auto sibling = (base / ".." / "shaders").lexically_normal();
        if (exists_directory(sibling)) return sibling;
    }
#if defined(NOEMANCER_SHADER_DIR)
    return std::filesystem::path(NOEMANCER_SHADER_DIR);
#else
    if (!base.empty()) return (base / ".." / "shaders").lexically_normal();
    return std::filesystem::path("shaders");
#endif
}

ShaderArtifactContract::ShaderArtifactContract() : impl_(make_empty_impl()) {}

ShaderArtifactContract::ShaderArtifactContract(const std::filesystem::path& manifest_path)
    : impl_(parse_manifest(manifest_path, ShaderArtifactBackend::automatic)) {}

ShaderArtifactContract::ShaderArtifactContract(const std::filesystem::path& manifest_path,
    const ShaderArtifactBackend backend) : impl_(parse_manifest(manifest_path, backend)) {}

ShaderArtifactContract::ShaderArtifactContract(const std::filesystem::path& manifest_path,
    const std::string_view backend) : impl_(make_impl(manifest_path, backend)) {}

ShaderArtifactContract ShaderArtifactContract::load_manifest(
    const std::filesystem::path& manifest_path, const ShaderArtifactBackend backend) {
    return ShaderArtifactContract(manifest_path, backend);
}

ShaderArtifactContract ShaderArtifactContract::load_manifest(
    const std::filesystem::path& manifest_path, const std::string_view backend) {
    return ShaderArtifactContract(manifest_path, backend);
}

ShaderArtifactContract ShaderArtifactContract::load(
    const std::filesystem::path& manifest_path, const ShaderArtifactBackend backend) {
    return ShaderArtifactContract(manifest_path, backend);
}

ShaderArtifactContract ShaderArtifactContract::load(
    const std::filesystem::path& manifest_path, const std::string_view backend) {
    return ShaderArtifactContract(manifest_path, backend);
}

bool ShaderArtifactContract::valid() const noexcept {
    return impl_ != nullptr && impl_->valid;
}

ShaderArtifactErrorCode ShaderArtifactContract::error_code_value() const noexcept {
    return impl_ == nullptr ? ShaderArtifactErrorCode::not_loaded : impl_->error;
}

std::string_view ShaderArtifactContract::error_code() const noexcept {
    if (impl_ == nullptr) return shader_artifact_error_code_name(ShaderArtifactErrorCode::not_loaded);
    return impl_->code;
}

std::string_view ShaderArtifactContract::error_detail() const noexcept {
    if (impl_ == nullptr) return "No shader artifact manifest has been loaded.";
    return impl_->detail;
}

const std::filesystem::path& ShaderArtifactContract::manifest_path() const noexcept {
    static const std::filesystem::path empty_path;
    return impl_ == nullptr ? empty_path : impl_->manifest_path;
}

ShaderArtifactBackend ShaderArtifactContract::selected_backend() const noexcept {
    return impl_ == nullptr ? ShaderArtifactBackend::automatic : impl_->selected_backend;
}

std::string_view ShaderArtifactContract::manifest_hash() const noexcept {
    if (impl_ == nullptr) return {};
    return impl_->manifest_hash;
}

std::string_view ShaderArtifactContract::source_hash() const noexcept {
    if (impl_ == nullptr) return {};
    return impl_->source_hash;
}

ShaderArtifactLoadResult ShaderArtifactContract::load(const ShaderArtifactRequest& request) const {
    if (impl_ == nullptr) return load_failure(ShaderArtifactErrorCode::not_loaded,
        "No shader artifact manifest has been loaded.");
    return load_from_impl(*impl_, request, ShaderArtifactBackend::automatic);
}

ShaderArtifactLoadResult ShaderArtifactContract::load(const ShaderArtifactRequest& request,
    const ShaderArtifactBackend backend) const {
    if (impl_ == nullptr) return load_failure(ShaderArtifactErrorCode::not_loaded,
        "No shader artifact manifest has been loaded.");
    return load_from_impl(*impl_, request, backend);
}

ShaderArtifactLoadResult ShaderArtifactContract::load(const ShaderArtifactRequest& request,
    const std::string_view backend) const {
    const auto parsed = shader_artifact_backend_from_string(backend);
    if (!parsed) return load_failure(ShaderArtifactErrorCode::backend_invalid,
        "Unknown shader artifact backend '" + std::string(backend) + "'.");
    return load(request, *parsed);
}

} // namespace noemancer
