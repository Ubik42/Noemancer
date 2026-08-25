#include "engine/vfs_document_reader.hpp"

#include <cstdint>
#include <span>
#include <utility>

#include <nlohmann/json.hpp>

namespace noemancer {
namespace {

using Json = nlohmann::json;

[[nodiscard]] VfsDocumentReadResult failure(
    std::string code,
    std::string detail,
    VfsFileHandle file = {},
    std::string sha256 = {}) {
    return VfsDocumentReadResult{
        false, std::move(code), std::move(detail), std::move(file), std::move(sha256), {}, {}};
}

[[nodiscard]] bool continuation(const std::uint8_t value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

// Strict Unicode scalar-value validation: rejects truncated sequences,
// overlong forms, surrogate code points and values beyond U+10FFFF.
[[nodiscard]] bool valid_utf8(const std::span<const std::byte> bytes) noexcept {
    std::size_t index{};
    while (index < bytes.size()) {
        const auto first = std::to_integer<std::uint8_t>(bytes[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1U >= bytes.size() ||
                !continuation(std::to_integer<std::uint8_t>(bytes[index + 1U]))) return false;
            index += 2U;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2U >= bytes.size()) return false;
            const auto second = std::to_integer<std::uint8_t>(bytes[index + 1U]);
            const auto third = std::to_integer<std::uint8_t>(bytes[index + 2U]);
            if (!continuation(second) || !continuation(third) ||
                (first == 0xe0U && second < 0xa0U) ||
                (first == 0xedU && second >= 0xa0U)) return false;
            index += 3U;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3U >= bytes.size()) return false;
            const auto second = std::to_integer<std::uint8_t>(bytes[index + 1U]);
            if (!continuation(second) ||
                !continuation(std::to_integer<std::uint8_t>(bytes[index + 2U])) ||
                !continuation(std::to_integer<std::uint8_t>(bytes[index + 3U])) ||
                (first == 0xf0U && second < 0x90U) ||
                (first == 0xf4U && second >= 0x90U)) return false;
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] std::string bytes_to_string(const std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string_view without_utf8_bom(const std::string_view value) noexcept {
    constexpr std::string_view bom{"\xef\xbb\xbf", 3U};
    return value.starts_with(bom) ? value.substr(bom.size()) : value;
}

[[nodiscard]] const Json* schema_member(const Json& document) noexcept {
    if (!document.is_object()) return nullptr;
    const auto schema = document.find("schema");
    if (schema != document.end()) return &*schema;
    const auto version = document.find("schemaVersion");
    return version == document.end() ? nullptr : &*version;
}

} // namespace

VfsDocumentReadResult read_vfs_document(
    const VirtualFileSystem& vfs,
    const VfsDocumentReadRequest& request) {
    if (request.kind != VfsDocumentKind::text && request.kind != VfsDocumentKind::json) {
        return failure("vfs.document-kind-invalid", "The requested document kind is not supported.");
    }
    if (request.kind != VfsDocumentKind::json && request.expected_schema) {
        return failure("vfs.document-schema-unsupported",
            "Schema validation is available only for JSON documents.");
    }

    const auto read = vfs.read(VfsReadRequest{
        .uri = request.uri,
        .offset = 0U,
        .length = 0U,
        .byte_budget = request.byte_budget,
        .cancelled = request.cancelled});
    if (!read.success) {
        return failure(read.code, read.detail, read.file, read.sha256);
    }
    if (!valid_utf8(read.bytes)) {
        return failure("vfs.document-utf8-invalid",
            "The document is not valid UTF-8.", read.file, read.sha256);
    }
    if (request.cancelled && request.cancelled()) {
        return failure("vfs.cancelled",
            "The document read was cancelled without publishing parsed content.", read.file, read.sha256);
    }

    auto source = bytes_to_string(read.bytes);
    if (request.kind == VfsDocumentKind::text) {
        return VfsDocumentReadResult{
            true, "ok", "The UTF-8 text document was read.", read.file, read.sha256,
            std::move(source), {}};
    }

    const auto json_source = without_utf8_bom(source);
    auto document = Json::parse(json_source, nullptr, false);
    if (document.is_discarded()) {
        return failure("vfs.document-json-invalid",
            "The UTF-8 document is not valid JSON.", read.file, read.sha256);
    }
    if (request.expected_schema) {
        const auto* declared = schema_member(document);
        if (declared == nullptr || !declared->is_string()) {
            return failure("vfs.document-schema-missing",
                "The JSON document does not declare a string schema identity.", read.file, read.sha256);
        }
        if (declared->get_ref<const std::string&>() != *request.expected_schema) {
            return failure("vfs.document-schema-mismatch",
                "The JSON document schema identity does not match the request.", read.file, read.sha256);
        }
    }
    if (request.cancelled && request.cancelled()) {
        return failure("vfs.cancelled",
            "The document read was cancelled without publishing parsed content.", read.file, read.sha256);
    }

    try {
        auto canonical = document.dump(-1, ' ', false, Json::error_handler_t::strict);
        if (request.cancelled && request.cancelled()) {
            return failure("vfs.cancelled",
                "The document read was cancelled without publishing parsed content.", read.file, read.sha256);
        }
        return VfsDocumentReadResult{
            true, "ok", "The JSON document was parsed and canonicalized.", read.file,
            read.sha256, {}, std::move(canonical)};
    } catch (const Json::exception&) {
        return failure("vfs.document-json-canonicalization-failed",
            "The parsed JSON document could not be represented canonically.", read.file, read.sha256);
    }
}

std::string_view vfs_document_kind_name(const VfsDocumentKind kind) noexcept {
    switch (kind) {
    case VfsDocumentKind::text: return "text";
    case VfsDocumentKind::json: return "json";
    }
    return "unknown";
}

} // namespace noemancer
