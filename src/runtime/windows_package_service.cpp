#include "runtime/windows_package_service.hpp"

#include "engine/asset_registry.hpp"
#include "engine/content_hash.hpp"
#include "engine/mesh_runtime_artifact.hpp"
#include "engine/package_pipeline.hpp"
#include "engine/project_document.hpp"
#include "engine/scripting_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <thread>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kCookManifestSchema = "noemancer.cook-manifest/0.1";
constexpr std::size_t kCookManifestMaxOutputs = 8192U;
constexpr std::size_t kCookManifestMaxMetadataText = 4096U;
constexpr std::size_t kCookManifestMaxIdentifierText = 256U;
constexpr std::size_t kCookManifestMaxMetadataItems = 512U;

std::string path_text(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string slug(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '.' || character == '_' || character == '-')
            result.push_back(character);
        else
            result.push_back('_');
    }
    if (result.empty()) result = "game";
    return result;
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool safe_relative_path(const std::filesystem::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name() || value.has_root_directory())
        return false;
    const auto normalized = value.lexically_normal();
    if (normalized == "." || normalized.empty()) return false;
    return normalized.begin() == normalized.end() || *normalized.begin() != "..";
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

struct FileHash final {
    bool available{};
    std::uintmax_t bytes{};
    std::string hash;
};

FileHash hash_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return {};
    const auto identity=sha256_file(path);
    return identity.success?FileHash{true,identity.bytes,identity.value}:FileHash{};
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::vector<std::byte> read_bounded_binary(const std::filesystem::path& path,
                                           const std::uintmax_t maximum_bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > maximum_bytes ||
        size > std::numeric_limits<std::size_t>::max()) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) return {};
    return bytes;
}

std::optional<std::string> read_notice(const std::filesystem::path& source,
                                       const std::filesystem::path& project_root) {
    std::vector<std::filesystem::path> directories;
    auto directory = source.parent_path().lexically_normal();
    const auto root = project_root.lexically_normal();
    while (!directory.empty()) {
        directories.push_back(directory);
        if (directory == root) break;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    for (const auto& candidate_directory : directories) {
        for (const auto& filename : {"LICENSE", "LICENSE.md", "LICENSE.txt", "NOTICE", "NOTICE.md", "NOTICE.txt"}) {
            const auto candidate = candidate_directory / filename;
            if (const auto contents = read_text(candidate); contents && !contents->empty()) return contents;
        }
    }
    return std::nullopt;
}

bool is_project_owned(std::string_view license) {
    auto value = lower(std::string(license));
    return value.find("noemancer") != std::string::npos ||
        value.find("project") != std::string::npos ||
        value.find("built-in") != std::string::npos ||
        value.find("builtin") != std::string::npos;
}

struct ServiceError final {
    std::string code;
    std::string detail;
};

bool bounded_manifest_text(const Json& value, const std::size_t maximum,
                           const bool allow_empty, std::string& output) {
    if (!value.is_string()) return false;
    output = value.get<std::string>();
    if ((!allow_empty && output.empty()) || output.size() > maximum) return false;
    for (const auto character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == 0U || std::iscntrl(byte)) return false;
    }
    return true;
}

Json error_json(const ServiceError& error) {
    return Json{
        {"schema", "noemancer.windows-package/0.1"},
        {"success", false},
        {"code", error.code},
        {"detail", error.detail},
        {"plan", nullptr},
        {"receipt", nullptr}
    };
}

std::filesystem::path payload_path(const std::filesystem::path& project_root,
                                    const std::string_view uri, ServiceError& error) {
    constexpr std::string_view prefix = "generated://";
    if (!uri.starts_with(prefix)) {
        error = {"package.payload-uri-invalid", "Cook payload URI must use generated://."};
        return {};
    }
    const auto relative = std::filesystem::path(std::string(uri.substr(prefix.size()))).lexically_normal();
    if (!safe_relative_path(relative)) {
        error = {"package.payload-path-escape", "Cook payload URI resolves outside the generated directory."};
        return {};
    }
    return (project_root / "generated" / relative).lexically_normal();
}

std::optional<Json> find_cook_manifest(const std::filesystem::path& generated_root,
                                       const std::string_view target_profile,
                                       const std::string_view plan_id,ServiceError& error) {
    const auto text=read_text(generated_root/"cook-manifests"/(std::string(plan_id)+".json"));
    if(!text) {error={"package.cook-manifest-missing","The current deterministic Cook plan has no committed manifest."};return std::nullopt;}
    const auto parsed=Json::parse(*text,nullptr,false);
    if(parsed.is_discarded()||!parsed.is_object()||parsed.value("schema",std::string{})!=kCookManifestSchema||
       parsed.value("targetProfile",std::string{})!=target_profile||parsed.value("planId",std::string{})!=plan_id) {
        error={"package.cook-manifest-invalid","The committed Cook manifest does not match the current deterministic plan."};return std::nullopt;
    }
    return parsed;
}

PackageFileDescriptor file_descriptor(std::string id, std::filesystem::path source,
                                      std::string license, bool required = true) {
    return PackageFileDescriptor{
        .id = std::move(id),
        .source_path = std::move(source),
        .license_id = std::move(license),
        .available = true,
        .required = required
    };
}

void add_license(std::map<std::string, PackageLicenseDescriptor>& licenses,
                 const AssetRecord* asset, const std::filesystem::path& project_root) {
    if (asset == nullptr) return;
    const auto id = asset->license.empty() ? std::string("unknown") : asset->license;
    if (licenses.contains(id)) return;
    const bool project_owned = is_project_owned(id);
    const auto source = asset->relative_path.empty() ? std::filesystem::path{} :
        std::filesystem::path(asset->source_root) / asset->relative_path;
    const auto notice = project_owned
        ? std::string("Project-owned or engine-provided content; no third-party notice is required.")
        : read_notice(source, project_root).value_or(std::string{});
    licenses.emplace(id, PackageLicenseDescriptor{
        .id = id,
        .name = id,
        .spdx_id = id,
        .notice = notice,
        .source_uri = asset->uri,
        .third_party = !project_owned,
        // project-only prevents extraction/reuse as a public library asset;
        // project-owned content is still distributable inside this game's package.
        .redistributable = project_owned || asset->redistribution == "public"
    });
}

std::vector<PackageFileDescriptor> scan_runtime_support(const std::filesystem::path& directory,
                                                         std::string license, const std::filesystem::path& skip) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return {};
    for (std::filesystem::directory_iterator iterator(directory, error), end; iterator != end; iterator.increment(error)) {
        if (error) break;
        if (!iterator->is_regular_file(error) || iterator->path() == skip) continue;
        if (lower(iterator->path().extension().string()) == ".dll") files.push_back(iterator->path());
    }
    std::ranges::sort(files, [](const auto& left, const auto& right) {
        return path_text(left.filename()) < path_text(right.filename());
    });
    std::vector<PackageFileDescriptor> result;
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto filename=lower(files[index].filename().string());
        result.push_back(file_descriptor("runtime.support." + std::to_string(index),files[index],
            filename.starts_with("icu")?"unicode-icu":license));
    }
    return result;
}

