#include "engine/asset_registry.hpp"
#include "engine/gltf_mesh.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;

struct ScopedFixture final {
    std::filesystem::path root;
    std::filesystem::path assets;
    std::filesystem::path source;
    std::filesystem::path geometry;
    std::filesystem::path image;

    ~ScopedFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

int fail(const int code, const std::string& detail) {
    std::cerr << "gltf external Cook test failure (" << code << "): " << detail << '\n';
    return code;
}

template <typename T>
void append_value(std::vector<std::byte>& bytes, const T value) {
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::vector<std::byte> decode_base64(std::string_view encoded) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table{};
    table.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        table[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }
    std::vector<std::byte> decoded;
    std::uint32_t accumulator{};
    int bits{};
    for (const auto value : encoded) {
        if (value == '=') break;
        const int digit = table[static_cast<unsigned char>(value)];
        if (digit < 0) continue;
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<std::byte>((accumulator >> bits) & 0xffU));
        }
    }
    return decoded;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return output.good();
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    if (!bytes.empty()) std::memcpy(bytes.data(), text.data(), text.size());
    return write_bytes(path, bytes);
}

bool make_fixture(ScopedFixture& fixture, std::string& error) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fixture.root = std::filesystem::temp_directory_path() /
        ("noemancer-gltf-external-cook-" + std::to_string(stamp));
    fixture.assets = fixture.root / "assets";
    fixture.source = fixture.assets / "models" / "triangle.gltf";
    fixture.geometry = fixture.assets / "models" / "geometry.bin";
    fixture.image = fixture.assets / "models" / "base.jpg";

    std::error_code filesystem_error;
    std::filesystem::create_directories(fixture.source.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "could not create fixture directories: " + filesystem_error.message();
        return false;
    }

    std::vector<std::byte> geometry;
    for (const float value : std::array{
             -0.5F, 0.0F, 0.0F,
              0.5F, 0.0F, 0.0F,
              0.0F, 1.0F, 0.0F}) {
        append_value(geometry, value);
    }
    if (!write_bytes(fixture.geometry, geometry)) {
        error = "could not write external geometry.bin";
        return false;
    }

    // A small 2x2 JPEG fixture shared by the glTF importer tests. Keeping the
    // encoded bytes external to the JSON exercises the same closure that a
    // real authoring project uses.
    constexpr std::string_view jpeg_base64 =
        "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAIDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD7r/Zv8B+GtS/Z4+F13d+HdJuru48LaXLNPPYxPJI7WkRZmYrkkkkknrmiiivx3H/73W/xS/Nn4Fmf+/V/8cvzZ//Z";
    const auto jpeg = decode_base64(jpeg_base64);
    if (jpeg.empty() || !write_bytes(fixture.image, jpeg)) {
        error = "could not write external 2x2 JPEG";
        return false;
    }

    const Json gltf = {
        {"asset", {{"version", "2.0"}, {"generator", "noemancer.gltf-external-cook-test"}}},
        {"buffers", {{{"uri", "geometry.bin"}, {"byteLength", geometry.size()}}}},
        {"bufferViews", {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", geometry.size()}}}},
        {"accessors", {{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}}}},
        {"images", {{{"uri", "base.jpg"}, {"mimeType", "image/jpeg"}}}},
        {"textures", {{{"source", 0}}}},
        {"materials", {{{"pbrMetallicRoughness", {
            {"baseColorTexture", {{"index", 0}}}}}}}},
        {"meshes", {{{"name", "ExternalTriangle"}, {"primitives", {{
            {"attributes", {{"POSITION", 0}}}, {"material", 0}}}}}}},
        {"nodes", {{{"name", "TriangleNode"}, {"mesh", 0}}}},
        {"scenes", {{{"nodes", {0}}}}},
        {"scene", 0}
    };
    if (!write_text(fixture.source, gltf.dump(2) + "\n")) {
        error = "could not write external triangle.gltf";
        return false;
    }

    const Json manifest = {
        {"schema", "noemancer.assets/0.1"},
        {"assets", Json::array({{
            {"id", "mesh.external-triangle"},
            {"displayName", "External Triangle"},
            {"kind", "Mesh"},
            {"uri", "asset://models/triangle.gltf"},
            {"path", "models/triangle.gltf"},
            {"license", "Noemancer test fixture"},
            {"redistribution", "test-only"},
            {"tags", {"gltf", "external", "jpeg"}}
        }, {
            {"id", "texture.external-jpeg"},
            {"displayName", "External JPEG"},
            {"kind", "Texture"},
            {"uri", "asset://models/base.jpg"},
            {"path", "models/base.jpg"},
            {"license", "Noemancer test fixture"},
            {"redistribution", "test-only"},
            {"tags", {"jpeg", "texture"}}
        }})}
    };
    if (!write_text(fixture.assets / "registry.json", manifest.dump(2) + "\n")) {
        error = "could not write fixture registry.json";
        return false;
    }
    return true;
}

Json find_input(const Json& plan, const std::string_view asset_id) {
    for (const auto& input : plan.value("inputs", Json::array())) {
        if (input.value("assetId", std::string{}) == asset_id) return input;
    }
    return Json::object();
}

} // namespace

