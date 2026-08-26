#include "engine/gltf_mesh.hpp"
#include "engine/image_decoder.hpp"
#include "engine/simulation_runtime.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ExternalGltfFixture final {
    std::filesystem::path root;
    std::filesystem::path document;
    std::filesystem::path buffer;
    std::filesystem::path image;
};

template <typename T>
void append(std::vector<std::byte>& bytes, const T value) {
    const auto begin = bytes.size();
    bytes.resize(begin + sizeof(T));
    std::memcpy(bytes.data() + begin, &value, sizeof(T));
}

std::filesystem::path make_minimal_skinned_glb(const std::string& alpha_mode="MASK") {
    std::vector<std::byte> binary;
    for (const float value : std::array{-0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}) append(binary, value);
    for (const std::uint8_t value : std::array<std::uint8_t, 12>{0,0,0,0, 1,0,0,0, 1,0,0,0}) append(binary, value);
    for (const float value : std::array{1.0F,0.0F,0.0F,0.0F, 1.0F,0.0F,0.0F,0.0F, 1.0F,0.0F,0.0F,0.0F}) append(binary, value);
    for (const std::uint16_t value : std::array<std::uint16_t, 3>{0,1,2}) append(binary, value);
    while (binary.size() % 4U != 0U) append(binary, std::uint8_t{});
    for (int matrix = 0; matrix < 2; ++matrix) for (int component = 0; component < 16; ++component)
        append(binary, component % 5 == 0 ? 1.0F : 0.0F);
    append(binary, 0.0F); append(binary, 1.0F);
    for (const float value : std::array{0.0F,0.0F,0.0F,1.0F, 0.0F,0.0F,0.3826834F,0.9238795F}) append(binary, value);
    for (const float value : std::array{0.0F,0.0F, 1.0F,0.0F, 0.5F,1.0F}) append(binary, value);
    constexpr std::array<std::uint8_t,16> pixels{128U,128U,255U,255U, 255U,128U,64U,255U,
        64U,255U,128U,255U, 255U,255U,255U,128U};
    const auto png=noemancer::encode_png_rgba8(2U,2U,pixels);
    const auto png_offset=binary.size();
    binary.resize(binary.size()+png.bytes.size());
    std::memcpy(binary.data()+png_offset,png.bytes.data(),png.bytes.size());
    while (binary.size()%4U!=0U) append(binary,std::uint8_t{});

    using Json = nlohmann::json;
    Json document = {
        {"asset", {{"version", "2.0"}, {"generator", "noemancer.test"}}},
        {"buffers", {{{"byteLength", binary.size()}}}},
        {"bufferViews", {
            {{"buffer",0},{"byteOffset",0},{"byteLength",36}},
            {{"buffer",0},{"byteOffset",36},{"byteLength",12}},
            {{"buffer",0},{"byteOffset",48},{"byteLength",48}},
            {{"buffer",0},{"byteOffset",96},{"byteLength",6}},
            {{"buffer",0},{"byteOffset",104},{"byteLength",128}},
            {{"buffer",0},{"byteOffset",232},{"byteLength",8}},
            {{"buffer",0},{"byteOffset",240},{"byteLength",32}},
            {{"buffer",0},{"byteOffset",272},{"byteLength",24}},
            {{"buffer",0},{"byteOffset",png_offset},{"byteLength",png.bytes.size()}}
        }},
        {"accessors", {
            {{"bufferView",0},{"componentType",5126},{"count",3},{"type","VEC3"}},
            {{"bufferView",1},{"componentType",5121},{"count",3},{"type","VEC4"}},
            {{"bufferView",2},{"componentType",5126},{"count",3},{"type","VEC4"}},
            {{"bufferView",3},{"componentType",5123},{"count",3},{"type","SCALAR"}},
            {{"bufferView",4},{"componentType",5126},{"count",2},{"type","MAT4"}},
            {{"bufferView",5},{"componentType",5126},{"count",2},{"type","SCALAR"}},
            {{"bufferView",6},{"componentType",5126},{"count",2},{"type","VEC4"}},
            {{"bufferView",7},{"componentType",5126},{"count",3},{"type","VEC2"}}
        }},
        {"images", {{{"bufferView",8},{"mimeType","image/png"}}}},
        {"textures", {{{"source",0}}}},
        {"materials", {{{"name","CompletePbr"},{"pbrMetallicRoughness",{{"baseColorFactor",{0.8F,0.7F,0.6F,0.9F}},{"metallicFactor",0.35F},{"roughnessFactor",0.45F},
            {"baseColorTexture",{{"index",0}}},{"metallicRoughnessTexture",{{"index",0}}}}},
            {"normalTexture",{{"index",0},{"scale",0.75F}}},{"occlusionTexture",{{"index",0},{"strength",0.6F}}},
            {"emissiveTexture",{{"index",0}}},{"emissiveFactor",{0.1F,0.2F,0.3F}},{"alphaMode",alpha_mode},{"alphaCutoff",0.4F},{"doubleSided",true}}}},
        {"meshes", {{{"name","Triangle"},{"primitives", {{{"attributes",{{"POSITION",0},{"TEXCOORD_0",7},{"JOINTS_0",1},{"WEIGHTS_0",2}}},{"indices",3},{"material",0}}}}}}},
        {"nodes", {
            {{"name","Root"},{"children",{1}}},
            {{"name","Tip"},{"translation",{0.0F,0.5F,0.0F}}},
            {{"name","SkinnedTriangle"},{"mesh",0},{"skin",0}}
        }},
        {"skins", {{{"name","TestRig"},{"joints",{0,1}},{"inverseBindMatrices",4},{"skeleton",0}}}},
        {"animations", {{{"name","TipTurn"},
            {"samplers", {{{"input",5},{"output",6},{"interpolation","LINEAR"}}}},
            {"channels", {{{"sampler",0},{"target",{{"node",1},{"path","rotation"}}}}}}
        }}},
        {"scenes", {{{"nodes",{0,2}}}}},
        {"scene", 0}
    };
    std::string json = document.dump();
    while (json.size() % 4U != 0U) json.push_back(' ');
    std::vector<std::byte> glb;
    const auto total_length = static_cast<std::uint32_t>(12U + 8U + json.size() + 8U + binary.size());
    append(glb, std::uint32_t{0x46546c67U}); append(glb, std::uint32_t{2U}); append(glb, total_length);
    append(glb, static_cast<std::uint32_t>(json.size())); append(glb, std::uint32_t{0x4e4f534aU});
    const auto json_begin = glb.size(); glb.resize(json_begin + json.size()); std::memcpy(glb.data() + json_begin, json.data(), json.size());
    append(glb, static_cast<std::uint32_t>(binary.size())); append(glb, std::uint32_t{0x004e4942U});
    glb.insert(glb.end(), binary.begin(), binary.end());
    const auto path = std::filesystem::temp_directory_path() / ("noemancer-minimal-skinned-"+alpha_mode+".glb");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    return path;
}