std::vector<PackageFileDescriptor> scan_shader_support(const std::filesystem::path& runtime) {
    const auto shader_root=(runtime.parent_path().parent_path()/"shaders").lexically_normal();
    const auto manifest_path=shader_root/"shader-artifact-manifest.json";
    const auto reflection_path=shader_root/"shader-artifact-reflection.json";
    const auto manifest_text=read_text(manifest_path);
    if(!manifest_text)return {};
    const auto manifest=Json::parse(*manifest_text,nullptr,false);
    if(manifest.is_discarded()||!manifest.is_object()||
       manifest.value("schema",std::string{})!="noemancer.shader-artifact-manifest/0.1"||
       !manifest.contains("shaders")||!manifest.at("shaders").is_array()||
       !manifest.contains("shaderCount")||!manifest.at("shaderCount").is_number_unsigned()||
       manifest.at("shaderCount").get<std::size_t>()!=manifest.at("shaders").size()||
       manifest.at("shaders").empty()||manifest.at("shaders").size()>256U)
        return {};

    // Distribution follows the already verified manifest, never a directory
    // glob.  Build trees can retain obsolete binaries across source-contract
    // revisions; staging one would create a package ABI that no authority
    // declared or reflected.
    std::vector<std::filesystem::path> files{manifest_path,reflection_path};
    std::set<std::string> declared_paths{
        path_text(manifest_path.filename()),path_text(reflection_path.filename())};
    for(const auto& shader:manifest.at("shaders")) {
        if(!shader.is_object())return {};
        for(const auto* backend:{"dxil","spv"}) {
            if(!shader.contains(backend)||!shader.at(backend).is_object()||
               !shader.at(backend).contains("path")||!shader.at(backend).at("path").is_string())
                return {};
            const auto relative=std::filesystem::path(shader.at(backend).at("path").get<std::string>()).lexically_normal();
            if(!safe_relative_path(relative)||relative.has_parent_path())return {};
            const auto extension=lower(relative.extension().string());
            if((std::string_view(backend)=="dxil"&&extension!=".dxil")||
               (std::string_view(backend)=="spv"&&extension!=".spv"))return {};
            if(!declared_paths.insert(path_text(relative)).second)return {};
            files.push_back((shader_root/relative).lexically_normal());
        }
    }
    std::error_code error;
    for(const auto& file:files)
        if(!std::filesystem::is_regular_file(file,error)||error)return {};
    std::ranges::sort(files,[](const auto& left,const auto& right) {
        return path_text(left.filename())<path_text(right.filename());
    });
    std::vector<PackageFileDescriptor> result;
    result.reserve(files.size());
    for(std::size_t index=0;index<files.size();++index) {
        auto descriptor=file_descriptor("runtime.shader."+std::to_string(index),files[index],"noemancer-runtime");
        descriptor.staging_path=std::filesystem::path("shaders")/files[index].filename();
        result.push_back(std::move(descriptor));
    }
    return result;
}

std::optional<std::filesystem::path> find_generated_license_path(
    const std::filesystem::path& runtime, const std::string_view filename) {
    auto ancestor=std::filesystem::absolute(runtime).parent_path();
    for(int depth=0;depth<8&&!ancestor.empty();++depth,ancestor=ancestor.parent_path())
        if(const auto candidate=ancestor/"generated/licenses"/filename;
            std::filesystem::is_regular_file(candidate))return candidate;
    return std::nullopt;
}

std::optional<std::string> find_generated_license(const std::filesystem::path& runtime,const std::string_view filename) {
    const auto path=find_generated_license_path(runtime,filename);
    if(!path)return std::nullopt;
    const auto text=read_text(*path);
    return text&&!text->empty()?text:std::nullopt;
}

struct RuntimeLicenseSpec final {
    std::string_view id;
    std::string_view name;
    std::string_view spdx_id;
    std::string_view filename;
    std::string_view source_uri;
};

constexpr std::array kRuntimeLicenseSpecs{
    RuntimeLicenseSpec{"noemancer-runtime", "Noemancer Runtime", "Apache-2.0", "Noemancer-LICENSE.txt", ""},
    RuntimeLicenseSpec{"sdl", "SDL", "Zlib", "SDL-LICENSE.txt", "https://github.com/libsdl-org/SDL"},
    RuntimeLicenseSpec{"flecs", "Flecs", "MIT", "flecs-LICENSE.txt", "https://github.com/SanderMertens/flecs"},
    RuntimeLicenseSpec{"nlohmann-json", "JSON for Modern C++", "MIT", "nlohmann-json-LICENSE.txt", "https://github.com/nlohmann/json"},
    RuntimeLicenseSpec{"dear-imgui", "Dear ImGui", "MIT", "Dear-ImGui-LICENSE.txt", "https://github.com/ocornut/imgui"},
    RuntimeLicenseSpec{"imguizmo", "ImGuizmo", "MIT", "ImGuizmo-LICENSE.txt", "https://github.com/CedricGuillemet/ImGuizmo"},
    RuntimeLicenseSpec{"lodepng", "LodePNG", "Zlib", "lodepng-LICENSE.txt", "https://github.com/lvandeve/lodepng"},
    RuntimeLicenseSpec{"libjpeg-turbo", "libjpeg-turbo", "LicenseRef-libjpeg-turbo-composite", "libjpeg-turbo-LICENSE.md", "https://github.com/libjpeg-turbo/libjpeg-turbo"},
    RuntimeLicenseSpec{"miniaudio", "miniaudio", "MIT", "miniaudio-LICENSE.txt", "https://github.com/mackron/miniaudio"},
    RuntimeLicenseSpec{"jolt-physics", "Jolt Physics", "MIT", "Jolt-Physics-LICENSE.txt", "https://github.com/jrouwe/JoltPhysics"},
    RuntimeLicenseSpec{"freetype", "FreeType", "FTL", "FreeType-LICENSE.txt", "https://gitlab.freedesktop.org/freetype/freetype"},
    RuntimeLicenseSpec{"rmlui", "RmlUi", "MIT", "RmlUi-LICENSE.txt", "https://github.com/mikke89/RmlUi"},
    RuntimeLicenseSpec{"lato-font", "Lato Font", "OFL-1.1", "Lato-Font-LICENSE.txt", "https://www.latofonts.com"},
    RuntimeLicenseSpec{"harfbuzz", "HarfBuzz", "MIT", "HarfBuzz-COPYING.txt", "https://github.com/harfbuzz/harfbuzz"},
    RuntimeLicenseSpec{"unicode-icu", "Unicode ICU", "Unicode-3.0", "ICU-LICENSE.txt", "https://github.com/unicode-org/icu"},
    RuntimeLicenseSpec{"ozz-animation", "ozz-animation", "MIT", "ozz-animation-LICENSE.md", "https://github.com/guillaumeblanc/ozz-animation"},
    RuntimeLicenseSpec{"ufbx", "ufbx", "MIT", "ufbx-LICENSE.txt", "https://github.com/ufbx/ufbx"},
    RuntimeLicenseSpec{"fastgltf", "fastgltf", "MIT", "fastgltf-LICENSE.md", "https://github.com/spnda/fastgltf"},
    RuntimeLicenseSpec{"meshoptimizer", "meshoptimizer", "MIT", "meshoptimizer-LICENSE.md", "https://github.com/zeux/meshoptimizer"},
    RuntimeLicenseSpec{"ktx-software", "KTX-Software", "Apache-2.0", "KTX-Software-LICENSE.md", "https://github.com/KhronosGroup/KTX-Software"}
};

std::vector<PackageFileDescriptor> scan_managed_support(const std::filesystem::path& directory,
                                                        const std::filesystem::path& skip,
                                                        std::string license) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return {};
    for (std::filesystem::directory_iterator iterator(directory, error), end; iterator != end; iterator.increment(error)) {
        if (error) break;
        if (!iterator->is_regular_file(error) || iterator->path() == skip) continue;
        const auto filename = lower(iterator->path().filename().string());
        if(filename=="noemancer.managed.dll")continue;
        if (filename.ends_with(".deps.json") || filename.ends_with(".runtimeconfig.json") ||
            lower(iterator->path().extension().string()) == ".dll" ||
            lower(iterator->path().extension().string()) == ".pdb") files.push_back(iterator->path());
    }
    std::ranges::sort(files, [](const auto& left, const auto& right) {
        return path_text(left.filename()) < path_text(right.filename());
    });
    std::vector<PackageFileDescriptor> result;
    for (std::size_t index = 0; index < files.size(); ++index)
        result.push_back(file_descriptor("script.support." + std::to_string(index), files[index], license));
    return result;
}