int main() {
    ScopedFixture fixture;
    std::string fixture_error;
    if (!make_fixture(fixture, fixture_error)) return fail(1, fixture_error);

    const noemancer::AssetRegistry registry(fixture.assets);
    const auto* asset = registry.find("mesh.external-triangle");
    if (asset == nullptr || !asset->available || !registry.errors().empty()) {
        return fail(2, "fixture AssetRegistry did not load a clean external glTF asset");
    }

    const auto inspection = Json::parse(registry.inspect_json("mesh.external-triangle"));
    const auto& imported = inspection.at("importedMetadata");
    if (!inspection.value("valid", false) || inspection.value("code", std::string{}) != "ok" ||
        !imported.value("valid", false) || imported.value("format", std::string{}) != "gltf-json" ||
        imported.value("externalDependencyCount", 0U) != 2U ||
        imported.value("sourceClosureBytes", 0ULL) == 0ULL ||
        !imported.value("sourceClosureHash", std::string{}).starts_with("sha256:")) {
        return fail(3, "inspect did not expose a valid external glTF closure: " + inspection.dump());
    }

    const auto plan = Json::parse(registry.cook_plan_json(
        {"mesh.external-triangle"}, "windows-x64-debug"));
    const auto input = find_input(plan, "mesh.external-triangle");
    const auto build_inputs = input.value("buildInputs", Json::array());
    if (!plan.value("valid", false) || input.empty() ||
        !input.value("importer", std::string{}).starts_with("gltf.json-external/0.1") ||
        build_inputs.size() != 1U ||
        build_inputs.front().value("role", std::string{}) != "gltf-source-closure" ||
        !build_inputs.front().value("sourceClosureHash", std::string{}).starts_with("sha256:") ||
        build_inputs.front().value("externalDependencies", Json::array()).size() != 2U) {
        return fail(4, "Cook plan did not expose the external glTF importer and closure identity: " + plan.dump());
    }
    const auto recipe_hash = input.value("recipeHash", std::string{});
    if (recipe_hash.empty() || !recipe_hash.starts_with("sha256:")) {
        return fail(5, "Cook plan did not produce a content-addressed recipe hash");
    }

    const auto first_receipt = Json::parse(registry.apply_cook_plan_json(plan.dump(), false));
    if (!first_receipt.value("success", false) || first_receipt.value("cacheMisses", 0U) != 1U) {
        return fail(6, "first external glTF Cook did not commit one cache miss: " + first_receipt.dump());
    }
    const auto cache_hash = recipe_hash.substr(recipe_hash.find(':') + 1U);
    const auto payload_path = fixture.root / "generated" / "cook-cache" /
        cache_hash / "payload.meshbin";
    if (!std::filesystem::is_regular_file(payload_path) || std::filesystem::file_size(payload_path) == 0U) {
        return fail(7, "first Cook did not create a non-empty payload.meshbin: " + payload_path.string());
    }

    const auto cache_receipt = Json::parse(registry.apply_cook_plan_json(plan.dump(), false));
    if (!cache_receipt.value("success", false) || cache_receipt.value("cacheHits", 0U) != 1U ||
        cache_receipt.value("cacheMisses", 0U) != 0U) {
        return fail(8, "second identical external glTF Cook was not a cache hit: " + cache_receipt.dump());
    }

    const auto jpeg_plan = Json::parse(registry.cook_plan_json(
        {"texture.external-jpeg"}, "windows-x64-debug"));
    const auto jpeg_input = find_input(jpeg_plan, "texture.external-jpeg");
    if (!jpeg_plan.value("valid", false) ||
        !jpeg_input.value("importer", std::string{}).starts_with("image.jpeg-rgba8/1.0")) {
        return fail(12, "standalone JPEG did not select the unified image-to-KTX2 Cook path");
    }
    const auto jpeg_receipt = Json::parse(registry.apply_cook_plan_json(jpeg_plan.dump(), false));
    if (!jpeg_receipt.value("success", false) || jpeg_receipt.value("cacheMisses", 0U) != 1U ||
        jpeg_receipt.dump().find("payload.ktx2") == std::string::npos) {
        return fail(13, "standalone JPEG did not produce a KTX2 Cook artifact: " + jpeg_receipt.dump());
    }

    // Keep the mutated geometry valid while changing its identity. The old
    // plan must fail during deterministic regeneration before any payload work.
    std::vector<std::byte> changed_geometry;
    for (const float value : std::array{
             -0.5F, 0.0F, 0.0F,
              0.5F, 0.0F, 0.0F,
              0.0F, 1.25F, 0.0F}) {
        append_value(changed_geometry, value);
    }
    if (!write_bytes(fixture.geometry, changed_geometry)) {
        return fail(14, "could not mutate external geometry.bin");
    }
    const auto stale_receipt = Json::parse(registry.apply_cook_plan_json(plan.dump(), false));
    if (stale_receipt.value("success", false) ||
        stale_receipt.value("code", std::string{}) != "asset.cook-plan-integrity-error") {
        return fail(15, "old Cook plan was not rejected after external dependency mutation: " + stale_receipt.dump());
    }

    const auto changed_plan = Json::parse(registry.cook_plan_json(
        {"mesh.external-triangle"}, "windows-x64-debug"));
    const auto changed_input = find_input(changed_plan, "mesh.external-triangle");
    if (!changed_plan.value("valid", false) || changed_input.empty() ||
        changed_input.value("recipeHash", std::string{}) == recipe_hash ||
        changed_input.value("cacheUri", std::string{}) == input.value("cacheUri", std::string{})) {
        return fail(16, "new Cook plan did not change recipe/cache identity after external mutation");
    }

    std::cout << "gltf external Cook: inspect closure, JPEG/KTX2, plan identity, payload.meshbin, cache hit, "
                 "stale-plan rejection and dependency-driven identity change verified\n";
    return 0;
}