void write_text(const std::filesystem::path& path,const std::string& value){
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(value.data(),static_cast<std::streamsize>(value.size()));
}

std::vector<std::byte> decode_base64_fixture(const std::string_view source){
    constexpr std::string_view alphabet=
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int,256> table{};table.fill(-1);
    for(std::size_t index=0;index<alphabet.size();++index)
        table[static_cast<unsigned char>(alphabet[index])]=static_cast<int>(index);
    std::vector<std::byte> output;std::uint32_t accumulator{};int bits{};
    for(const unsigned char value:source){
        if(value=='=')break;
        const int decoded=table[value];if(decoded<0)continue;
        accumulator=(accumulator<<6U)|static_cast<std::uint32_t>(decoded);bits+=6;
        if(bits>=8){bits-=8;output.push_back(static_cast<std::byte>((accumulator>>bits)&0xffU));}
    }
    return output;
}

ExternalGltfFixture make_external_gltf_fixture(const bool jpeg=false){
    ExternalGltfFixture fixture;
    fixture.root=std::filesystem::temp_directory_path()/
        (jpeg?"noemancer-gltf-external-jpeg-tests":"noemancer-gltf-external-snapshot-tests");
    fixture.document=fixture.root/"scene.gltf";
    fixture.buffer=fixture.root/"geometry"/"mesh.bin";
    fixture.image=fixture.root/"textures"/(jpeg?"base.jpg":"base.png");
    std::error_code ignored;std::filesystem::remove_all(fixture.root,ignored);
    std::filesystem::create_directories(fixture.buffer.parent_path());
    std::filesystem::create_directories(fixture.image.parent_path());
    std::vector<std::byte> geometry;
    for(const float value:std::array{-0.5F,0.0F,0.0F,0.5F,0.0F,0.0F,0.0F,1.0F,0.0F})
        append(geometry,value);
    {std::ofstream output(fixture.buffer,std::ios::binary|std::ios::trunc);
     output.write(reinterpret_cast<const char*>(geometry.data()),static_cast<std::streamsize>(geometry.size()));}
    std::vector<std::byte> image_bytes;
    if(jpeg){
        constexpr std::string_view encoded=
            "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAIDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD7r/Zv8B+GtS/Z4+F13d+HdJuru48LaXLNPPYxPJI7WkRZmYrkkkkknrmiiivx3H/73W/xS/Nn4Fmf+/V/8cvzZ//Z";
        image_bytes=decode_base64_fixture(encoded);
    }else{
        constexpr std::array<std::uint8_t,16> pixels{
            255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
        const auto png=noemancer::encode_png_rgba8(2U,2U,pixels);
        image_bytes.resize(png.bytes.size());
        std::memcpy(image_bytes.data(),png.bytes.data(),png.bytes.size());
    }
    {std::ofstream output(fixture.image,std::ios::binary|std::ios::trunc);
     output.write(reinterpret_cast<const char*>(image_bytes.data()),static_cast<std::streamsize>(image_bytes.size()));}
    const nlohmann::json document={
        {"asset",{{"version","2.0"}}},
        {"buffers",{{{"uri","geometry/mesh.bin"},{"byteLength",geometry.size()}}}},
        {"bufferViews",{{{"buffer",0},{"byteOffset",0},{"byteLength",geometry.size()}}}},
        {"accessors",{{{"bufferView",0},{"componentType",5126},{"count",3},{"type","VEC3"}}}},
        // URI-backed images are allowed to omit image.mimeType in glTF 2.0;
        // the importer must recognize the bounded dependency by file signature.
        {"images",{{{"uri",jpeg?"textures/base.jpg":"textures/base.png"}}}},
        {"textures",{{{"source",0}}}},
        {"materials",{{{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}}}}}}},
        {"meshes",{{{"primitives",{{{"attributes",{{"POSITION",0}}},{"material",0}}}}}}},
        {"nodes",{{{"mesh",0}}}},
        {"scenes",{{{"nodes",{0}}}}},
        {"scene",0}
    };
    write_text(fixture.document,document.dump());return fixture;
}