std::vector<PackageFileDescriptor> scan_managed_host_support(const std::filesystem::path& runtime,
                                                             const std::string_view preferred_configuration) {
    auto ancestor=runtime.parent_path();std::filesystem::path selected;
    for(int depth=0;depth<6&&!ancestor.empty();++depth,ancestor=ancestor.parent_path()) {
        const auto candidate=ancestor/"managed"/preferred_configuration;
        std::error_code error;if(std::filesystem::is_regular_file(candidate/"Noemancer.ManagedHost.dll",error))selected=candidate;
        if(!selected.empty())break;
    }
    if(selected.empty())return {};
    std::vector<PackageFileDescriptor> result;std::vector<std::filesystem::path> files;
    for(const auto* name:{"Noemancer.Managed.dll","Noemancer.ManagedHost.dll","Noemancer.ManagedHost.deps.json",
                         "Noemancer.ManagedHost.runtimeconfig.json","Noemancer.ManagedHost.pdb"}) {
        const auto path=selected/name;std::error_code error;if(std::filesystem::is_regular_file(path,error))files.push_back(path);
    }
    for(std::size_t index=0;index<files.size();++index) {auto descriptor=file_descriptor("managed.host."+std::to_string(index),files[index],"noemancer-runtime");
        descriptor.staging_path=std::filesystem::path("managed")/files[index].filename();result.push_back(std::move(descriptor));}
    return result;
}

std::tuple<int,int,int> runtime_version_key(const std::string& version) {
    std::array<int,3> parts{};std::size_t begin{};
    for(std::size_t index=0;index<parts.size()&&begin<version.size();++index) {
        const auto end=version.find('.',begin);const auto* first=version.data()+begin;
        const auto* last=version.data()+(end==std::string::npos?version.size():end);
        if(std::from_chars(first,last,parts[index]).ec!=std::errc{})return {};
        begin=end==std::string::npos?version.size():end+1U;
    }
    return {parts[0],parts[1],parts[2]};
}

std::vector<PackageFileDescriptor> scan_bundled_dotnet_runtime(const std::filesystem::path& runtime) {
    auto ancestor=runtime.parent_path();std::filesystem::path root;
    for(int depth=0;depth<8&&!ancestor.empty();++depth,ancestor=ancestor.parent_path()) {
        const auto candidate=ancestor/"_tools/dotnet";std::error_code error;
        if(std::filesystem::is_directory(candidate/"host/fxr",error)&&
           std::filesystem::is_directory(candidate/"shared/Microsoft.NETCore.App",error)){root=candidate;break;}
    }
    if(root.empty())return {};
    std::filesystem::path selected;std::tuple<int,int,int> selected_key{};std::error_code error;
    for(const auto& entry:std::filesystem::directory_iterator(root/"host/fxr",error)) {
        if(!entry.is_directory(error))continue;const auto key=runtime_version_key(entry.path().filename().string());
        if(std::get<0>(key)!=10||(!selected.empty()&&key<=selected_key))continue;
        const auto shared=root/"shared/Microsoft.NETCore.App"/entry.path().filename();
        if(std::filesystem::is_regular_file(entry.path()/"hostfxr.dll",error)&&std::filesystem::is_directory(shared,error)) {
            selected=entry.path().filename();selected_key=key;
        }
    }
    if(selected.empty())return {};
    std::vector<std::filesystem::path> files{root/"host/fxr"/selected/"hostfxr.dll",root/"LICENSE.txt",root/"ThirdPartyNotices.txt"};
    for(std::filesystem::recursive_directory_iterator iterator(root/"shared/Microsoft.NETCore.App"/selected,error),end;
        iterator!=end;iterator.increment(error))if(!error&&iterator->is_regular_file(error))files.push_back(iterator->path());
    std::ranges::sort(files,{},[](const auto& path){return path.generic_string();});
    std::vector<PackageFileDescriptor> result;result.reserve(files.size());
    for(std::size_t index=0;index<files.size();++index) {
        auto descriptor=file_descriptor("runtime.dotnet."+std::to_string(index),files[index],"microsoft-dotnet-runtime");
        descriptor.staging_path=std::filesystem::path("runtime/dotnet")/files[index].lexically_relative(root);
        result.push_back(std::move(descriptor));
    }
    return result;
}

struct VcRuntimeBundle final {
    std::string version;
    std::string redistribution_notice;
    std::vector<PackageFileDescriptor> files;
};

VcRuntimeBundle scan_bundled_vc_runtime() {
    std::vector<std::filesystem::path> roots;
#ifdef _WIN32
    char* configured{};std::size_t configured_size{};
    if(_dupenv_s(&configured,&configured_size,"VCToolsRedistDir")==0&&configured!=nullptr) {
        roots.emplace_back(configured);std::free(configured);
    }
    char* program_files{};std::size_t program_files_size{};
    if(_dupenv_s(&program_files,&program_files_size,"ProgramFiles")==0&&program_files!=nullptr) {
        const auto visual_studio=std::filesystem::path(program_files)/"Microsoft Visual Studio/2022";
        for(const auto* edition:{"Community","Professional","Enterprise","BuildTools"})
            roots.push_back(visual_studio/edition/"VC/Redist/MSVC");
        std::free(program_files);
    }
#endif
    std::filesystem::path selected_crt;std::tuple<int,int,int> selected_key{};std::string selected_version;
    for(const auto& root:roots) {
        std::vector<std::filesystem::path> version_roots;std::error_code error;
        if(std::filesystem::is_directory(root/"x64/Microsoft.VC143.CRT",error))version_roots.push_back(root);
        else if(std::filesystem::is_directory(root,error))for(const auto& entry:std::filesystem::directory_iterator(root,error))
            if(entry.is_directory(error))version_roots.push_back(entry.path());
        for(const auto& version_root:version_roots) {
            const auto crt=version_root/"x64/Microsoft.VC143.CRT";
            if(!std::filesystem::is_directory(crt,error))continue;
            const auto key=runtime_version_key(version_root.filename().string());
            if(std::get<0>(key)<=0||(!selected_crt.empty()&&key<=selected_key))continue;
            selected_crt=crt;selected_key=key;selected_version=version_root.filename().string();
        }
    }
    VcRuntimeBundle result;result.version=selected_version;if(selected_crt.empty())return result;
    std::vector<std::filesystem::path> dlls;std::error_code error;
    for(const auto& entry:std::filesystem::directory_iterator(selected_crt,error))
        if(entry.is_regular_file(error)&&lower(entry.path().extension().string())==".dll")dlls.push_back(entry.path());
    std::ranges::sort(dlls,{},[](const auto& path){return lower(path.filename().string());});
    for(std::size_t index=0;index<dlls.size();++index) {
        auto descriptor=file_descriptor("runtime.vc."+std::to_string(index),dlls[index],"microsoft-vc-runtime");
        descriptor.staging_path=std::filesystem::path("bin")/dlls[index].filename();result.files.push_back(std::move(descriptor));
    }
    auto ancestor=selected_crt;
    for(int depth=0;depth<10&&!ancestor.empty();++depth,ancestor=ancestor.parent_path()) {
        const auto notice=ancestor/"Licenses/1033/Redist.txt";
        if(const auto text=read_text(notice);text&&!text->empty()) {
            result.redistribution_notice=*text;auto descriptor=file_descriptor("runtime.vc.redist",notice,"microsoft-vc-runtime");
            descriptor.staging_path="licenses/microsoft-vc/Redist.txt";result.files.push_back(std::move(descriptor));break;
        }
    }
    if(result.redistribution_notice.empty())result.files.clear();return result;
}

std::optional<std::filesystem::path> select_runtime_executable(const std::filesystem::path& invoked_runtime,
                                                                const std::string_view configuration) {
    if(invoked_runtime.empty())return std::nullopt;
    const auto invoked=std::filesystem::absolute(invoked_runtime).lexically_normal();
    if(lower(invoked.parent_path().filename().string())==lower(std::string(configuration)))return invoked;
    const auto candidate=(invoked.parent_path().parent_path()/configuration/invoked.filename()).lexically_normal();
    std::error_code error;
    if(std::filesystem::is_regular_file(candidate,error))return candidate;
    return std::nullopt;
}

