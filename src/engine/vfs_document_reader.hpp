#pragma once

#include "engine/virtual_file_system.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace noemancer {

enum class VfsDocumentKind : std::uint8_t {
    text,
    json
};

struct VfsDocumentReadRequest final {
    std::string uri;
    VfsDocumentKind kind{VfsDocumentKind::text};
    std::size_t byte_budget{1024U * 1024U};
    // When present, JSON must be an object whose string-valued `schema` or
    // `schemaVersion` member exactly matches this stable schema identity.
    std::optional<std::string> expected_schema;
    std::function<bool()> cancelled;
};

struct VfsDocumentReadResult final {
    bool success{};
    std::string code;
    std::string detail;
    VfsFileHandle file;
    std::string sha256;
    // Populated only for a successful text read. The UTF-8 bytes are retained
    // exactly as stored, including an optional UTF-8 BOM.
    std::string text;
    // Populated only for a successful JSON read. Object keys are ordered and
    // insignificant source whitespace is removed for stable downstream input.
    std::string canonical_json;
};

// Reads a complete UTF-8 document through the VFS authority. It never falls
// back to a physical path and never publishes partial text after cancellation
// or parsing failure.
[[nodiscard]] VfsDocumentReadResult read_vfs_document(
    const VirtualFileSystem& vfs,
    const VfsDocumentReadRequest& request);

[[nodiscard]] std::string_view vfs_document_kind_name(VfsDocumentKind kind) noexcept;

} // namespace noemancer