std::filesystem::path write_uri_fixture(const std::filesystem::path& root,
                                        const std::string& name,
                                        const std::string& uri){
    const auto path=root/name;
    write_text(path,nlohmann::json{{"asset",{{"version","2.0"}}},
        {"buffers",{{{"uri",uri},{"byteLength",4}}}}}.dump());
    return path;
}

} // namespace

int main() {
    std::error_code ignored;
    const auto external=make_external_gltf_fixture();
    const auto snapshot=noemancer::read_gltf_source_snapshot(external.document);
    if(!snapshot.valid||snapshot.dependencies.size()!=2U||
       snapshot.dependencies[0].normalized_relative_path!="geometry/mesh.bin"||
       snapshot.dependencies[1].normalized_relative_path!="textures/base.png"||
       !snapshot.content_hash.starts_with("sha256:")||
       snapshot.total_bytes!=snapshot.source_bytes+
           snapshot.dependencies[0].source_bytes+snapshot.dependencies[1].source_bytes){
        std::cerr<<snapshot.code<<": "<<snapshot.detail<<'\n';return 15;
    }
    if(!noemancer::verify_gltf_source_snapshot(snapshot).unchanged)return 16;
    const auto external_decoded=noemancer::decode_gltf_mesh(external.document);
    if(!external_decoded.valid||external_decoded.vertices.size()!=3U||
       external_decoded.indices.size()!=3U||external_decoded.primitives.size()!=1U||
       external_decoded.images.size()!=1U||!external_decoded.images[0].valid||
       external_decoded.images[0].mime_type!="image/png"||
       external_decoded.primitives[0].base_color_image!=0){
        std::cerr<<"Immutable external JSON glTF snapshot did not decode through fastgltf: "
                 <<external_decoded.code<<": "<<external_decoded.detail<<'\n';
        std::filesystem::remove_all(external.root,ignored);return 17;
    }
    const auto external_jpeg=make_external_gltf_fixture(true);
    const auto jpeg_snapshot=noemancer::read_gltf_source_snapshot(external_jpeg.document);
    const auto jpeg_decoded=noemancer::decode_gltf_mesh(external_jpeg.document);
    if(!jpeg_snapshot.valid||jpeg_snapshot.dependencies.size()!=2U||
       !jpeg_decoded.valid||jpeg_decoded.images.size()!=1U||!jpeg_decoded.images[0].valid||
       jpeg_decoded.images[0].mime_type!="image/jpeg"||jpeg_decoded.images[0].width!=2U||
       jpeg_decoded.images[0].height!=2U||jpeg_decoded.images[0].rgba8.size()!=16U||
       jpeg_decoded.primitives[0].base_color_image!=0){
        std::cerr<<"External JPEG glTF did not decode through libjpeg-turbo: "
                 <<jpeg_decoded.code<<": "<<jpeg_decoded.detail<<'\n';
        std::filesystem::remove_all(external.root,ignored);
        std::filesystem::remove_all(external_jpeg.root,ignored);return 21;
    }
    const std::array<std::string,4> unsafe_uris{
        "../outside.bin","%2e%2e/outside.bin","C:/outside.bin","https://invalid/mesh.bin"};
    for(std::size_t index=0;index<unsafe_uris.size();++index){
        const auto rejected=noemancer::read_gltf_source_snapshot(
            write_uri_fixture(external.root,"unsafe-"+std::to_string(index)+".gltf",unsafe_uris[index]));
        if(rejected.valid||rejected.code!="gltf.external-uri-unsafe")return 18;
    }
    noemancer::GltfSourceSnapshotLimits small;small.maximum_dependency_bytes=1U;
    if(noemancer::read_gltf_source_snapshot(external.document,small).code!=
       "gltf.dependency-budget-exceeded")return 19;
    {std::fstream changed(external.buffer,std::ios::binary|std::ios::in|std::ios::out);
     const char byte='\x01';changed.write(&byte,1);}
    const auto changed=noemancer::verify_gltf_source_snapshot(snapshot);
    if(changed.unchanged||changed.normalized_relative_path!="geometry/mesh.bin")return 20;
    std::filesystem::remove_all(external.root,ignored);
    std::filesystem::remove_all(external_jpeg.root,ignored);

    const auto path = make_minimal_skinned_glb();
    const auto container = noemancer::read_glb_container(path);
    if (!container.valid || container.version != 2U || container.source_bytes == 0U ||
        container.json_chunk().empty() || container.binary_chunk().empty() ||
        container.detail.find("validated") == std::string::npos) {
        std::cerr << "GLB container boundary did not preserve validated JSON and binary payloads\n";
        return 11;
    }
    const auto decoded = noemancer::decode_glb_mesh(path);
    std::filesystem::remove(path, ignored);
    if (!decoded.valid || decoded.detail.find("fastgltf") == std::string::npos) {
        std::cerr << decoded.code << ": " << decoded.detail << '\n';
        return 1;
    }
    const auto metadata_path = std::filesystem::temp_directory_path() / "noemancer-metadata-only-glb.glb";
    {
        std::string json = nlohmann::json{{"asset", {{"version", "2.0"}}}}.dump();
        while (json.size() % 4U != 0U) json.push_back(' ');
        std::vector<std::byte> metadata_glb;
        append(metadata_glb, std::uint32_t{0x46546c67U});
        append(metadata_glb, std::uint32_t{2U});
        append(metadata_glb, static_cast<std::uint32_t>(12U + 8U + json.size()));
        append(metadata_glb, static_cast<std::uint32_t>(json.size()));
        append(metadata_glb, std::uint32_t{0x4e4f534aU});
        const auto json_begin = metadata_glb.size();
        metadata_glb.resize(json_begin + json.size());
        std::memcpy(metadata_glb.data() + json_begin, json.data(), json.size());
        std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(metadata_glb.data()), static_cast<std::streamsize>(metadata_glb.size()));
    }
    const auto metadata_container = noemancer::read_glb_container(metadata_path);
    const auto metadata_mesh = noemancer::decode_glb_mesh(metadata_path);
    std::filesystem::remove(metadata_path, ignored);
    if (!metadata_container.valid || metadata_container.has_binary_chunk || !metadata_container.binary_chunk().empty() ||
        metadata_mesh.valid || metadata_mesh.code != "gltf.missing-binary-chunk") {
        std::cerr << "GLB container did not distinguish optional BIN metadata from mesh decoding\n";
        return 13;
    }
    const auto truncated_path = std::filesystem::temp_directory_path() / "noemancer-truncated-glb.glb";
    {
        std::ofstream output(truncated_path, std::ios::binary | std::ios::trunc);
        const std::array<std::uint8_t, 12> truncated{0x67U, 0x6cU, 0x54U, 0x46U, 0x02U, 0x00U, 0x00U, 0x00U,
            0x20U, 0x00U, 0x00U, 0x00U};
        output.write(reinterpret_cast<const char*>(truncated.data()), static_cast<std::streamsize>(truncated.size()));
    }
    const auto rejected_container = noemancer::read_glb_container(truncated_path);
    std::filesystem::remove(truncated_path, ignored);
    if (rejected_container.valid || rejected_container.code != "gltf.invalid-header") {
        std::cerr << "GLB container boundary accepted a truncated payload\n";
        return 12;
    }
    const auto blend_path=make_minimal_skinned_glb("BLEND");
    const auto blend_decoded=noemancer::decode_glb_mesh(blend_path);
    std::filesystem::remove(blend_path,ignored);
    if (!blend_decoded.valid || blend_decoded.primitives.size()!=1U || blend_decoded.primitives[0].alpha_mode!="BLEND") {
        std::cerr << "glTF Alpha Blend material was not preserved\n";
        return 7;
    }
    if (decoded.vertices.size() != 3U || decoded.primitives.size() != 1U || decoded.primitives[0].skin != 0 ||
        decoded.vertices[1].joints[0] != 1U || decoded.vertices[1].weights[0] != 1.0F ||
        std::abs(decoded.vertices[0].tangent[0]) < 0.5F || decoded.primitives[0].alpha_mode != "MASK" ||
        !decoded.primitives[0].double_sided || decoded.primitives[0].emissive_factor[1] != 0.2F ||
        decoded.images.size()!=1U || !decoded.images[0].valid || decoded.primitives[0].normal_image!=0 ||
        decoded.primitives[0].metallic_roughness_image!=0 || decoded.primitives[0].occlusion_image!=0 ||
        decoded.primitives[0].emissive_image!=0 || decoded.primitives[0].bounds_radius<=0.0F) {
        std::cerr << "Skinned vertex payload was not decoded deterministically\n";
        return 2;
    }
    if (decoded.skins.size() != 1U || decoded.skins[0].joints.size() != 2U ||
        decoded.skins[0].joints[1].parent_joint != 0 || decoded.skins[0].joints[1].local_transform[13] != 0.5F) {
        std::cerr << "Skin hierarchy or bind data was not decoded\n";
        return 3;
    }
    if (decoded.animations.size() != 1U || decoded.animations[0].channels.size() != 1U ||
        decoded.animations[0].duration != 1.0F || decoded.animations[0].channels[0].path != "rotation" ||
        decoded.animations[0].channels[0].values.size() != 2U) {
        std::cerr << "Animation sampler/channel data was not decoded\n";
        return 4;
    }
    auto runtime_source = decoded;
    runtime_source.animations[0].channels.push_back(noemancer::GltfAnimationChannel{
        .node_index = 0U, .path = "translation", .interpolation = "LINEAR", .times = {0.0F, 1.0F},
        .values = {{{0.0F, 0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 0.0F}}}});
    auto return_clip = runtime_source.animations[0];
    return_clip.name = "TipReturn";
    return_clip.channels.back().values = {{{0.0F, 0.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F, 0.0F}}};
    runtime_source.animations.push_back(std::move(return_clip));
    noemancer::AnimationRuntime runtime;
    const auto compiled = runtime.compile_gltf_asset("asset.test.skinned-triangle", runtime_source, 0U, 0U);
    const auto baseline = runtime.compile_gltf_asset("asset.test.skinned-triangle-baseline", runtime_source, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_runtime_baseline);
    const auto optimized = runtime.compile_gltf_asset("asset.test.skinned-triangle-optimized", runtime_source, 0U, 0U,
        noemancer::AnimationCompressionMode::ozz_hierarchical_key_reduction);
    const auto compiled_return = runtime.compile_gltf_asset("asset.test.skinned-triangle", runtime_source, 0U, 1U);
    const auto compression_comparison = runtime.compare_compiled_clips(baseline.clip_asset, optimized.clip_asset, 257U);
    const auto pose_start = runtime.sample_skeletal_pose(compiled.clip_asset, 0.0F);
    const auto pose_end = runtime.sample_skeletal_pose(compiled.clip_asset, 1.0F);
    if (!compiled.success || compiled.joint_count != 2U || runtime.duration(compiled.clip_asset) != 1.0F ||
        !pose_start.valid || !pose_end.valid || pose_end.skinning_matrices.size() != 2U ||
        pose_start.skinning_matrices[1][1] == pose_end.skinning_matrices[1][1]) {
        std::cerr << "Validated glTF animation did not compile and sample through ozz\n";
        return 5;
    }
    if (!baseline.success || !optimized.success || !optimized.compression.optimizer_attempted ||
        !optimized.compression.optimizer_applied || optimized.compression.fallback_used ||
        optimized.compression.input_raw_archive_bytes == 0U ||
        optimized.compression.baseline_runtime_archive_bytes == 0U ||
        optimized.compression.selected_runtime_archive_bytes == 0U || optimized.compression.skeleton_archive_bytes == 0U ||
        !optimized.compression.input_raw_archive_hash.starts_with("fnv1a64:") ||
        !optimized.compression.selected_runtime_archive_hash.starts_with("fnv1a64:") ||
        baseline.compression.selected_runtime_archive_hash != optimized.compression.baseline_runtime_archive_hash ||
        !compression_comparison.success || compression_comparison.sample_count != 257U ||
        compression_comparison.maximum_local_translation_error_meters > 0.001F ||
        compression_comparison.maximum_local_rotation_error_degrees > 0.02F ||
        compression_comparison.maximum_local_scale_error > 0.001F ||
        compression_comparison.maximum_model_translation_error_meters > 0.001F ||
        compression_comparison.maximum_model_probe_error_meters > 0.0011F ||
        compression_comparison.maximum_skinning_matrix_absolute_error > 0.001F ||
        compression_comparison.maximum_root_motion_delta_error_meters > 0.001F) {
        std::cerr << "ozz optimization did not produce deterministic size/hash/error evidence: "
                  << "baseline=" << baseline.success
                  << " attempted=" << optimized.compression.optimizer_attempted
                  << " applied=" << optimized.compression.optimizer_applied
                  << " fallback=" << optimized.compression.fallback_used
                  << " rawArchive=" << optimized.compression.input_raw_archive_bytes
                  << " baselineArchive=" << optimized.compression.baseline_runtime_archive_bytes
                  << " selectedArchive=" << optimized.compression.selected_runtime_archive_bytes
                  << " skeletonArchive=" << optimized.compression.skeleton_archive_bytes
                  << " baselineHash=" << baseline.compression.selected_runtime_archive_hash
                  << " expectedBaselineHash=" << optimized.compression.baseline_runtime_archive_hash
                  << " comparison=" << compression_comparison.success
                  << " localT=" << compression_comparison.maximum_local_translation_error_meters
                  << " localR=" << compression_comparison.maximum_local_rotation_error_degrees
                  << " localS=" << compression_comparison.maximum_local_scale_error
                  << " modelT=" << compression_comparison.maximum_model_translation_error_meters
                  << " modelProbe=" << compression_comparison.maximum_model_probe_error_meters
                  << " skin=" << compression_comparison.maximum_skinning_matrix_absolute_error
                  << " root=" << compression_comparison.maximum_root_motion_delta_error_meters << '\n';
        return 14;
    }
    const auto root_motion = runtime.root_motion_delta(compiled.clip_asset, 0.75F, 0.25F, true, 1.0F);
    const auto blended = runtime.sample_blended_skeletal_pose(compiled.clip_asset, 0.5F,
        compiled_return.clip_asset, 0.5F, 0.5F);
    if (!compiled_return.success || !root_motion.valid || std::abs(root_motion.x - 0.5F) > 0.001F ||
        !blended.valid || blended.joints.size() != 2U || blended.joints[0].name != "Root" || blended.joints[1].parent != 0) {
        std::cerr << "Root motion, cross-fade, or skeleton debug evidence is invalid\n";
        return 8;
    }
    noemancer::World world;
    const auto registered = world.register_gltf_animations("asset.test.skinned-triangle", runtime_source);
    noemancer::SceneDocument scene{
        .scene_guid = "scene.gltf-animation-test",
        .name = "glTF Animation Test",
        .entities = {noemancer::SceneEntityDocument{
            .guid = "entity.skinned",
            .name = "Skinned",
            .transform = noemancer::SceneTransform{{0.0, 0.0, 0.0}},
            .animation_player = noemancer::SceneAnimationPlayer{compiled.clip_asset, 1.0, true, true,
                compiled_return.clip_asset, 0.4, "apply"},
            .mesh_renderer = noemancer::SceneMeshRenderer{"asset.test.skinned-triangle", true, true, true}
        }}
    };
    if (registered.size() != 2U || !registered[0].success || !registered[1].success || !world.load_scene(scene).success) {
        std::cerr << "World animation asset registration failed\n";
        return 6;
    }
    world.tick(0.2F);
    const auto views = world.entity_views();
    if (views.size() != 1U || !views[0].skeletal_pose || views[0].skeletal_pose->skinning_matrices.size() != 2U ||
        !views[0].animation_player || views[0].animation_player->next_clip_asset.empty() ||
        std::abs(views[0].transform->x) > 0.001F ||
        world.animation_observation_json().find("debugEvidence") == std::string::npos ||
        world.animation_skeleton_json("entity.skinned", 1U).find(R"("truncated":true)") == std::string::npos) {
        std::cerr << "Registered glTF pose, blended root motion, or bounded skeleton evidence is invalid\n";
        return 9;
    }
    world.tick(0.3F);
    if (world.entity_views()[0].animation_player->clip_asset != compiled_return.clip_asset ||
        !world.entity_views()[0].animation_player->next_clip_asset.empty()) {
        std::cerr << "Cross-fade did not commit the target clip deterministically\n";
        return 10;
    }
    return 0;
}
