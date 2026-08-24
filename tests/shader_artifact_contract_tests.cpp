#include "engine/content_hash.hpp"
#include "runtime/shader_artifact_contract.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Json=nlohmann::json;

void require(const bool condition,const std::string& message) {
    if(!condition)throw std::runtime_error(message);
}

void write_bytes(const std::filesystem::path& path,const std::vector<std::byte>& bytes) {
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output),"Unable to write shader contract fixture bytes.");
}

void write_json(const std::filesystem::path& path,const Json& value) {
    std::ofstream output(path,std::ios::binary);
    output<<value.dump(2);
    require(static_cast<bool>(output),"Unable to write shader contract fixture manifest.");
}

Json shader_entry(const std::string& hash,const std::size_t bytes) {
    return Json{
        {"stem","fixture.vert"},{"stage","vertex"},{"entrypoint","main"},
        {"resources",{{"uniformBuffers",1},{"samplers",0},{"storageBuffers",2},
            {"readonlyStorageBuffers",0},{"readwriteStorageBuffers",0}}},
        {"source",{{"path","fixture.vert.hlsl"},{"bytes",4},{"sha256",
            "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}},
        {"dxil",{{"path","fixture.vert.dxil"},{"bytes",bytes},{"sha256",hash}}},
        {"spv",{{"path","fixture.vert.spv"},{"bytes",bytes},{"sha256",hash}}}
    };
}

} // namespace

int main() {
    const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root=std::filesystem::temp_directory_path()/ ("noemancer-shader-contract-"+unique);
    try {
        std::filesystem::create_directories(root);
        const std::vector<std::byte> artifact{std::byte{0x44},std::byte{0x58},std::byte{0x49},std::byte{0x4c}};
        const auto identity=noemancer::sha256_bytes(std::span<const std::byte>(artifact));
        require(identity.success,"Unable to hash fixture artifact.");
        write_bytes(root/"fixture.vert.dxil",artifact);
        write_bytes(root/"fixture.vert.spv",artifact);
        const auto entry=shader_entry(identity.value,artifact.size());
        const Json manifest{{"schema","noemancer.shader-artifact-manifest/0.1"},
            {"shaders",Json::array({entry})}};
        write_json(root/"manifest.json",manifest);

        const noemancer::ShaderArtifactContract contract(root/"manifest.json");
        require(contract.valid(),std::string(contract.error_code())+": "+std::string(contract.error_detail()));
        const noemancer::ShaderArtifactRequest request{
            .stem="fixture.vert",.stage=noemancer::ShaderArtifactStage::vertex,
            .resources={.uniform_buffers=1,.storage_buffers=2}};
        const auto dxil=contract.load(request,noemancer::ShaderArtifactBackend::dxil);
        const auto spirv=contract.load(request,noemancer::ShaderArtifactBackend::spv);
        require(dxil.success&&spirv.success,"Both backend artifacts must load through the same contract.");
        require(dxil.bytes==artifact&&spirv.bytes==artifact&&dxil.artifact_hash==identity.value,
            "Loaded shader bytes and identities must match the manifest.");

        auto wrong=request;
        wrong.resources.uniform_buffers=2;
        const auto resource_failure=contract.load(wrong,noemancer::ShaderArtifactBackend::dxil);
        require(!resource_failure.success&&resource_failure.code=="shader-artifact.resource-mismatch",
            "Runtime binding drift must fail before SDL pipeline creation.");
        wrong=request;
        wrong.stage=noemancer::ShaderArtifactStage::fragment;
        const auto stage_failure=contract.load(wrong,noemancer::ShaderArtifactBackend::dxil);
        require(!stage_failure.success&&stage_failure.code=="shader-artifact.stage-mismatch",
            "Runtime stage drift must fail before SDL pipeline creation.");

        auto tampered=artifact;
        tampered.back()=std::byte{0x00};
        write_bytes(root/"fixture.vert.dxil",tampered);
        const auto hash_failure=contract.load(request,noemancer::ShaderArtifactBackend::dxil);
        require(!hash_failure.success&&hash_failure.code=="shader-artifact.artifact-hash-mismatch",
            "Artifact mutation after manifest parse must fail closed.");

        auto unsafe=manifest;
        unsafe["shaders"][0]["dxil"]["path"]="../fixture.vert.dxil";
        write_json(root/"unsafe.json",unsafe);
        const noemancer::ShaderArtifactContract unsafe_contract(root/"unsafe.json");
        require(!unsafe_contract.valid()&&unsafe_contract.error_code()=="shader-artifact.unsafe-path",
            "Manifest traversal must be rejected.");

        auto duplicate=manifest;
        duplicate["shaders"].push_back(entry);
        write_json(root/"duplicate.json",duplicate);
        const noemancer::ShaderArtifactContract duplicate_contract(root/"duplicate.json");
        require(!duplicate_contract.valid()&&duplicate_contract.error_code()=="shader-artifact.duplicate-stem",
            "Duplicate shader stems must be rejected.");
        std::filesystem::remove_all(root);
    } catch(const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root,ignored);
        std::cerr<<"shader_artifact_contract_tests: "<<error.what()<<'\n';
        return 1;
    }
    std::cout<<"shader_artifact_contract_tests: ok\n";
    return 0;
}