std::optional<std::filesystem::path> find_script_assembly(const ProjectDocument& project,
                                                          const std::string_view preferred_configuration) {
    if (!project.script_project) return std::nullopt;
    const auto script_project = project.root / *project.script_project;
    const auto stem = script_project.stem().string() + ".dll";
    {
        const auto directory = script_project.parent_path() / "bin" / std::string(preferred_configuration);
        std::vector<std::filesystem::path> candidates;
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) return std::nullopt;
        for (std::filesystem::recursive_directory_iterator iterator(directory, error), end; iterator != end; iterator.increment(error)) {
            if (error) break;
            if (iterator->is_regular_file(error) && iterator->path().filename() == stem &&
                path_text(iterator->path()).find("/ref/") == std::string::npos &&
                path_text(iterator->path()).find("/refint/") == std::string::npos)
                candidates.push_back(iterator->path());
        }
        if (!candidates.empty()) {
            std::ranges::sort(candidates, [](const auto& left, const auto& right) {
                return path_text(left) < path_text(right);
            });
            return candidates.back();
        }
    }
    return std::nullopt;
}

std::optional<PackageCookManifest> make_cook_manifest(const PackageInput& input,
                                                       const AssetRegistry& registry,
                                                       const Json& manifest, ServiceError& error) {
    if (!manifest.contains("outputs") || !manifest.at("outputs").is_array() ||
        manifest.at("outputs").size() > kCookManifestMaxOutputs) {
        error = {"package.cook-manifest-invalid", "Cook manifest outputs must be an array."};
        return std::nullopt;
    }
    PackageCookManifest result;
    result.schema = manifest.value("schema", std::string{});
    result.content_hash = manifest.value("contentHash", std::string{});
    result.target_profile = manifest.value("targetProfile", std::string{});
    std::map<std::string,std::tuple<std::string,std::string,std::string>> unique_outputs;
    for (std::size_t index = 0; index < manifest.at("outputs").size(); ++index) {
        const auto& output = manifest.at("outputs").at(index);
        if (!output.is_object() || !output.contains("assetId") || !output.at("assetId").is_string() ||
            !output.contains("payloadUri") || !output.at("payloadUri").is_string() ||
            !output.contains("payloadFormat") || !output.at("payloadFormat").is_string()) {
            error = {"package.cook-manifest-invalid", "Cook manifest output at index " + std::to_string(index) + " is incomplete."};
            return std::nullopt;
        }
        std::string asset_id;
        std::string payload_uri;
        std::string payload_format;
        std::string payload_hash;
        if (!bounded_manifest_text(output.at("assetId"), kCookManifestMaxIdentifierText, false, asset_id) ||
            !bounded_manifest_text(output.at("payloadUri"), kCookManifestMaxMetadataText, false, payload_uri) ||
            !bounded_manifest_text(output.at("payloadFormat"), kCookManifestMaxIdentifierText, false, payload_format)) {
            error = {"package.cook-manifest-invalid", "Cook manifest output at index " +
                std::to_string(index) + " has an invalid or oversized identity field."};
            return std::nullopt;
        }
        if (!output.contains("payloadHash") ||
            !bounded_manifest_text(output.at("payloadHash"), kCookManifestMaxMetadataText, false,
                                   payload_hash)) {
            error = {"package.artifact-hash-missing", "Cook output " + asset_id + " has no payloadHash."};
            return std::nullopt;
        }
        if(output.contains("buildInputs")) {
            if(!output.at("buildInputs").is_array() ||
               output.at("buildInputs").size() > kCookManifestMaxMetadataItems) {
                error={"package.cook-manifest-invalid","Cook buildInputs must be an array."};
                return std::nullopt;
            }
            for(const auto& build_input:output.at("buildInputs")) {
                std::string build_input_id;
                if(!build_input.is_object()||!build_input.contains("assetId")||
                    !bounded_manifest_text(build_input.at("assetId"), kCookManifestMaxIdentifierText,
                                           false, build_input_id)) {
                    error={"package.cook-manifest-invalid","Cook build input identity is invalid."};
                    return std::nullopt;
                }
                std::string build_input_redistribution;
                if (build_input.contains("redistribution") &&
                    !bounded_manifest_text(build_input.at("redistribution"), kCookManifestMaxIdentifierText,
                                           true, build_input_redistribution)) {
                    error={"package.cook-manifest-invalid","Cook build input redistribution is invalid."};
                    return std::nullopt;
                }
                if(lower(build_input_redistribution)=="local-only") {
                    error={"package.derived-source-not-redistributable",
                        "Cooked asset "+asset_id+" derives from local-only source "+
                            build_input_id+"."};
                    return std::nullopt;
                }
            }
        }
        ServiceError path_error;
        const auto payload = payload_path(input.project.root, payload_uri, path_error);
        if (!path_error.code.empty()) {
            error = path_error;
            return std::nullopt;
        }
        const auto payload_identity = hash_file(payload);
        if (!payload_identity.available || payload_identity.hash != payload_hash) {
            error = {"package.file-identity-mismatch",
                "Cook output " + asset_id + " does not match its payloadHash."};
            return std::nullopt;
        }
        const auto output_identity=std::tuple{payload_uri,payload_format,payload_hash};
        if(const auto found=unique_outputs.find(asset_id);found!=unique_outputs.end()) {
            if(found->second!=output_identity) {
                error={"package.cook-output-conflict","Cook manifest repeats "+asset_id+" with a different payload identity."};
                return std::nullopt;
            }
            continue;
        }
        unique_outputs.emplace(asset_id,output_identity);
        if (payload_format == "meshopt/meshbin" || payload.extension() == ".glb" ||
            payload.extension() == ".fbx") {
            error = {"package.source-geometry-forbidden",
                "Cook output " + asset_id + " is legacy or source geometry, not a runtime artifact."};
            return std::nullopt;
        }
        if (payload_format.starts_with("noemancer/meshbin/") &&
            payload_format != "noemancer/meshbin/0.2") {
            error = {"package.mesh-artifact-version-unsupported",
                "Cook output " + asset_id + " uses an unsupported mesh artifact version."};
            return std::nullopt;
        }
        if (payload_format == "noemancer/meshbin/0.2") {
            constexpr std::uintmax_t maximum_mesh_artifact_bytes = 512U * 1024U * 1024U;
            const auto bytes = read_bounded_binary(payload, maximum_mesh_artifact_bytes);
            const auto loaded = load_mesh_runtime_artifact(bytes, asset_id, {}, payload_hash);
            if (!loaded.success) {
                error = {"package.mesh-artifact-invalid", "Cook output " + asset_id +
                    " failed runtime artifact validation: " + loaded.code + " - " + loaded.detail};
                return std::nullopt;
            }
        }
        std::string source_asset_id;
        if (output.contains("sourceAssetId") &&
            !bounded_manifest_text(output.at("sourceAssetId"), kCookManifestMaxIdentifierText,
                                   false, source_asset_id)) {
            error = {"package.cook-manifest-invalid", "Cook output " + asset_id +
                " sourceAssetId must be a bounded non-empty string."};
            return std::nullopt;
        }
        const auto* asset = registry.find(asset_id);
        const auto* source_asset = source_asset_id.empty() ? nullptr : registry.find(source_asset_id);
        // A derived page normally has no registry record of its own. Its
        // sourceAssetId supplies the legal/default metadata while explicit
        // manifest fields remain authoritative for the derived artifact.
        const auto* metadata_asset = asset == nullptr ? source_asset : asset;

        PackageCookArtifact artifact;
        artifact.asset_id = asset_id;
        artifact.display_name = metadata_asset == nullptr ? asset_id : metadata_asset->display_name;
        artifact.kind = metadata_asset == nullptr ? std::string{"Cooked"} : metadata_asset->kind;
        artifact.payload_uri = payload_uri;
        artifact.payload_format = payload_format;
        artifact.source_path = payload;
        artifact.content_hash = payload_hash;
        artifact.license_id = metadata_asset == nullptr ? std::string{} : metadata_asset->license;
        artifact.redistribution = metadata_asset == nullptr ? std::string{} : metadata_asset->redistribution;
        artifact.streaming_mode = metadata_asset == nullptr ? std::string{"stream"} : metadata_asset->streaming_mode;
        artifact.streaming_importance = metadata_asset == nullptr ? std::string{"normal"} : metadata_asset->streaming_importance;
        artifact.streaming_priority = metadata_asset == nullptr ? 500U : metadata_asset->streaming_priority;
        artifact.available = true;
        // The manifest may contain cache outputs from a broader Cook. Package
        // closure starts from startup-scene references and follows dependencies.
        artifact.required = false;
        if (metadata_asset != nullptr) artifact.dependencies = metadata_asset->dependencies;
        if (metadata_asset != nullptr) artifact.tags = metadata_asset->tags;

        const auto invalid_metadata = [&](const std::string_view field,
                                          const std::string_view expected) {
            error = {"package.cook-manifest-invalid", "Cook output " + asset_id +
                " field " + std::string(field) + " " + std::string(expected) + "."};
            return false;
        };
        const auto read_text_field = [&](const char* field, std::string& target,
                                         const std::size_t maximum = kCookManifestMaxMetadataText) {
            if (!output.contains(field)) return true;
            std::string value;
            if (!bounded_manifest_text(output.at(field), maximum, false, value))
                return invalid_metadata(field, "must be a bounded non-empty string");
            target = std::move(value);
            return true;
        };
        if (!read_text_field("displayName", artifact.display_name, kCookManifestMaxMetadataText) ||
            !read_text_field("kind", artifact.kind, kCookManifestMaxIdentifierText) ||
            !read_text_field("license", artifact.license_id, kCookManifestMaxIdentifierText) ||
            !read_text_field("redistribution", artifact.redistribution, kCookManifestMaxIdentifierText))
            return std::nullopt;

        const auto read_string_array = [&](const char* field, std::vector<std::string>& target) {
            if (!output.contains(field)) return true;
            const auto& value = output.at(field);
            if (!value.is_array() || value.size() > kCookManifestMaxMetadataItems)
                return invalid_metadata(field, "must be an array within the metadata bound");
            std::vector<std::string> items;
            items.reserve(value.size());
            for (std::size_t item_index = 0U; item_index < value.size(); ++item_index) {
                std::string item;
                if (!bounded_manifest_text(value.at(item_index), kCookManifestMaxIdentifierText,
                                           false, item)) {
                    error = {"package.cook-manifest-invalid", "Cook output " + asset_id +
                        " field " + field + " contains an invalid item at index " +
                        std::to_string(item_index) + "."};
                    return false;
                }
                items.push_back(std::move(item));
            }
            target = std::move(items);
            return true;
        };
        if (!read_string_array("tags", artifact.tags) ||
            !read_string_array("dependencies", artifact.dependencies))
            return std::nullopt;

        if (output.contains("required")) {
            if (!output.at("required").is_boolean()) {
                invalid_metadata("required", "must be a boolean");
                return std::nullopt;
            }
            artifact.required = output.at("required").get<bool>();
        }
        if (output.contains("streamingPolicy")) {
            const auto& policy = output.at("streamingPolicy");
            if (!policy.is_object()) {
                invalid_metadata("streamingPolicy", "must be an object");
                return std::nullopt;
            }
            for (auto field = policy.begin(); field != policy.end(); ++field) {
                if (field.key() != "mode" && field.key() != "importance" && field.key() != "priority") {
                    invalid_metadata("streamingPolicy", "contains an unknown field");
                    return std::nullopt;
                }
            }
            std::string mode;
            std::string importance;
            if (!policy.contains("mode") || !bounded_manifest_text(
                    policy.at("mode"), kCookManifestMaxIdentifierText, false, mode) ||
                (mode != "stream" && mode != "resident")) {
                invalid_metadata("streamingPolicy.mode", "must be stream or resident");
                return std::nullopt;
            }
            if (!policy.contains("importance") || !bounded_manifest_text(
                    policy.at("importance"), kCookManifestMaxIdentifierText, false, importance) ||
                (importance != "low" && importance != "normal" && importance != "high" &&
                 importance != "critical")) {
                invalid_metadata("streamingPolicy.importance", "must be low, normal, high or critical");
                return std::nullopt;
            }
            if (!policy.contains("priority") || !policy.at("priority").is_number_unsigned()) {
                invalid_metadata("streamingPolicy.priority", "must be an unsigned integer in 0..1000");
                return std::nullopt;
            }
            const auto priority = policy.at("priority").get<std::uint64_t>();
            if (priority > 1000U) {
                invalid_metadata("streamingPolicy.priority", "must be an unsigned integer in 0..1000");
                return std::nullopt;
            }
            artifact.streaming_mode = std::move(mode);
            artifact.streaming_importance = std::move(importance);
            artifact.streaming_priority = static_cast<std::uint32_t>(priority);
        }
        result.outputs.push_back(std::move(artifact));
    }
    return result;
}

void add_cook_artifact_license(std::map<std::string, PackageLicenseDescriptor>& licenses,
                               const PackageCookArtifact& artifact,
                               const AssetRecord* provenance_asset,
                               const std::filesystem::path& project_root) {
    if (artifact.license_id.empty()) return;
    if (provenance_asset != nullptr && provenance_asset->license == artifact.license_id) {
        add_license(licenses, provenance_asset, project_root);
        return;
    }
    if (licenses.contains(artifact.license_id)) return;

    const bool project_owned = is_project_owned(artifact.license_id) ||
        artifact.redistribution == "project-only";
    const auto source = provenance_asset == nullptr ? std::filesystem::path{} :
        (provenance_asset->source_root.empty() || provenance_asset->relative_path.empty()
            ? std::filesystem::path{}
            : std::filesystem::path(provenance_asset->source_root) / provenance_asset->relative_path);
    const auto notice = source.empty()
        ? std::string("License declared by the Cook manifest for derived artifact ") + artifact.asset_id + "."
        : read_notice(source, project_root).value_or(
            std::string("License declared by the Cook manifest for derived artifact ") + artifact.asset_id + ".");
    licenses.emplace(artifact.license_id, PackageLicenseDescriptor{
        .id = artifact.license_id,
        .name = artifact.license_id,
        .spdx_id = artifact.license_id,
        .notice = notice,
        .source_uri = provenance_asset == nullptr ?
            "generated://cook-manifest/" + slug(artifact.asset_id) : provenance_asset->uri,
        .third_party = !project_owned,
        .redistributable = project_owned || artifact.redistribution == "public" ||
            artifact.redistribution == "allowed"
    });
}

PackageCommitCallback make_atomic_commit_callback(const PackagePlan& plan) {
    return [plan](const PackageCommitRequest& request) {
        PackageCommitResult result;
        const auto output = request.staging_root.lexically_normal();
        if (output.empty() || output.filename().empty() || output.filename() == "." || output.filename() == "..") {
            result.code = "package.output-invalid";
            result.detail = "Package output must name a directory.";
            return result;
        }
        std::error_code error;
        if (std::filesystem::exists(output, error)) {
            if (std::filesystem::is_symlink(output, error)) {
                result.code = "package.output-symlink";
                result.detail = "Refusing to package through a symbolic-link output path.";
                return result;
            }
            result.code = "package.output-exists";
            result.detail = "Package output already exists; choose a new path.";
            return result;
        }
        const auto parent = output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
        std::filesystem::create_directories(parent, error);
        if (error) {
            result.code = "package.output-parent-failed";
            result.detail = error.message();
            return result;
        }
        const auto hash_suffix = request.content_hash.substr(request.content_hash.find(':') + 1U);
        const auto temporary = parent / ("." + slug(output.filename().string()) + ".staging-" + slug(hash_suffix));
        if (std::filesystem::exists(temporary, error)) {
            result.code = "package.staging-exists";
            result.detail = "A sibling staging directory with the same package identity already exists.";
            return result;
        }
        bool staging_created = false;
        try {
            std::filesystem::create_directories(temporary);
            staging_created = true;
            for (const auto& entry : request.entries) {
                const auto destination = temporary / entry.staging_path;
                std::filesystem::create_directories(destination.parent_path());
                if (entry.role == "game-profile") {
                    std::ofstream stream(destination, std::ios::binary);
                    stream << plan.game_profile_json << '\n';
                    if (!stream) throw std::runtime_error("Game Profile write failed.");
                } else if(entry.role=="asset-registry") {
                    std::ofstream stream(destination,std::ios::binary);
                    stream<<plan.content_registry_json<<'\n';
                    if(!stream)throw std::runtime_error("Asset Registry write failed.");
                } else if (entry.role == "license-manifest") {
                    std::ofstream stream(destination, std::ios::binary);
                    stream << plan.third_party_license_json << '\n';
                    if (!stream) throw std::runtime_error("License manifest write failed.");
                } else if (entry.role == "notice-manifest") {
                    std::ofstream stream(destination, std::ios::binary);
                    stream << plan.notice_text;
                    if (!stream) throw std::runtime_error("NOTICE write failed.");
                } else {
                    std::error_code copy_error;
                    std::filesystem::copy_file(entry.source_path, destination,
                        std::filesystem::copy_options::none, copy_error);
                    if (copy_error) throw std::runtime_error("Package copy failed for " + entry.id + ": " + copy_error.message());
                    const auto committed=hash_file(destination);
                    if(!committed.available||committed.bytes!=entry.bytes||committed.hash!=entry.content_hash)
                        throw std::runtime_error("Package copy identity changed for "+entry.id+" during commit.");
                }
            }
            // Virus scanners can briefly retain a handle to a newly copied
            // executable on Windows. Keep the atomic sibling rename, but give
            // that transient observer a bounded opportunity to release it.
            for(std::uint32_t attempt=0U;attempt<40U;++attempt) {
                error.clear();
                std::filesystem::rename(temporary, output, error);
                if(!error)break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (error) throw std::runtime_error("Atomic package directory rename failed: " + error.message());
            staging_created = false;
            result.success = true;
            result.atomic = true;
            result.code = "ok";
            result.detail = "Package staged and committed with an atomic directory rename.";
            result.commit_id = "directory-rename:" + request.content_hash;
            return result;
        } catch (const std::exception& exception) {
            if (staging_created) {
                std::error_code cleanup_error;
                std::filesystem::remove_all(temporary, cleanup_error);
            }
            result.code = "package.commit-failed";
            result.detail = exception.what();
            return result;
        }
    };
}

Json make_service_envelope(const PackagePlan& plan, const PackageReceipt& receipt) {
    return Json{
        {"schema", "noemancer.windows-package/0.1"},
        {"success", receipt.success},
        {"code", receipt.code},
        {"detail", receipt.detail},
        {"plan", Json::parse(package_plan_json(plan))},
        {"receipt", Json::parse(package_receipt_json(receipt))}
    };
}

} // namespace

std::string run_windows_package_json(const WindowsPackageOptions& options) {
    try {
        if (options.project_path.empty() || options.output_path.empty())
            return error_json({"package.arguments-invalid", "Both project and output paths are required."}).dump();
        const auto loaded = load_project(options.project_path);
        if (!loaded) {
            const auto errors = Json::parse(project_load_errors_json(loaded));
            return Json{
                {"schema", "noemancer.windows-package/0.1"},
                {"success", false},
                {"code", "package.project-load-failed"},
                {"detail", "Project manifest or startup scene could not be loaded."},
                {"projectErrors", errors.at("errors")},
                {"plan", nullptr},
                {"receipt", nullptr}
            }.dump();
        }
        const auto target_profile = cook_platform_profile(options.target_profile);
        std::string profile_code;
        std::string profile_detail;
        if (!validate_cook_platform_profile(target_profile, profile_code, profile_detail))
            return error_json({"package.target-profile-invalid", profile_detail}).dump();
        const auto managed_configuration=options.target_profile.ends_with("-debug")?"Debug":"Release";
        if(loaded.project->script_project) {
            ManagedScriptRuntime compiler;
            const auto configured=Json::parse(compiler.configure_project_json(
                loaded.project->root,*loaded.project->script_project),nullptr,false);
            if(!configured.is_object()||!configured.value("success",false))return Json{{"schema","noemancer.windows-package/0.1"},
                {"success",false},{"code","package.script-configure-failed"},
                {"detail","Project scripting could not be configured for packaging."},{"compile",configured},{"plan",nullptr},{"receipt",nullptr}}.dump();
            const auto compiled=Json::parse(compiler.compile_project_json(managed_configuration),nullptr,false);
            if(!compiled.is_object()||!compiled.value("success",false))return Json{{"schema","noemancer.windows-package/0.1"},
                {"success",false},{"code","package.script-compile-failed"},
                {"detail","Project C# failed to compile for the selected Package profile."},{"compile",compiled},{"plan",nullptr},{"receipt",nullptr}}.dump();
        }

        if (loaded.project->asset_roots.empty())
            return error_json({"package.asset-roots-missing", "The project must declare at least one asset root."}).dump();
        AssetRegistry registry(loaded.project->root / loaded.project->asset_roots.front());
        for (std::size_t index = 1; index < loaded.project->asset_roots.size(); ++index)
            static_cast<void>(registry.add_root(loaded.project->root / loaded.project->asset_roots[index]));
        if (registry.records().empty())
            return error_json({"package.asset-registry-empty", "The project Asset Registry contains no assets."}).dump();

        std::vector<std::string> asset_ids;
        for(const auto& id:collect_scene_asset_ids(*loaded.startup_scene))
            if(const auto* record=registry.find(id);record!=nullptr&&record->available)asset_ids.push_back(id);
        for(const auto& id:loaded.project->packaged_assets)asset_ids.push_back(id);
        const auto cook_plan=Json::parse(registry.cook_plan_json(asset_ids,options.target_profile),nullptr,false);
        if(!cook_plan.is_object()||!cook_plan.value("valid",false))return Json{{"schema","noemancer.windows-package/0.1"},
            {"success",false},{"code","package.cook-plan-invalid"},{"detail","Project assets could not form a valid Cook plan."},
            {"cookPlan",cook_plan},{"plan",nullptr},{"receipt",nullptr}}.dump();
        if(!options.dry_run) {
            const auto cook_receipt=Json::parse(registry.apply_cook_plan_json(cook_plan.dump(),false),nullptr,false);
            if(!cook_receipt.is_object()||!cook_receipt.value("success",false))return Json{{"schema","noemancer.windows-package/0.1"},
                {"success",false},{"code","package.cook-failed"},{"detail","Project Cook failed before packaging."},
                {"cookPlan",cook_plan},{"cookReceipt",cook_receipt},{"plan",nullptr},{"receipt",nullptr}}.dump();
        }
        ServiceError manifest_error;
        auto manifest=find_cook_manifest(loaded.project->root/"generated",options.target_profile,
            cook_plan.value("planId",std::string{}),manifest_error);
        if (!manifest) return error_json(manifest_error).dump();
        if(manifest->value("contentHash",std::string{})!=cook_plan.value("contentHash",std::string{}))
            return error_json({"package.cook-manifest-stale","Cook manifest content does not match current project assets."}).dump();

        PackageInput input;
        input.project = *loaded.project;
        input.startup_scene = *loaded.startup_scene;
        input.target_profile = target_profile;
        input.game_profile = PackageGameProfile{
            .id = options.target_profile,
            .display_name = loaded.project->name,
            .platform = "windows",
            .architecture = "x64",
            .configuration = lower(managed_configuration),
            .executable_name = slug(loaded.project->name) + ".exe"
        };
        input.staging_root = options.output_path;
        input.dry_run = options.dry_run;
        input.startup_scene_file = file_descriptor("startup.scene",
            loaded.project->root / loaded.project->startup_scene, "noemancer-project");
        if(loaded.project->hud_document)input.hud_document_file=file_descriptor("project.hud",
            loaded.project->root/ *loaded.project->hud_document,"noemancer-project");
        if (const auto cook = make_cook_manifest(input, registry, *manifest, manifest_error))
            input.cook_manifest = *cook;
        else
            return error_json(manifest_error).dump();

        const auto vc_runtime=managed_configuration==std::string_view("Release")?scan_bundled_vc_runtime():VcRuntimeBundle{};
        if(managed_configuration==std::string_view("Release")&&vc_runtime.files.empty())
            return error_json({"package.vc-runtime-missing",
                "Release distribution requires the official Microsoft VC143 app-local Runtime and redistribution notice."}).dump();
        std::map<std::string, PackageLicenseDescriptor> licenses;
        for (const auto& output : input.cook_manifest.outputs) {
            const AssetRecord* provenance_asset = registry.find(output.asset_id);
            if (provenance_asset == nullptr) {
                for (const auto& manifest_output : manifest->at("outputs")) {
                    if (!manifest_output.is_object() ||
                        manifest_output.value("assetId", std::string{}) != output.asset_id ||
                        !manifest_output.contains("sourceAssetId") ||
                        !manifest_output.at("sourceAssetId").is_string()) continue;
                    provenance_asset = registry.find(manifest_output.at("sourceAssetId").get<std::string>());
                    if (provenance_asset != nullptr) break;
                }
            }
            add_cook_artifact_license(licenses, output, provenance_asset, loaded.project->root);
        }
        for(const auto& output:manifest->at("outputs"))if(output.contains("buildInputs"))
            for(const auto& build_input:output.at("buildInputs")) {
                const auto* build_asset=registry.find(build_input.at("assetId").get<std::string>());
                add_license(licenses,build_asset,loaded.project->root);
                if(build_asset!=nullptr&&!build_asset->license.empty())
                    input.required_license_ids.push_back(build_asset->license);
            }
        std::ranges::sort(input.required_license_ids);
        input.required_license_ids.erase(std::unique(input.required_license_ids.begin(),
            input.required_license_ids.end()),input.required_license_ids.end());
        licenses.emplace("noemancer-project", PackageLicenseDescriptor{
            .id = "noemancer-project", .name = "Noemancer project", .spdx_id = "LicenseRef-Noemancer-project",
            .notice = "Project-owned content.", .third_party = false, .redistributable = true});
        const auto license_anchor=options.runtime_executable.empty()?std::filesystem::path("noemancer.exe"):
            options.runtime_executable;
        std::map<std::string, std::filesystem::path> runtime_license_paths;
        for(const auto& spec:kRuntimeLicenseSpecs) {
            const auto path=find_generated_license_path(license_anchor,spec.filename);
            const auto notice=path?read_text(*path):std::nullopt;
            if(!path||!notice||notice->empty())return error_json({"package.runtime-license-missing",
                "The pinned Runtime dependency license is required for distribution: "+std::string(spec.id)+
                " ("+std::string(spec.filename)+")."}).dump();
            const bool engine_owned=spec.id==std::string_view("noemancer-runtime");
            licenses.emplace(std::string(spec.id),PackageLicenseDescriptor{
                .id=std::string(spec.id),.name=std::string(spec.name),.spdx_id=std::string(spec.spdx_id),
                .notice=*notice,.source_uri=std::string(spec.source_uri),.third_party=!engine_owned,.redistributable=true});
            runtime_license_paths.emplace(std::string(spec.id),*path);
            if(!engine_owned)input.required_license_ids.push_back(std::string(spec.id));
        }
        std::ranges::sort(input.required_license_ids);
        input.required_license_ids.erase(std::unique(input.required_license_ids.begin(),
            input.required_license_ids.end()),input.required_license_ids.end());
        const auto icu_notice=find_generated_license(license_anchor,"ICU-LICENSE.txt");
        if(!icu_notice)return error_json({"package.icu-license-missing",
            "The ICU Runtime license text is required before its DLLs can be redistributed."}).dump();
        const auto ozz_license_path=find_generated_license_path(license_anchor,"ozz-animation-LICENSE.md");
        const auto ozz_notice=find_generated_license(license_anchor,"ozz-animation-LICENSE.md");
        if(!ozz_license_path||!ozz_notice)return error_json({"package.ozz-license-missing",
            "The ozz-animation license text is required before the statically linked Runtime can be redistributed."}).dump();
        const auto ufbx_license_path=find_generated_license_path(license_anchor,"ufbx-LICENSE.txt");
        const auto ufbx_notice=find_generated_license(license_anchor,"ufbx-LICENSE.txt");
        if(!ufbx_license_path||!ufbx_notice)return error_json({"package.ufbx-license-missing",
            "The ufbx license text is required before the statically linked Runtime can be redistributed."}).dump();
        const auto fastgltf_license_path=find_generated_license_path(license_anchor,"fastgltf-LICENSE.md");
        const auto fastgltf_notice=find_generated_license(license_anchor,"fastgltf-LICENSE.md");
        const auto meshoptimizer_license_path=find_generated_license_path(license_anchor,"meshoptimizer-LICENSE.md");
        const auto meshoptimizer_notice=find_generated_license(license_anchor,"meshoptimizer-LICENSE.md");
        const auto ktx_license_path=find_generated_license_path(license_anchor,"KTX-Software-LICENSE.md");
        const auto ktx_notice=find_generated_license(license_anchor,"KTX-Software-LICENSE.md");
        if(!fastgltf_license_path||!fastgltf_notice||!meshoptimizer_license_path||!meshoptimizer_notice||
            !ktx_license_path||!ktx_notice)return error_json({"package.geometry-codec-license-missing",
                "fastgltf, meshoptimizer and KTX-Software license texts are required for Runtime distribution."}).dump();
        licenses.emplace("unicode-icu",PackageLicenseDescriptor{
            .id="unicode-icu",.name="Unicode ICU",.spdx_id="Unicode-3.0",.notice=*icu_notice,
            .source_uri="https://github.com/unicode-org/icu",.third_party=true,.redistributable=true});
        licenses.emplace("ozz-animation",PackageLicenseDescriptor{
            .id="ozz-animation",.name="ozz-animation",.spdx_id="MIT",.notice=*ozz_notice,
            .source_uri="https://github.com/guillaumeblanc/ozz-animation",.third_party=true,.redistributable=true});
        licenses.emplace("ufbx",PackageLicenseDescriptor{
            .id="ufbx",.name="ufbx",.spdx_id="MIT",.notice=*ufbx_notice,
            .source_uri="https://github.com/ufbx/ufbx",.third_party=true,.redistributable=true});
        licenses.emplace("fastgltf",PackageLicenseDescriptor{
            .id="fastgltf",.name="fastgltf",.spdx_id="MIT",.notice=*fastgltf_notice,
            .source_uri="https://github.com/spnda/fastgltf",.third_party=true,.redistributable=true});
        licenses.emplace("meshoptimizer",PackageLicenseDescriptor{
            .id="meshoptimizer",.name="meshoptimizer",.spdx_id="MIT",.notice=*meshoptimizer_notice,
            .source_uri="https://github.com/zeux/meshoptimizer",.third_party=true,.redistributable=true});
        licenses.emplace("ktx-software",PackageLicenseDescriptor{
            .id="ktx-software",.name="KTX-Software",.spdx_id="Apache-2.0",.notice=*ktx_notice,
            .source_uri="https://github.com/KhronosGroup/KTX-Software",.third_party=true,.redistributable=true});
        licenses.emplace("microsoft-dotnet-runtime",PackageLicenseDescriptor{
            .id="microsoft-dotnet-runtime",.name="Microsoft .NET Runtime",.spdx_id="MIT",
            .notice="Redistributed .NET Runtime license and third-party notices are included under runtime/dotnet/.",
            .source_uri="https://github.com/dotnet/runtime",.third_party=true,.redistributable=true});
        if(!vc_runtime.files.empty())licenses.emplace("microsoft-vc-runtime",PackageLicenseDescriptor{
            .id="microsoft-vc-runtime",.name="Microsoft Visual C++ Runtime",.spdx_id="LicenseRef-Microsoft-Visual-Cpp-Runtime",
            .notice="Official app-local redistributable files; the build-host Redist.txt is included under licenses/microsoft-vc/.",
            .source_uri="https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist",.third_party=true,.redistributable=true});
        for (const auto& [unused, license] : licenses) {
            static_cast<void>(unused);
            input.licenses.push_back(license);
        }

        const auto requested_runtime=options.runtime_executable.empty()?std::filesystem::path("noemancer.exe"):options.runtime_executable;
        const auto selected_runtime=select_runtime_executable(requested_runtime,managed_configuration);
        if(!selected_runtime)return error_json({"package.runtime-configuration-missing",
            std::string("The selected Package profile requires a prebuilt ")+managed_configuration+
            " native Runtime next to the invoking configuration. Build target noemancer for that configuration first."}).dump();
        const auto runtime=*selected_runtime;
        input.runtime.executable = file_descriptor("runtime.executable", runtime, "noemancer-runtime");
        input.runtime.support_files = scan_runtime_support(runtime.parent_path(), "noemancer-runtime", runtime);
        auto shader_support=scan_shader_support(runtime);
        const auto has_shader_support=[&shader_support](const std::string_view filename) {
            return std::ranges::any_of(shader_support,[filename](const auto& file) {
                return file.source_path.filename().generic_string()==filename;
            });
        };
        if(shader_support.empty()||!has_shader_support("shader-artifact-manifest.json")||
           !has_shader_support("shader-artifact-reflection.json"))
            return error_json({"package.shader-artifacts-missing",
                "The selected Runtime configuration requires its generated Shader Artifact Manifest, reflection proof and binaries."}).dump();
        input.runtime.support_files.insert(input.runtime.support_files.end(),
            std::make_move_iterator(shader_support.begin()),std::make_move_iterator(shader_support.end()));
        auto ozz_license_file=file_descriptor("runtime.license.ozz",*ozz_license_path,"ozz-animation");
        ozz_license_file.staging_path="licenses/runtime/ozz-animation-LICENSE.md";
        input.runtime.support_files.push_back(std::move(ozz_license_file));
        auto ufbx_license_file=file_descriptor("runtime.license.ufbx",*ufbx_license_path,"ufbx");
        ufbx_license_file.staging_path="licenses/runtime/ufbx-LICENSE.txt";
        input.runtime.support_files.push_back(std::move(ufbx_license_file));
        auto fastgltf_license_file=file_descriptor("runtime.license.fastgltf",*fastgltf_license_path,"fastgltf");
        fastgltf_license_file.staging_path="licenses/runtime/fastgltf-LICENSE.md";
        input.runtime.support_files.push_back(std::move(fastgltf_license_file));
        auto meshoptimizer_license_file=file_descriptor("runtime.license.meshoptimizer",*meshoptimizer_license_path,"meshoptimizer");
        meshoptimizer_license_file.staging_path="licenses/runtime/meshoptimizer-LICENSE.md";
        input.runtime.support_files.push_back(std::move(meshoptimizer_license_file));
        auto ktx_license_file=file_descriptor("runtime.license.ktx-software",*ktx_license_path,"ktx-software");
        ktx_license_file.staging_path="licenses/runtime/KTX-Software-LICENSE.md";
        input.runtime.support_files.push_back(std::move(ktx_license_file));
        const std::set<std::string_view> individually_staged{
            "ozz-animation","ufbx","fastgltf","meshoptimizer","ktx-software"};
        for(const auto& spec:kRuntimeLicenseSpecs) {
            if(individually_staged.contains(spec.id))continue;
            auto descriptor=file_descriptor("runtime.license."+std::string(spec.id),
                runtime_license_paths.at(std::string(spec.id)),std::string(spec.id));
            descriptor.staging_path=std::filesystem::path("licenses/runtime")/std::string(spec.filename);
            input.runtime.support_files.push_back(std::move(descriptor));
        }
        const auto lato_font=runtime_license_paths.at("lato-font").parent_path().parent_path()/
            "fonts/LatoLatin-Regular.ttf";
        if(!std::filesystem::is_regular_file(lato_font))return error_json({"package.runtime-font-missing",
            "The pinned Lato UI font is required for a relocatable Player UI."}).dump();
        auto font_file=file_descriptor("runtime.font.lato",lato_font,"lato-font");
        font_file.staging_path="content/fonts/LatoLatin-Regular.ttf";
        input.runtime.support_files.push_back(std::move(font_file));
        input.runtime.requirements={
            {.id="microsoft-vc-runtime",.display_name="Microsoft Visual C++ 2015-2022 Redistributable",
             .version=vc_runtime.version.empty()?"14.x":vc_runtime.version,.architecture="x64",.bundled=!vc_runtime.files.empty()},
            {.id="microsoft-dotnet-runtime",.display_name="Microsoft.NETCore.App Runtime",
             .version="10.x",.architecture="x64",.bundled=managed_configuration==std::string_view("Release")}
        };
        input.runtime.support_files.insert(input.runtime.support_files.end(),vc_runtime.files.begin(),vc_runtime.files.end());
        if(managed_configuration==std::string_view("Release")) {
            auto dotnet_runtime=scan_bundled_dotnet_runtime(runtime);
            if(dotnet_runtime.empty())return error_json({"package.dotnet-runtime-missing",
                "Release distribution requires the pinned .NET 10 Runtime for app-local deployment."}).dump();
            input.runtime.support_files.insert(input.runtime.support_files.end(),
                std::make_move_iterator(dotnet_runtime.begin()),std::make_move_iterator(dotnet_runtime.end()));
        }
        auto managed_host=scan_managed_host_support(runtime,managed_configuration);
        if(managed_host.size()<4U)return error_json({"package.managed-host-configuration-missing",
            std::string("The selected Package profile requires the matching ")+managed_configuration+" ManagedHost output."}).dump();
        input.runtime.support_files.insert(input.runtime.support_files.end(),std::make_move_iterator(managed_host.begin()),std::make_move_iterator(managed_host.end()));
        if (const auto assembly = find_script_assembly(*loaded.project,managed_configuration)) {
            input.script.assembly = file_descriptor("project.script", *assembly, "noemancer-project");
            input.script.support_files = scan_managed_support(assembly->parent_path(), *assembly, "noemancer-project");
        }
        const auto output_ids = [&] {
            std::set<std::string> ids;
            for (const auto& output : input.cook_manifest.outputs) ids.insert(output.asset_id);
            return ids;
        }();
        for (const auto& id : collect_scene_asset_ids(input.startup_scene))
            if (output_ids.contains(id)) input.startup_asset_ids.push_back(id);
        for(const auto& id:input.project.packaged_assets)
            if(output_ids.contains(id))input.startup_asset_ids.push_back(id);

        const PackageFileProbe probe = [](const std::filesystem::path& source) -> std::optional<PackageFileObservation> {
            if (source.generic_string() == "generated://package")
                return PackageFileObservation{.bytes = 0U, .content_hash = {}, .available = true};
            const auto observed = hash_file(source);
            if (!observed.available) return std::nullopt;
            return PackageFileObservation{.bytes = observed.bytes, .content_hash = observed.hash, .available = true};
        };
        const auto plan = plan_package(input, probe);
        const auto receipt = plan.valid
            ? commit_package(plan, options.dry_run ? PackageCommitCallback{} : make_atomic_commit_callback(plan))
            : commit_package(plan);
        return make_service_envelope(plan, receipt).dump();
    } catch (const std::exception& error) {
        return error_json({"package.service-failed", error.what()}).dump();
    }
}

} // namespace noemancer
