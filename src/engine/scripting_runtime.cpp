#include "engine/scripting_runtime.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace noemancer {
namespace {
using Json = nlohmann::json;
std::atomic<std::uint64_t> next_managed_session{1};
struct ProcessCompileArtifact final { std::string result_json; std::string assembly; };
std::mutex process_compile_cache_mutex;
std::unordered_map<std::string,ProcessCompileArtifact> process_compile_cache;

constexpr std::size_t managed_cache_max_files = 4096U;
constexpr std::uintmax_t managed_cache_max_bytes = 256U * 1024U * 1024U;
constexpr std::size_t managed_cache_max_entries = 32U;
constexpr std::uintmax_t managed_cache_total_max_bytes = 256U * 1024U * 1024U;

std::string hex_hash(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << value;
    return output.str();
}

std::string file_content_fingerprint(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t hash = 1469598103934665603ULL;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    if (input.bad()) return {};
    return hex_hash(hash);
}

std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
    char* value{};
    std::size_t size{};
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value, size == 0U ? 0U : size - 1U);
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

std::filesystem::path managed_compile_cache_root() {
    if (const auto configured = environment_value("NOEMANCER_MANAGED_COMPILE_CACHE");
        configured && !configured->empty())
        return std::filesystem::path(*configured);
    if (const auto local_app_data = environment_value("LOCALAPPDATA");
        local_app_data && !local_app_data->empty())
        return std::filesystem::path(*local_app_data) / "Noemancer" / "managed-compile-cache";
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    return (error ? std::filesystem::path(".") : temporary) / "noemancer-managed-compile-cache";
}

bool safe_relative_cache_path(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& part : relative) if (part == ".." || part == ".") return false;
    return true;
}

struct ManagedCacheFile final {
    std::filesystem::path relative;
    std::uintmax_t bytes{};
    std::string fingerprint;
};

struct ManagedCacheEntry final {
    std::string fingerprint;
    std::string configuration;
    std::filesystem::path assembly_relative;
    std::vector<ManagedCacheFile> files;
    std::uintmax_t total_bytes{};
};

void prune_managed_compile_cache() {
    struct OnDiskEntry final {
        std::filesystem::path path;
        std::filesystem::file_time_type modified{};
        std::uintmax_t bytes{};
    };
    const auto root = managed_compile_cache_root();
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) return;
    std::vector<OnDiskEntry> entries;
    for (const auto& fingerprint : std::filesystem::directory_iterator(root, error)) {
        if (error || !fingerprint.is_directory(error)) continue;
        for (const auto& configuration : std::filesystem::directory_iterator(fingerprint.path(), error)) {
            if (error || !configuration.is_directory(error) ||
                (configuration.path().filename() != "Debug" && configuration.path().filename() != "Release")) continue;
            if (!std::filesystem::is_regular_file(configuration.path() / "manifest.json", error) || error) continue;
            std::uintmax_t bytes{};
            for (const auto& file : std::filesystem::recursive_directory_iterator(configuration.path(), error)) {
                if (error || !file.is_regular_file(error)) continue;
                const auto size = file.file_size(error);
                if (error || size > managed_cache_total_max_bytes || bytes > managed_cache_total_max_bytes - size) {
                    error.clear(); bytes = managed_cache_total_max_bytes + 1U; break;
                }
                bytes += size;
            }
            const auto modified = std::filesystem::last_write_time(configuration.path(), error);
            if (error) { error.clear(); continue; }
            entries.push_back({configuration.path(), modified, bytes});
        }
    }
    std::ranges::sort(entries, [](const auto& left, const auto& right) { return left.modified < right.modified; });
    std::uintmax_t total_bytes{};
    for (const auto& entry : entries) {
        if (total_bytes > std::numeric_limits<std::uintmax_t>::max() - entry.bytes)
            total_bytes = std::numeric_limits<std::uintmax_t>::max();
        else
            total_bytes += entry.bytes;
    }
    while (entries.size() > managed_cache_max_entries || total_bytes > managed_cache_total_max_bytes) {
        if (entries.empty()) break;
        const auto oldest = entries.front();
        std::filesystem::remove_all(oldest.path, error);
        if (!error) {
            if (total_bytes >= oldest.bytes) total_bytes -= oldest.bytes; else total_bytes = 0;
        }
        error.clear();
        entries.erase(entries.begin());
    }
}

std::tuple<int, int, int> version_key(const std::string& version) {
    std::array<int, 3> parts{};
    std::size_t begin{};
    for (std::size_t index = 0; index < parts.size() && begin < version.size(); ++index) {
        const auto end = version.find('.', begin);
        const auto* first = version.data() + begin;
        const auto* last = version.data() + (end == std::string::npos ? version.size() : end);
        if (std::from_chars(first, last, parts[index]).ec != std::errc{}) return {};
        begin = end == std::string::npos ? version.size() : end + 1;
    }
    return {parts[0], parts[1], parts[2]};
}

Json hostfxr_probe() {
    std::vector<std::filesystem::path> roots;
#ifdef _WIN32
    std::array<wchar_t,32768> module_path{};
    const auto module_length=GetModuleFileNameW(nullptr,module_path.data(),static_cast<DWORD>(module_path.size()));
    if(module_length>0&&module_length<module_path.size()) {
        const auto executable_directory=std::filesystem::path(module_path.data()).parent_path();
        roots.emplace_back(executable_directory.parent_path()/"runtime/dotnet/host/fxr");
    }
    roots.emplace_back(std::filesystem::path(NOEMANCER_SOURCE_DIR) / "_tools/dotnet/host/fxr");
    char* dotnet_root{};
    std::size_t dotnet_root_size{};
    if (_dupenv_s(&dotnet_root, &dotnet_root_size, "DOTNET_ROOT") == 0 && dotnet_root != nullptr) {
        roots.emplace_back(std::filesystem::path(dotnet_root) / "host/fxr");
        std::free(dotnet_root);
    }
    roots.emplace_back("C:/Program Files/dotnet/host/fxr");
    constexpr auto library_name = "hostfxr.dll";
#else
    if (const auto* dotnet_root = std::getenv("DOTNET_ROOT"); dotnet_root != nullptr && *dotnet_root != '\0')
        roots.emplace_back(std::filesystem::path(dotnet_root) / "host/fxr");
    roots.emplace_back("/usr/share/dotnet/host/fxr");
    roots.emplace_back("/usr/local/share/dotnet/host/fxr");
#ifdef __APPLE__
    constexpr auto library_name = "libhostfxr.dylib";
#else
    constexpr auto library_name = "libhostfxr.so";
#endif
#endif
    std::vector<std::filesystem::path> unique_roots;unique_roots.reserve(roots.size());
    for(const auto& root:roots)if(std::ranges::find(unique_roots,root)==unique_roots.end())unique_roots.push_back(root);
    roots=std::move(unique_roots);
    std::vector<std::string> versions;
    std::tuple<int, int, int> selected_key{};
    std::string selected;
    std::filesystem::path selected_library;
    std::error_code error;
    Json searched = Json::array();
    for (const auto& root : roots) {
        searched.push_back(root.generic_string());
        error.clear();
        if (!std::filesystem::is_directory(root, error)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (!entry.is_directory()) continue;
            const auto version = entry.path().filename().string();
            const auto key = version_key(version);
            const auto library = entry.path() / library_name;
            error.clear();
            if (std::get<0>(key) <= 0 || !std::filesystem::is_regular_file(library, error)) continue;
            versions.push_back(version);
            if (selected.empty() || key > selected_key) {
                selected_key = key;
                selected = version;
                selected_library = library;
            }
        }
    }
    std::ranges::sort(versions);
    versions.erase(std::unique(versions.begin(), versions.end()), versions.end());
    const auto highest_major = std::get<0>(selected_key);
    return {{"searchedRoots", std::move(searched)}, {"discoveredVersions", versions}, {"selectedVersion", selected},
        {"libraryPath", selected_library.generic_string()}, {"requiredMajor", 10}, {"compatible", highest_major >= 10},
        {"diagnostic", highest_major >= 10 ? "compatible-hostfxr-found" : highest_major > 0 ? "hostfxr-version-too-old" : "hostfxr-not-found"}};
}

std::filesystem::path newest_managed_artifact(const std::string_view filename) {
    std::vector<std::filesystem::path> roots;
#ifdef _WIN32
    std::array<wchar_t,32768> module_path{};const auto length=GetModuleFileNameW(nullptr,module_path.data(),static_cast<DWORD>(module_path.size()));
    if(length>0&&length<module_path.size()) {const auto directory=std::filesystem::path(module_path.data()).parent_path();
        roots.push_back(directory/"managed");roots.push_back(directory.parent_path()/"managed");}
#endif
    roots.push_back(std::filesystem::path(NOEMANCER_BINARY_DIR)/"managed");
    std::error_code error;
    for(const auto& root:roots) {
        error.clear();if(!std::filesystem::is_directory(root,error))continue;
        std::filesystem::path selected;std::filesystem::file_time_type selected_time{};
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
            if (error) break;
            if (!entry.is_regular_file(error) || entry.path().filename() != filename) continue;
            const auto time = entry.last_write_time(error);
            if (selected.empty() || (!error && time > selected_time)) {selected=entry.path();selected_time=time;}
        }
        if(!selected.empty())return selected;
    }
    return {};
}

std::string assembly_fingerprint(const std::filesystem::path& assembly) {
    std::error_code error;std::uint64_t hash=1469598103934665603ULL;
    const auto mix=[&](const std::string_view value){for(const auto byte:value){hash^=static_cast<unsigned char>(byte);hash*=1099511628211ULL;}};
    mix(assembly.filename().generic_string());mix(std::to_string(std::filesystem::file_size(assembly,error)));
    error.clear();mix(std::to_string(std::filesystem::last_write_time(assembly,error).time_since_epoch().count()));
    std::ostringstream output;output<<std::hex<<hash;return output.str();
}

std::string result_code(const int value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<std::uint32_t>(value);
    return stream.str();
}

std::vector<std::filesystem::path> project_source_inputs(const std::filesystem::path& project_file) {
    std::vector<std::filesystem::path> inputs;
    std::error_code error;
    const auto root = project_file.parent_path();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
        if (error) break;
        const auto relative = entry.path().lexically_relative(root);
        if (!relative.empty() && (relative.begin()->string() == "bin" || relative.begin()->string() == "obj")) continue;
        if (!entry.is_regular_file(error)) continue;
        const auto extension = entry.path().extension().string();
        if (extension == ".cs" || extension == ".csproj" || extension == ".props" || extension == ".targets")
            inputs.push_back(entry.path());
    }
    std::ranges::sort(inputs);
    return inputs;
}

std::string source_fingerprint(const std::filesystem::path& project_file, const std::string_view configuration) {
    const auto inputs=project_source_inputs(project_file);
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](const std::string_view value) {
        for (const auto byte : value) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ULL; }
    };
    const auto mix_file = [&](const std::filesystem::path& root, const std::filesystem::path& input,
                              const std::string_view prefix = {}) {
        const auto relative = input.lexically_relative(root).generic_string();
        mix(prefix); mix(relative);
        const auto content = file_content_fingerprint(input);
        if (!content.empty()) mix(content);
    };
    // The identity is deliberately content- and relative-path-based.  Runtime
    // project snapshots live in a fresh temporary directory on every launch;
    // absolute paths or mtimes would make a safe cross-process cache impossible.
    mix("noemancer-managed-source/2");
    mix(configuration);
    const auto project_root = project_file.parent_path();
    for (const auto& input : inputs) mix_file(project_root, input, "project/");
    // ProjectReference points at the public Managed API.  A project source
    // fingerprint must invalidate when that API changes, even if gameplay C#
    // itself is untouched.  Keep the identity independent of checkout roots.
    const auto managed_root = std::filesystem::path(NOEMANCER_SOURCE_DIR) / "managed" / "Noemancer.Managed";
    for (const auto& input : project_source_inputs(managed_root / "Noemancer.Managed.csproj"))
        mix_file(managed_root, input, "sdk/");
    const auto global_json = std::filesystem::path(NOEMANCER_SOURCE_DIR) / "global.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(global_json, error)) mix_file(global_json.parent_path(), global_json, "sdk/");
    const auto dotnet = std::filesystem::path(NOEMANCER_SOURCE_DIR) / "_tools" / "dotnet" / "dotnet.exe";
    if (std::filesystem::is_regular_file(dotnet, error)) mix_file(dotnet.parent_path(), dotnet, "compiler/");
    // Directory.Build.* files in the project root or its ancestors can alter
    // compilation without appearing in the project-local source walk.
    for (auto directory = project_root; !directory.empty(); directory = directory.parent_path()) {
        for (const auto* name : {"Directory.Build.props", "Directory.Build.targets"}) {
            const auto input = directory / name;
            error.clear();
            if (std::filesystem::is_regular_file(input, error)) mix_file(directory, input, "build/");
        }
        if (directory == directory.root_path()) break;
    }
    std::ostringstream output;
    output << std::hex << hash;
    return output.str();
}

std::filesystem::path managed_cache_entry_root(const std::string_view fingerprint,
                                               const std::string_view configuration) {
    return managed_compile_cache_root() / std::string(fingerprint) / std::string(configuration);
}

std::optional<ManagedCacheEntry> read_managed_cache_entry(const std::string_view fingerprint,
                                                          const std::string_view configuration) {
    if (fingerprint.empty() || (configuration != "Debug" && configuration != "Release")) return std::nullopt;
    if (fingerprint.find_first_not_of("0123456789abcdef") != std::string_view::npos) return std::nullopt;
    const auto root = managed_cache_entry_root(fingerprint, configuration);
    std::ifstream manifest_file(root / "manifest.json", std::ios::binary);
    if (!manifest_file) return std::nullopt;
    std::ostringstream manifest_text;
    manifest_text << manifest_file.rdbuf();
    const auto manifest = Json::parse(manifest_text.str(), nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object() ||
        manifest.value("schema", std::string{}) != "noemancer.managed-compile-cache/0.1" ||
        manifest.value("fingerprint", std::string{}) != fingerprint ||
        manifest.value("configuration", std::string{}) != configuration ||
        !manifest.contains("assembly") || !manifest.at("assembly").is_string() ||
        !manifest.contains("files") || !manifest.at("files").is_array() ||
        manifest.at("files").size() == 0U || manifest.at("files").size() > managed_cache_max_files)
        return std::nullopt;
    ManagedCacheEntry entry{.fingerprint = std::string(fingerprint), .configuration = std::string(configuration),
                            .assembly_relative = manifest.at("assembly").get<std::string>()};
    if (!safe_relative_cache_path(entry.assembly_relative)) return std::nullopt;
    for (const auto& item : manifest.at("files")) {
        if (!item.is_object() || !item.contains("path") || !item.at("path").is_string() ||
            !item.contains("bytes") || !item.at("bytes").is_number_unsigned() ||
            !item.contains("fingerprint") || !item.at("fingerprint").is_string()) return std::nullopt;
        ManagedCacheFile file{.relative = item.at("path").get<std::string>(),
                              .bytes = item.at("bytes").get<std::uintmax_t>(),
                              .fingerprint = item.at("fingerprint").get<std::string>()};
        if (!safe_relative_cache_path(file.relative) || file.bytes > managed_cache_max_bytes ||
            file.fingerprint.empty() || entry.total_bytes > managed_cache_max_bytes - file.bytes) return std::nullopt;
        entry.total_bytes += file.bytes;
        const auto source = root / file.relative;
        std::error_code error;
        if (!std::filesystem::is_regular_file(source, error) || error ||
            std::filesystem::file_size(source, error) != file.bytes || error ||
            file_content_fingerprint(source) != file.fingerprint) return std::nullopt;
        entry.files.push_back(std::move(file));
    }
    if (std::ranges::find(entry.files, entry.assembly_relative, &ManagedCacheFile::relative) == entry.files.end())
        return std::nullopt;
    return entry;
}

bool materialize_managed_cache_entry(const ManagedCacheEntry& entry,
                                     const std::filesystem::path& cache_root,
                                     const std::filesystem::path& output_root) {
    std::error_code error;
    std::filesystem::create_directories(output_root, error);
    if (error) return false;
    for (const auto& file : entry.files) {
        const auto destination = output_root / file.relative;
        const auto relative = destination.lexically_relative(output_root);
        if (!safe_relative_cache_path(relative)) return false;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error || !std::filesystem::copy_file(cache_root / file.relative, destination,
                                                 std::filesystem::copy_options::overwrite_existing, error) || error)
            return false;
        error.clear();
        if (std::filesystem::file_size(destination, error) != file.bytes || error ||
            file_content_fingerprint(destination) != file.fingerprint) return false;
    }
    return std::filesystem::is_regular_file(output_root / entry.assembly_relative, error) && !error;
}

std::optional<ManagedCacheEntry> write_managed_cache_entry(const std::string_view fingerprint,
                                                           const std::string_view configuration,
                                                           const std::filesystem::path& output_root,
                                                           const std::filesystem::path& assembly) {
    if (fingerprint.empty() || (configuration != "Debug" && configuration != "Release")) return std::nullopt;
    std::error_code error;
    const auto relative_assembly = assembly.lexically_relative(output_root);
    if (!safe_relative_cache_path(relative_assembly)) return std::nullopt;
    ManagedCacheEntry entry{.fingerprint = std::string(fingerprint), .configuration = std::string(configuration),
                            .assembly_relative = relative_assembly};
    if (!std::filesystem::is_directory(output_root, error) || error) return std::nullopt;
    for (std::filesystem::recursive_directory_iterator iterator(output_root, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        const auto relative = iterator->path().lexically_relative(output_root);
        const auto bytes = iterator->file_size(error);
        const auto fingerprint_value = file_content_fingerprint(iterator->path());
        if (error || !safe_relative_cache_path(relative) || fingerprint_value.empty() ||
            bytes > managed_cache_max_bytes || entry.files.size() >= managed_cache_max_files ||
            entry.total_bytes > managed_cache_max_bytes - bytes) return std::nullopt;
        entry.total_bytes += bytes;
        entry.files.push_back({relative, bytes, fingerprint_value});
    }
    if (error || entry.files.empty() ||
        std::ranges::find(entry.files, relative_assembly, &ManagedCacheFile::relative) == entry.files.end())
        return std::nullopt;

    const auto final_root = managed_cache_entry_root(fingerprint, configuration);
    const auto temporary_root = final_root.parent_path() /
        (std::string(configuration) + ".tmp-" + std::to_string(next_managed_session.fetch_add(1)));
    std::filesystem::remove_all(temporary_root, error);
    error.clear();
    std::filesystem::create_directories(temporary_root, error);
    if (error) return std::nullopt;
    const auto cleanup = [&] { std::error_code ignored; std::filesystem::remove_all(temporary_root, ignored); };
    for (const auto& file : entry.files) {
        const auto destination = temporary_root / file.relative;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error || !std::filesystem::copy_file(output_root / file.relative, destination,
                                                 std::filesystem::copy_options::overwrite_existing, error) || error) {
            cleanup();
            return std::nullopt;
        }
    }
    const Json manifest = {
        {"schema", "noemancer.managed-compile-cache/0.1"}, {"fingerprint", entry.fingerprint},
        {"configuration", entry.configuration}, {"assembly", entry.assembly_relative.generic_string()},
        {"bytes", entry.total_bytes}, {"files", Json::array()}
    };
    auto writable_manifest = manifest;
    for (const auto& file : entry.files)
        writable_manifest["files"].push_back({{"path", file.relative.generic_string()}, {"bytes", file.bytes},
                                               {"fingerprint", file.fingerprint}});
    std::ofstream manifest_output(temporary_root / "manifest.json", std::ios::binary | std::ios::trunc);
    if (!manifest_output) { cleanup(); return std::nullopt; }
    manifest_output << writable_manifest.dump();
    manifest_output.close();
    if (!manifest_output) { cleanup(); return std::nullopt; }
    std::filesystem::create_directories(final_root.parent_path(), error);
    if (error) { cleanup(); return std::nullopt; }
    if (std::filesystem::exists(final_root, error)) {
        cleanup();
        return read_managed_cache_entry(fingerprint, configuration);
    }
    std::filesystem::rename(temporary_root, final_root, error);
    if (error) { cleanup(); return std::nullopt; }
    prune_managed_compile_cache();
    return entry;
}

std::filesystem::path configured_debug_adapter_path() {
#ifdef _WIN32
    constexpr std::string_view adapter_executable="netcoredbg.exe";
    char* configured_adapter{};
    std::size_t configured_adapter_size{};
    if(_dupenv_s(&configured_adapter,&configured_adapter_size,"NOEMANCER_MANAGED_DEBUG_ADAPTER")==0&&configured_adapter!=nullptr) {
        const std::filesystem::path result{configured_adapter};
        std::free(configured_adapter);
        if(!result.empty()) return result;
    }
#else
    constexpr std::string_view adapter_executable="netcoredbg";
    if(const auto* configured=std::getenv("NOEMANCER_MANAGED_DEBUG_ADAPTER");configured!=nullptr&&*configured!='\0')
        return std::filesystem::path(configured);
#endif
    return std::filesystem::path(NOEMANCER_SOURCE_DIR)/"_tools/netcoredbg"/adapter_executable;
}

bool debug_payload_path_key(const std::string_view key) {
    return key=="path"||key=="program"||key=="sourceRoot"||key=="projectAssembly"||key=="runtimeConfig"||
        key=="managedAssembly"||key=="adapter"||key=="stderr"||key.ends_with("Path")||key.ends_with("path");
}

Json sanitize_debug_payload(const Json& value) {
    if(value.is_array()) {
        Json result=Json::array();
        for(const auto& item:value) result.push_back(sanitize_debug_payload(item));
        return result;
    }
    if(!value.is_object()) return value;
    Json result=Json::object();
    for(const auto& [key,item]:value.items()) {
        if(debug_payload_path_key(key)) continue;
        result[key]=sanitize_debug_payload(item);
    }
    return result;
}

Json parse_debug_json(const std::string& value) {
    const auto parsed=Json::parse(value,nullptr,false);
    return parsed.is_discarded()?Json::object():sanitize_debug_payload(parsed);
}

std::string debug_session_state_or_null(const ManagedDebugSession* session) {
    if(session==nullptr) return "null";
    return sanitize_debug_payload(Json::parse(session->state_json(),nullptr,false)).dump();
}

#ifdef _WIN32
struct ProcessResult final { bool started{}; std::uint32_t exit_code{}; std::string output; std::string error; };

ProcessResult run_process(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
                          const std::filesystem::path& working_directory) {
    ProcessResult result;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe{};
    HANDLE write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        result.error = "Unable to create compiler output pipe: " + std::to_string(GetLastError());
        return result;
    }
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        result.error = "Unable to secure compiler output pipe: " + std::to_string(GetLastError());
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return result;
    }
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    for (const auto& argument : arguments) command += L" \"" + argument + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const auto started = CreateProcessW(executable.wstring().c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, working_directory.wstring().c_str(), &startup, &process);
    CloseHandle(write_pipe);
    if (!started) {
        result.error = "CreateProcessW failed: " + std::to_string(GetLastError());
        CloseHandle(read_pipe);
        return result;
    }
    result.started = true;
    std::thread reader([&] {
        std::array<char, 4096> buffer{};
        DWORD read{};
        while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
            if (result.output.size() < 4U * 1024U * 1024U) result.output.append(buffer.data(), read);
        }
    });
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code{};
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = exit_code;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    reader.join();
    CloseHandle(read_pipe);
    return result;
}
#endif

Json compiler_diagnostics(const std::string& output) {
    Json diagnostics = Json::array();
    static const std::regex pattern(R"(^(.+?)\((\d+),(\d+)\):\s+(error|warning)\s+([A-Za-z]+\d+):\s+(.+?)(?:\s+\[(.+)\])?$)");
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line) && diagnostics.size() < 256U) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        if (std::regex_match(line, match, pattern)) {
            diagnostics.push_back({{"severity", match[4].str()}, {"code", match[5].str()}, {"message", match[6].str()},
                {"file", std::filesystem::path(match[1].str()).generic_string()}, {"line", std::stoi(match[2].str())},
                {"column", std::stoi(match[3].str())}, {"project", match[7].matched ? match[7].str() : ""}});
        } else if (line.find(": error ") != std::string::npos || line.starts_with("error ")) {
            diagnostics.push_back({{"severity", "error"}, {"code", "dotnet.unparsed-diagnostic"}, {"message", line}});
        }
    }
    return diagnostics;
}

#ifdef _WIN32
using hostfxr_handle = void*;
using hostfxr_initialize_for_runtime_config_fn = int(__cdecl*)(const wchar_t*, const void*, hostfxr_handle*);
using hostfxr_get_runtime_delegate_fn = int(__cdecl*)(hostfxr_handle, int, void**);
using hostfxr_close_fn = int(__cdecl*)(hostfxr_handle);
using load_assembly_and_get_function_pointer_fn = int(__cdecl*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
using managed_entry_fn = int(__cdecl*)(const std::uint8_t*, int, std::uint8_t*, int);
constexpr int load_assembly_and_get_function_pointer_delegate = 5;

std::optional<Json> call_managed_entry(void* raw_entry,const std::string_view request) {
    if(raw_entry==nullptr)return std::nullopt;
    constexpr std::size_t maximum_response_size=1024U*1024U;
    std::vector<std::uint8_t> response(64U*1024U);
    auto entry=reinterpret_cast<managed_entry_fn>(raw_entry);
    auto written=entry(reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<int>(request.size()),
        response.data(),static_cast<int>(response.size()));
    if(written<0&&written!=std::numeric_limits<int>::min()&&static_cast<std::size_t>(-written)<=maximum_response_size) {
        response.resize(static_cast<std::size_t>(-written));
        written=entry(reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<int>(request.size()),
            response.data(),static_cast<int>(response.size()));
    }
    if(written<0||static_cast<std::size_t>(written)>response.size())return std::nullopt;
    auto parsed=Json::parse(std::string_view(reinterpret_cast<const char*>(response.data()),static_cast<std::size_t>(written)),nullptr,false);
    return parsed.is_discarded()?std::nullopt:std::optional<Json>{std::move(parsed)};
}
#endif
}

ManagedScriptRuntime::ManagedScriptRuntime():session_id_("managed-session-"+std::to_string(next_managed_session.fetch_add(1))) {}

ManagedScriptRuntime::~ManagedScriptRuntime() {
    if(debug_session_!=nullptr) debug_session_->shutdown();
#ifdef _WIN32
    if(managed_entry_!=nullptr) {
        try {
            const auto request=Json{{"operation","session.release"},{"sessionId",session_id_}}.dump();
            std::array<std::uint8_t,1024> response{};
            static_cast<void>(reinterpret_cast<managed_entry_fn>(managed_entry_)(
                reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<int>(request.size()),response.data(),static_cast<int>(response.size())));
        } catch (...) {
            // Destruction is a best-effort boundary and must never terminate the host process.
        }
    }
    if (hostfxr_module_ != nullptr) FreeLibrary(static_cast<HMODULE>(hostfxr_module_));
#endif
}

void ManagedScriptRuntime::ensure_host() const {
    if (host_attempted_) return;
    host_attempted_ = true;
    const auto probe = hostfxr_probe();
    hostfxr_path_ = probe.value("libraryPath", "");
    if (!probe.value("compatible", false)) {
        host_status_ = "hostfxr-incompatible";
        host_error_ = probe.value("diagnostic", "hostfxr-not-found");
        return;
    }
    const auto runtime_config = newest_managed_artifact("Noemancer.ManagedHost.runtimeconfig.json");
    const auto assembly = newest_managed_artifact("Noemancer.ManagedHost.dll");
    runtime_config_path_ = runtime_config.generic_string();
    managed_assembly_path_ = assembly.generic_string();
    if (runtime_config.empty() || assembly.empty()) {
        host_status_ = "managed-host-artifacts-missing";
        host_error_ = "Build the noemancer_managed_host target after installing the pinned .NET SDK.";
        return;
    }
#ifdef _WIN32
    const auto dotnet_root=std::filesystem::path(hostfxr_path_).parent_path().parent_path().parent_path().parent_path().wstring();
    if (std::filesystem::is_directory(dotnet_root)) _wputenv_s(L"DOTNET_ROOT", dotnet_root.c_str());
    const auto module = LoadLibraryW(std::filesystem::path(hostfxr_path_).wstring().c_str());
    if (module == nullptr) {
        host_status_ = "hostfxr-load-failed";
        host_error_ = "LoadLibraryW failed with " + std::to_string(GetLastError());
        return;
    }
    hostfxr_module_ = module;
    const auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(GetProcAddress(module, "hostfxr_initialize_for_runtime_config"));
    const auto get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(module, "hostfxr_get_runtime_delegate"));
    const auto close = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(module, "hostfxr_close"));
    if (initialize == nullptr || get_delegate == nullptr || close == nullptr) {
        host_status_ = "hostfxr-symbol-missing";
        host_error_ = "Required HostFXR exports were not found.";
        return;
    }
    hostfxr_handle context{};
    const auto initialize_result = initialize(runtime_config.wstring().c_str(), nullptr, &context);
    if (initialize_result < 0 || context == nullptr) {
        host_status_ = "runtime-initialize-failed";
        host_error_ = result_code(initialize_result);
        return;
    }
    void* loader{};
    const auto delegate_result = get_delegate(context, load_assembly_and_get_function_pointer_delegate, &loader);
    close(context);
    if (delegate_result < 0 || loader == nullptr) {
        host_status_ = "runtime-delegate-failed";
        host_error_ = result_code(delegate_result);
        return;
    }
    void* entry{};
    const auto unmanaged_callers_only = reinterpret_cast<const wchar_t*>(static_cast<std::intptr_t>(-1));
    const auto load = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loader);
    const auto load_result = load(assembly.wstring().c_str(), L"Noemancer.ManagedHost.EntryPoint, Noemancer.ManagedHost",
                                  L"Invoke", unmanaged_callers_only, nullptr, &entry);
    if (load_result < 0 || entry == nullptr) {
        host_status_ = "managed-entry-load-failed";
        host_error_ = result_code(load_result);
        return;
    }
    managed_entry_ = entry;
    host_ready_ = true;
    host_status_ = "ready";
#else
    host_status_ = "platform-loader-pending";
    host_error_ = "The first executable HostFXR vertical is implemented for Windows.";
#endif
}

std::string ManagedScriptRuntime::abi_json() const {
    ensure_host();
    const auto probe = hostfxr_probe();
    return Json{{"schemaVersion", "noemancer.managed-script-abi/0.5"}, {"backend", "coreclr-hostfxr/1.0"},
        {"language", "C#"}, {"targetFramework", "net10.0"}, {"isolation", "default-bootstrap+collectible-project-alc"},
        {"valueBoundary", Json::array({"bool", "i32", "i64", "f32", "f64", "utf8", "entity-id", "asset-id", "json-value"})},
        {"callbacks", Json::array({"OnCreate", "OnFixedUpdate", "OnUpdate", "OnContactEnter", "OnContactStay", "OnContactExit",
            "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit", "OnUiAction", "OnDestroy"})},
        {"commandBuffer", Json::array({"scene.transform.set-position","scene.property.set","sprite.playback.set","audio.voice.play","gameplay.persistence.request","gameplay.event.emit","gameplay.prefab.spawn","gameplay.entity.despawn","gameplay.tag.set"})},
        {"queryBoundary",{{"entityLimit",256},{"stableIds",true},{"nativeHandles",false},{"truncationReported",true}}},
        {"forbiddenAcrossAbi", Json::array({"Flecs entity handle", "SDL handle", "GPU handle", "Jolt BodyID", "native pointer"})},
        {"hostfxr", probe}, {"status", host_status_}, {"ready", host_ready_}, {"error", host_error_},
        {"runtimeConfig", runtime_config_path_}, {"managedAssembly", managed_assembly_path_},{"sessionId",session_id_}}.dump();
}

std::string ManagedScriptRuntime::observe_json() const {
    ensure_host();
    Json instances = Json::array();
    for (const auto& instance : instances_) instances.push_back({{"id", instance.id}, {"entityId", instance.entity_id},
        {"assemblyAsset", instance.assembly_asset}, {"typeName", instance.type_name}, {"state", instance.state},
        {"publicState",Json::parse(instance.public_state_json,nullptr,false)},
        {"lastCallback", instance.last_callback}, {"callbackCount", instance.callback_count},
        {"properties",Json::parse(instance.properties_json)},{"sceneOwned",instance.scene_owned}});
    auto managed_result = Json::parse(last_managed_result_, nullptr, false);
    if (managed_result.is_discarded()) managed_result = nullptr;
    auto compile = Json::parse(last_compile_result_, nullptr, false);
    if (compile.is_discarded()) compile = nullptr;
    return Json{{"schemaVersion", "noemancer.managed-script-host/0.3"}, {"revision", revision_},
        {"backend", "coreclr-hostfxr/1.0"}, {"status", host_status_}, {"ready", host_ready_},
        {"sessionId",session_id_},
        {"hostfxrPath", hostfxr_path_}, {"runtimeConfig", runtime_config_path_}, {"managedAssembly", managed_assembly_path_},
        {"lastManagedResult", std::move(managed_result)}, {"lastCompile", std::move(compile)},
        {"instances", std::move(instances)}}.dump();
}

std::string ManagedScriptRuntime::attach_json(const std::string_view instance_id, const std::string_view entity_id,
                                               const std::string_view assembly_asset, const std::string_view type_name) {
    const auto valid = !instance_id.empty() && !entity_id.empty() && !assembly_asset.empty() && !type_name.empty() &&
        std::ranges::none_of(instances_, [&](const auto& value) { return value.id == instance_id; });
    if (valid) {
        instances_.push_back({std::string(instance_id), std::string(entity_id), std::string(assembly_asset), std::string(type_name)});
        ++revision_;
    }
    return Json{{"schemaVersion", "noemancer.script-action-receipt/0.2"}, {"success", valid},
        {"code", valid ? "ok" : "scripting.invalid-or-duplicate-instance"}, {"operation", "instance.attach"}, {"revision", revision_}}.dump();
}

std::string ManagedScriptRuntime::invoke_json(const std::string_view instance_id, const std::string_view callback,
                                               const std::string_view arguments_json,const std::string_view context_json) {
    static constexpr std::array<std::string_view, 11> callbacks{"OnCreate", "OnFixedUpdate", "OnUpdate",
        "OnContactEnter", "OnContactStay", "OnContactExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit", "OnUiAction", "OnDestroy"};
    auto instance = std::ranges::find(instances_, instance_id, &ManagedScriptInstance::id);
    const auto arguments = Json::parse(arguments_json, nullptr, false);
    const auto context = Json::parse(context_json, nullptr, false);
    const auto valid = instance != instances_.end() && std::ranges::find(callbacks, callback) != callbacks.end() &&
        !arguments.is_discarded() && arguments.is_object() && !context.is_discarded() && context.is_object();
    ensure_host();
    bool executed{};
    bool managed_success{};
    Json managed_result;
#ifdef _WIN32
    if (valid && host_ready_) {
        const auto request = Json{{"operation","lifecycle.invoke"},{"sessionId",session_id_},{"instanceId", instance->id}, {"entityId", instance->entity_id},
            {"assemblyAsset", instance->assembly_asset}, {"typeName", instance->type_name},
            {"callback", callback}, {"arguments", arguments}, {"projectAssembly", project_assembly_path_},
            {"projectFingerprint", project_build_fingerprint_}, {"context", context}}.dump();
        auto entry = reinterpret_cast<managed_entry_fn>(managed_entry_);
        constexpr std::size_t maximum_response_size = 1024U * 1024U;
        std::vector<std::uint8_t> response(64U * 1024U);
        auto written = entry(reinterpret_cast<const std::uint8_t*>(request.data()), static_cast<int>(request.size()),
                             response.data(), static_cast<int>(response.size()));
        if (written < 0 && written != std::numeric_limits<int>::min() &&
            static_cast<std::size_t>(-written) <= maximum_response_size) {
            response.resize(static_cast<std::size_t>(-written));
            written = entry(reinterpret_cast<const std::uint8_t*>(request.data()), static_cast<int>(request.size()),
                            response.data(), static_cast<int>(response.size()));
        }
        executed = true;
        if (written >= 0 && static_cast<std::size_t>(written) <= response.size()) {
            last_managed_result_.assign(reinterpret_cast<const char*>(response.data()), static_cast<std::size_t>(written));
            managed_result = Json::parse(last_managed_result_, nullptr, false);
            managed_success = !managed_result.is_discarded() && managed_result.value("success", false);
        }
    }
#endif
    const auto success = valid && executed && managed_success;
    if (success) {
        instance->last_callback = callback;
        ++instance->callback_count;
        instance->state = callback == "OnDestroy" ? "destroyed" : "active";
        const auto public_state=managed_result.value("state",Json::object());
        instance->public_state_json=public_state.is_object()?public_state.dump():std::string{"{}"};
        ++revision_;
    } else if(valid&&executed) {
        instance->last_callback=callback;
        ++instance->callback_count;
        instance->state="faulted";
        ++revision_;
    }
    if(success&&callback=="OnDestroy")instances_.erase(instance);
    if (managed_result.is_discarded() || managed_result.is_null()) managed_result = nullptr;
    const auto code = !valid ? "scripting.invalid-invocation" : !host_ready_ ? "scripting.host-not-ready" :
        !managed_success ? "scripting.managed-callback-failed" : "ok";
    return Json{{"schemaVersion", "noemancer.script-action-receipt/0.2"}, {"success", success}, {"code", code},
        {"operation", "lifecycle.invoke"}, {"instanceId", instance_id}, {"callback", callback}, {"revision", revision_},
        {"executedManagedCode", executed}, {"managedResult", std::move(managed_result)}, {"hostStatus", host_status_}}.dump();
}

std::string ManagedScriptRuntime::configure_project_json(const std::filesystem::path& project_root,
                                                          const std::filesystem::path& script_project) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(project_root, error);
    const auto canonical_project = std::filesystem::weakly_canonical(script_project.is_absolute() ? script_project : canonical_root / script_project, error);
    const auto relative = canonical_project.lexically_relative(canonical_root);
    const auto inside_root = !canonical_root.empty() && !canonical_project.empty() && !relative.empty() &&
        relative.begin()->string() != "..";
    const auto valid = inside_root && canonical_project.extension() == ".csproj" &&
        std::filesystem::is_regular_file(canonical_project, error);
    if (valid) {
        project_root_ = canonical_root;
        script_project_ = canonical_project;
        last_compile_result_.clear();
        compile_fingerprint_.clear();
        project_assembly_path_.clear();
        project_build_fingerprint_.clear();
        type_catalog_fingerprint_.clear();
        type_catalog_json_.clear();
        observed_source_fingerprint_.clear();
        source_probe_time_={};
        ++compile_revision_;
    }
    return Json{{"schemaVersion", "noemancer.script-project-action/0.1"}, {"success", valid},
        {"code", valid ? "ok" : "scripting.invalid-or-unsafe-project"}, {"operation", "project.configure"},
        {"projectRoot", valid ? canonical_root.generic_string() : ""},
        {"scriptProject", valid ? canonical_project.generic_string() : ""}, {"revision", compile_revision_}}.dump();
}

std::string ManagedScriptRuntime::load_project_assembly_json(const std::filesystem::path& assembly,
                                                              const std::string_view configuration) {
    std::error_code error;const auto canonical=std::filesystem::weakly_canonical(assembly,error);
    const auto valid_configuration=configuration=="Debug"||configuration=="Release";
    const auto valid=valid_configuration&&!canonical.empty()&&canonical.extension()==".dll"&&std::filesystem::is_regular_file(canonical,error);
    if(valid) {
        project_root_=canonical.parent_path();script_project_.clear();project_assembly_path_=canonical.generic_string();
        project_build_fingerprint_=assembly_fingerprint(canonical);compile_fingerprint_=project_build_fingerprint_;
        observed_source_fingerprint_=project_build_fingerprint_;last_configuration_=std::string(configuration);
        type_catalog_fingerprint_.clear();type_catalog_json_.clear();last_compile_result_=Json{{"schemaVersion","noemancer.script-compile-result/0.1"},
            {"success",true},{"code","scripting.prebuilt-assembly-loaded"},{"configuration",configuration},{"cacheHit",true},
            {"cacheScope","package"},{"fingerprint",project_build_fingerprint_},{"assembly",project_assembly_path_},{"revision",++compile_revision_}}.dump();
    }
    return Json{{"schemaVersion","noemancer.script-project-action/0.1"},{"success",valid},
        {"code",valid?"ok":!valid_configuration?"scripting.invalid-configuration":"scripting.assembly-not-found"},
        {"operation","project.load-prebuilt"},{"assembly",valid?canonical.generic_string():std::string{}},
        {"configuration",configuration},{"revision",compile_revision_}}.dump();
}

std::string ManagedScriptRuntime::compile_project_json(const std::string_view configuration) {
    const auto compile_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
        return std::round(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compile_started).count() * 100.0) / 100.0;
    };
    const auto valid_configuration = configuration == "Debug" || configuration == "Release";
    if (script_project_.empty() || !valid_configuration) {
        return Json{{"schemaVersion", "noemancer.script-compile-result/0.1"}, {"success", false},
            {"code", script_project_.empty() ? "scripting.project-not-configured" : "scripting.invalid-configuration"},
            {"configuration", configuration}, {"cacheHit", false}, {"durationMs", elapsed_ms()}, {"diagnostics", Json::array()}}.dump();
    }
    const auto fingerprint = source_fingerprint(script_project_, configuration);
    last_configuration_=std::string(configuration);
    observed_source_fingerprint_=fingerprint;
    source_probe_time_=std::chrono::steady_clock::now();
    if (fingerprint == compile_fingerprint_ && !last_compile_result_.empty()) {
        auto cached = Json::parse(last_compile_result_);
        cached["cacheHit"] = true;
        cached["cacheScope"] = "runtime-instance";
        cached["durationMs"] = elapsed_ms();
        return cached.dump();
    }
    const auto cache_key=script_project_.generic_string()+":"+fingerprint;
    {
        std::scoped_lock lock(process_compile_cache_mutex);
        const auto cached=process_compile_cache.find(cache_key);
        if(cached!=process_compile_cache.end()&&std::filesystem::is_regular_file(cached->second.assembly)) {
            auto result=Json::parse(cached->second.result_json);
            result["cacheHit"]=true;result["cacheScope"]="process";result["fingerprint"]=fingerprint;
            result["assembly"]=cached->second.assembly;result["revision"]=++compile_revision_;result["durationMs"]=elapsed_ms();
            compile_fingerprint_=fingerprint;project_build_fingerprint_=fingerprint;
            project_assembly_path_=cached->second.assembly;last_compile_result_=result.dump();
            if(type_catalog_fingerprint_!=fingerprint)type_catalog_json_.clear();
            return last_compile_result_;
        }
    }
    const auto output_root = script_project_.parent_path() / "bin" / std::string(configuration);
    if (const auto cached = read_managed_cache_entry(fingerprint, configuration); cached) {
        const auto cache_root = managed_cache_entry_root(fingerprint, configuration);
        if (materialize_managed_cache_entry(*cached, cache_root, output_root)) {
            Json result{{"schemaVersion", "noemancer.script-compile-result/0.1"}, {"success", true},
                {"code", "scripting.managed-cache-hit"}, {"configuration", configuration}, {"cacheHit", true},
                {"cacheScope", "disk"}, {"fingerprint", fingerprint},
                {"cacheArtifact", {{"schema", "noemancer.managed-compile-cache/0.1"},
                                    {"fileCount", cached->files.size()}, {"bytes", cached->total_bytes}}},
                {"project", script_project_.generic_string()},
                {"assembly", (output_root / cached->assembly_relative).generic_string()},
                {"diagnostics", Json::array()}, {"output", ""}, {"revision", ++compile_revision_},
                {"durationMs", elapsed_ms()}};
            compile_fingerprint_=fingerprint;project_build_fingerprint_=fingerprint;
            project_assembly_path_=result.at("assembly").get<std::string>();last_compile_result_=result.dump();
            if(type_catalog_fingerprint_!=fingerprint)type_catalog_json_.clear();
            {
                std::scoped_lock lock(process_compile_cache_mutex);
                process_compile_cache[cache_key]={last_compile_result_,project_assembly_path_};
            }
            return last_compile_result_;
        }
    }
    const auto dotnet = std::filesystem::path(NOEMANCER_SOURCE_DIR) / "_tools/dotnet/dotnet.exe";
    Json result{{"schemaVersion", "noemancer.script-compile-result/0.1"}, {"success", false},
        {"code", "scripting.dotnet-sdk-not-found"}, {"configuration", configuration}, {"cacheHit", false},
        {"cacheScope", "none"},
        {"fingerprint", fingerprint}, {"project", script_project_.generic_string()}, {"diagnostics", Json::array()},
        {"output", ""}, {"assembly", ""}, {"revision", compile_revision_}, {"durationMs", 0.0},
        {"buildDurationMs", 0.0}, {"cacheArtifact", nullptr}};
#ifdef _WIN32
    if (std::filesystem::is_regular_file(dotnet)) {
        const auto build_started = std::chrono::steady_clock::now();
        const auto process = run_process(dotnet, {L"build", script_project_.wstring(), L"--configuration",
            std::wstring(configuration.begin(), configuration.end()), L"--nologo", L"--tl:off", L"--verbosity:minimal",
            L"-p:GenerateFullPaths=true", L"-p:PreferredUILang=en-US",
            L"-p:NoemancerSdkRoot=" + std::filesystem::path(NOEMANCER_SOURCE_DIR).wstring()}, script_project_.parent_path());
        result["buildDurationMs"] = std::round(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_started).count() * 100.0) / 100.0;
        result["exitCode"] = process.exit_code;
        result["output"] = process.output.size() <= 64U * 1024U ? process.output : process.output.substr(process.output.size() - 64U * 1024U);
        result["diagnostics"] = compiler_diagnostics(process.output);
        if (!process.started) {
            result["code"] = "scripting.compiler-start-failed";
            result["diagnostics"].push_back({{"severity", "error"}, {"code", "native.process-start"}, {"message", process.error}});
        } else if (process.exit_code != 0) {
            result["code"] = "scripting.compile-failed";
        } else {
            std::filesystem::path assembly;
            std::filesystem::file_time_type newest{};
            std::error_code error;
            const auto bin_root = script_project_.parent_path() / "bin" / configuration;
            const auto expected_assembly = script_project_.stem().string() + ".dll";
            if (std::filesystem::is_directory(bin_root, error)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(bin_root, error)) {
                    if (!entry.is_regular_file(error) || entry.path().filename() != expected_assembly) continue;
                    const auto relative = entry.path().lexically_relative(bin_root).generic_string();
                    if (relative.find("/ref/") != std::string::npos || relative.find("/refint/") != std::string::npos) continue;
                    const auto time = entry.last_write_time(error);
                    if (assembly.empty() || (!error && time > newest)) { assembly = entry.path(); newest = time; }
                }
            }
            result["success"] = !assembly.empty();
            result["code"] = assembly.empty() ? "scripting.assembly-not-found" : "ok";
            result["assembly"] = assembly.generic_string();
            if (!assembly.empty()) {
                project_assembly_path_ = assembly.generic_string();
                project_build_fingerprint_ = fingerprint;
                if (const auto cache = write_managed_cache_entry(fingerprint, configuration, bin_root, assembly); cache) {
                    result["cacheArtifact"] = {{"schema", "noemancer.managed-compile-cache/0.1"},
                        {"fileCount", cache->files.size()}, {"bytes", cache->total_bytes}};
                    result["cacheScope"] = "disk-write";
                } else {
                    result["cacheScope"] = "none";
                }
            }
        }
    }
#else
    result["code"] = "scripting.compiler-platform-pending";
#endif
    ++compile_revision_;
    result["revision"] = compile_revision_;
    result["durationMs"] = elapsed_ms();
    compile_fingerprint_ = fingerprint;
    last_compile_result_ = result.dump();
    if(result.value("success",false)&&!project_assembly_path_.empty()) {
        std::scoped_lock lock(process_compile_cache_mutex);
        process_compile_cache[cache_key]={last_compile_result_,project_assembly_path_};
    }
    return last_compile_result_;
}

std::string ManagedScriptRuntime::discover_project_types_json() const {
    if(project_assembly_path_.empty()||project_build_fingerprint_.empty())
        return Json{{"schemaVersion","noemancer.managed-type-catalog/0.1"},{"success",false},
            {"code","scripting.project-not-built"},{"types",Json::array()},{"typeCount",0}}.dump();
    if(type_catalog_fingerprint_==project_build_fingerprint_&&!type_catalog_json_.empty())return type_catalog_json_;
    ensure_host();
    if(!host_ready_)return Json{{"schemaVersion","noemancer.managed-type-catalog/0.1"},{"success",false},
        {"code","scripting.host-not-ready"},{"types",Json::array()},{"typeCount",0}}.dump();
#ifdef _WIN32
    const auto request=Json{{"operation","project.inspect"},{"sessionId",session_id_},
        {"projectAssembly",project_assembly_path_},{"projectFingerprint",project_build_fingerprint_}}.dump();
    auto entry=reinterpret_cast<managed_entry_fn>(managed_entry_);constexpr std::size_t maximum_response_size=1024U*1024U;
    std::vector<std::uint8_t> response(64U*1024U);
    auto written=entry(reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<int>(request.size()),response.data(),static_cast<int>(response.size()));
    if(written<0&&written!=std::numeric_limits<int>::min()&&static_cast<std::size_t>(-written)<=maximum_response_size) {
        response.resize(static_cast<std::size_t>(-written));
        written=entry(reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<int>(request.size()),response.data(),static_cast<int>(response.size()));
    }
    if(written>=0&&static_cast<std::size_t>(written)<=response.size()) {
        type_catalog_json_.assign(reinterpret_cast<const char*>(response.data()),static_cast<std::size_t>(written));
        const auto parsed=Json::parse(type_catalog_json_,nullptr,false);
        if(!parsed.is_discarded()&&parsed.value("success",false)){type_catalog_fingerprint_=project_build_fingerprint_;return type_catalog_json_;}
    }
#endif
    return Json{{"schemaVersion","noemancer.managed-type-catalog/0.1"},{"success",false},
        {"code","scripting.type-discovery-failed"},{"types",Json::array()},{"typeCount",0}}.dump();
}

void ManagedScriptRuntime::refresh_source_probe() const {
    if(script_project_.empty())return;
    const auto now=std::chrono::steady_clock::now();
    if(source_probe_time_.time_since_epoch().count()!=0&&now-source_probe_time_<std::chrono::milliseconds(500))return;
    observed_source_fingerprint_=source_fingerprint(script_project_,last_configuration_);
    source_probe_time_=now;
}

std::string ManagedScriptRuntime::project_observe_json() const {
    refresh_source_probe();
    auto compile = Json::parse(last_compile_result_, nullptr, false);
    if (compile.is_discarded()) compile = nullptr;
    // A failed attempt must not make the source appear up to date. The last
    // attempted fingerprint remains diagnostic evidence; only a successful
    // build advances the candidate fingerprint.
    const auto needs_compile=!script_project_.empty()&&observed_source_fingerprint_!=project_build_fingerprint_;
    const auto needs_reload=!script_project_.empty()&&observed_source_fingerprint_!=project_build_fingerprint_;
    Json sources=Json::array();std::size_t source_count{};constexpr std::size_t source_limit=256U;
    if(!script_project_.empty())for(const auto& source:project_source_inputs(script_project_)) {
        if(source.extension()!=".cs")continue;++source_count;
        if(sources.size()>=source_limit)continue;
        std::error_code error;const auto relative=source.lexically_relative(project_root_);
        const auto bytes=std::filesystem::file_size(source,error);
        sources.push_back({{"path",relative.generic_string()},{"language","csharp"},
            {"bytes",error?0U:bytes}});
    }
    return Json{{"schemaVersion", "noemancer.script-project-state/0.3"}, {"configured", !script_project_.empty()},
        {"projectRoot", project_root_.generic_string()}, {"scriptProject", script_project_.generic_string()},
        {"loadedCandidateAssembly", project_assembly_path_}, {"loadedCandidateFingerprint", project_build_fingerprint_},
        {"sourceDocuments",{{"items",std::move(sources)},{"count",source_count},{"truncated",source_count>source_limit}}},
        {"sourceState",{{"configuration",last_configuration_},{"observedFingerprint",observed_source_fingerprint_},
            {"lastAttemptFingerprint",compile_fingerprint_},{"needsCompile",needs_compile},{"needsReload",needs_reload},
            {"probeIntervalMilliseconds",500}}},
        {"revision", compile_revision_}, {"lastCompile", std::move(compile)}}.dump();
}

std::string ManagedScriptRuntime::state_capture_json() const {
    ensure_host();Json entries=Json::array();Json errors=Json::array();
    for(const auto& instance:instances_) {
        if(instance.state=="destroyed"||instance.state=="faulted")continue;
#ifdef _WIN32
        const auto request=Json{{"operation","state.capture"},{"sessionId",session_id_},{"instanceId",instance.id}}.dump();
        const auto response=host_ready_?call_managed_entry(managed_entry_,request):std::nullopt;
        if(response&&(response->value("success",false)||response->value("code",std::string{})=="scripting.instance-not-created"))
            entries.push_back({{"instanceId",instance.id},{"entityId",instance.entity_id},
                {"typeName",instance.type_name},{"state",response->value("state",Json::object())}});
        else errors.push_back({{"instanceId",instance.id},{"code",response?response->value("code",std::string("scripting.state-capture-failed")):
            std::string("scripting.host-not-ready")}});
#else
        errors.push_back({{"instanceId",instance.id},{"code","scripting.compiler-platform-pending"}});
#endif
    }
    return Json{{"schemaVersion","noemancer.script-state/0.1"},{"success",errors.empty()},
        {"instanceCount",entries.size()},{"instances",std::move(entries)},{"errors",std::move(errors)}}.dump();
}

std::string ManagedScriptRuntime::state_restore_json(const std::string_view document_json) {
    const auto document=Json::parse(document_json,nullptr,false);Json errors=Json::array(),receipts=Json::array();
    if(document.is_discarded()||!document.is_object()||document.value("schemaVersion",std::string{})!="noemancer.script-state/0.1"||
       !document.contains("instances")||!document.at("instances").is_array()||document.at("instances").size()>256U)
        return Json{{"schemaVersion","noemancer.script-state-restore/0.1"},{"success",false},{"code","scripting.invalid-state-document"},
            {"restoredCount",0},{"errors",Json::array()}}.dump();
    ensure_host();
    for(std::size_t index=0;index<document.at("instances").size();++index) {
        const auto& saved=document.at("instances").at(index);
        if(!saved.is_object()||!saved.contains("instanceId")||!saved.at("instanceId").is_string()||
           !saved.contains("entityId")||!saved.at("entityId").is_string()||!saved.contains("typeName")||
           !saved.at("typeName").is_string()||!saved.contains("state")||!saved.at("state").is_object()) {
            errors.push_back({{"index",index},{"code","scripting.invalid-state-entry"}});continue;}
        const auto id=saved.at("instanceId").get<std::string>();const auto current=std::ranges::find(instances_,id,&ManagedScriptInstance::id);
        if(current==instances_.end()||current->entity_id!=saved.at("entityId").get<std::string>()||
           current->type_name!=saved.at("typeName").get<std::string>()) {
            errors.push_back({{"index",index},{"instanceId",id},{"code","scripting.state-instance-mismatch"}});continue;}
#ifdef _WIN32
        const auto request=Json{{"operation","state.restore"},{"sessionId",session_id_},{"instanceId",id},{"state",saved.at("state")}}.dump();
        const auto response=host_ready_?call_managed_entry(managed_entry_,request):std::nullopt;
        if(!response||!response->value("success",false))errors.push_back({{"index",index},{"instanceId",id},
            {"code",response?response->value("code",std::string("scripting.state-restore-failed")):std::string("scripting.host-not-ready")}});
        else {if(response->contains("publicState")&&response->at("publicState").is_object())current->public_state_json=response->at("publicState").dump();
            receipts.push_back(*response);}
#endif
    }
    if(errors.empty())++revision_;
    return Json{{"schemaVersion","noemancer.script-state-restore/0.1"},{"success",errors.empty()},
        {"code",errors.empty()?"ok":"scripting.state-restore-failed"},{"restoredCount",receipts.size()},
        {"receipts",std::move(receipts)},{"errors",std::move(errors)}}.dump();
}

std::string ManagedScriptRuntime::debug_attach_json() const {
    ensure_host();
#ifdef _WIN32
    const auto process_id=static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process_id=static_cast<std::uint64_t>(getpid());
#endif
    std::filesystem::path adapter;
    adapter=configured_debug_adapter_path();
    std::error_code error;
    const auto adapter_ready=std::filesystem::is_regular_file(adapter,error);
    const auto assembly_ready=!project_assembly_path_.empty()&&std::filesystem::is_regular_file(project_assembly_path_,error);
    const auto symbols=assembly_ready?std::filesystem::path(project_assembly_path_).replace_extension(".pdb"):std::filesystem::path{};
    const auto symbols_ready=!symbols.empty()&&std::filesystem::is_regular_file(symbols,error);
    const auto target_ready=host_ready_&&assembly_ready&&symbols_ready;
    const auto ready=target_ready&&adapter_ready;
    const auto code=!host_ready_?"scripting.host-not-ready":!assembly_ready?"scripting.assembly-not-built":
        !symbols_ready?"scripting.symbols-not-found":!adapter_ready?"scripting.debug-adapter-not-installed":"ok";
    return Json{{"schemaVersion","noemancer.managed-debug-attach/0.1"},{"ready",ready},{"code",code},
        {"targetReady",target_ready},{"adapterReady",adapter_ready},{"processId",process_id},{"runtime","coreclr"},
        {"targetFramework","net10.0"},{"configuration",last_configuration_},{"projectAssembly",project_assembly_path_},
        {"symbols",symbols.generic_string()},{"symbolsAvailable",symbols_ready},{"adapter",{{"kind","netcoredbg"},
            {"path",adapter.generic_string()},{"protocol","debug-adapter-protocol"}}},
        {"request",{{"command","attach"},{"processId",process_id},{"justMyCode",false}}},
        {"sourceRoot",project_root_.generic_string()},{"sessionId",session_id_}}.dump();
}

std::string ManagedScriptRuntime::debug_session_start_json() {
    const auto operation="debug.session.start";
    const auto adapter=configured_debug_adapter_path();
    std::error_code error;
    if(!std::filesystem::is_regular_file(adapter,error))
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",false},
            {"code","scripting.debug-adapter-not-installed"},{"operation",operation},{"state",nullptr}}.dump();
    if(debug_session_!=nullptr&&debug_session_->active())
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",true},
            {"code","scripting.debug-session-already-active"},{"operation",operation},
            {"session",Json::parse(debug_session_state_or_null(debug_session_.get()))}}.dump();
    if(debug_session_==nullptr) debug_session_=std::make_unique<ManagedDebugSession>();
    const auto started=debug_session_->start(adapter,{"--interpreter=vscode"},std::chrono::seconds(5));
    const auto code=started?"ok":debug_session_->last_error().empty()?"scripting.debug-session-start-failed":debug_session_->last_error();
    return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",started},
        {"code",code},{"operation",operation},{"session",Json::parse(debug_session_state_or_null(debug_session_.get()))}}.dump();
}

std::string ManagedScriptRuntime::debug_session_status_json() const {
    if(debug_session_==nullptr)
        return Json{{"schemaVersion","noemancer.managed-debug-session/0.1"},{"success",false},
            {"code","scripting.debug-session-not-started"},{"operation","debug.session.status"},{"session",nullptr}}.dump();
    return Json{{"schemaVersion","noemancer.managed-debug-session/0.1"},{"success",true},{"code","ok"},
        {"operation","debug.session.status"},{"session",Json::parse(debug_session_state_or_null(debug_session_.get()))}}.dump();
}

std::string ManagedScriptRuntime::debug_session_request_json(const std::string_view command,
                                                              const std::string_view arguments_json,
                                                              const std::uint32_t timeout_ms) {
    static constexpr std::array<std::string_view,14> allowed{
        "initialize","launch","attach","configurationDone","setBreakpoints","continue","pause","next","stepIn","stepOut",
        "threads","stackTrace","disconnect","terminate"};
    if(std::ranges::find(allowed,command)==allowed.end())
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",false},
            {"code","scripting.debug-command-not-allowed"},{"operation","debug.session.request"},
            {"command",command},{"body",Json::object()}}.dump();
    if(timeout_ms==0U||timeout_ms>30000U)
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",false},
            {"code","scripting.invalid-debug-timeout"},{"operation","debug.session.request"},
            {"command",command},{"body",Json::object()}}.dump();
    if(arguments_json.size()>256U*1024U)
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",false},
            {"code","scripting.debug-arguments-too-large"},{"operation","debug.session.request"},
            {"command",command},{"body",Json::object()}}.dump();
    const auto arguments=Json::parse(arguments_json.empty()?"{}":std::string(arguments_json),nullptr,false);
    if(arguments.is_discarded()||!arguments.is_object())
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",false},
            {"code","scripting.invalid-debug-arguments"},{"operation","debug.session.request"},
            {"command",command},{"body",Json::object()}}.dump();
    if(debug_session_==nullptr||!debug_session_->active())
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",false},
            {"code","scripting.debug-session-not-active"},{"operation","debug.session.request"},
            {"command",command},{"body",Json::object()}}.dump();
    const auto timeout=std::chrono::milliseconds(timeout_ms);
    const auto reply=command=="terminate"?debug_session_->terminate(timeout):debug_session_->request(command,arguments.dump(),timeout);
    const auto success=reply.received&&reply.success;
    const auto code=reply.error.empty()?(success?"ok":"scripting.debug-request-failed"):reply.error;
    const auto body=Json::parse(reply.body_json,nullptr,false);
    return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",success},
        {"code",code},{"operation","debug.session.request"},{"command",command},
        {"requestSequence",reply.sequence},{"sent",reply.sent},{"received",reply.received},
        {"timedOut",reply.timed_out},{"processExited",reply.process_exited},
        {"message",reply.message},
        {"body",body.is_discarded()?Json::object():sanitize_debug_payload(body)}}.dump();
}

std::string ManagedScriptRuntime::debug_session_events_json() {
    if(debug_session_==nullptr)
        return Json{{"schemaVersion","noemancer.managed-debug-session-events/0.1"},{"success",false},
            {"code","scripting.debug-session-not-started"},{"eventCount",0},{"events",Json::array()}}.dump();
    Json events=Json::array();
    for(const auto& event:debug_session_->drain_events()) {
        const auto body=Json::parse(event.body_json,nullptr,false);
        const auto kind=event.kind==DapSessionMessageKind::event?"event":event.kind==DapSessionMessageKind::request?"request":"response";
        events.push_back({{"kind",kind},{"sequence",event.sequence},{"requestSequence",event.request_sequence},
            {"command",event.command},{"event",event.event},
            {"body",body.is_discarded()?Json::object():sanitize_debug_payload(body)}});
    }
    return Json{{"schemaVersion","noemancer.managed-debug-session-events/0.1"},{"success",true},{"code","ok"},
        {"eventCount",events.size()},{"events",std::move(events)}}.dump();
}

std::string ManagedScriptRuntime::debug_session_stop_json(const std::uint32_t timeout_ms) {
    if(timeout_ms==0U||timeout_ms>30000U)
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",false},
            {"code","scripting.invalid-debug-timeout"},{"operation","debug.session.stop"},{"alreadyStopped",false}}.dump();
    if(debug_session_==nullptr||!debug_session_->active())
        return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",true},
            {"code","ok"},{"operation","debug.session.stop"},{"alreadyStopped",true}}.dump();
    const auto reply=debug_session_->request("disconnect",R"({"restart":false,"terminateDebuggee":false})",
        std::chrono::milliseconds(timeout_ms));
    debug_session_->shutdown(std::chrono::milliseconds(timeout_ms));
    const auto stopped=!debug_session_->active();
    const auto success=stopped&&(reply.received?reply.success:reply.error.empty());
    const auto code=reply.error.empty()?(success?"ok":"scripting.debug-session-stop-failed"):reply.error;
    return Json{{"schemaVersion","noemancer.managed-debug-session-action/0.1"},{"success",success},
        {"code",code},{"operation","debug.session.stop"},{"alreadyStopped",false},
        {"requestSequence",reply.sequence},{"received",reply.received},{"timedOut",reply.timed_out}}.dump();
}

std::optional<std::string> ManagedScriptRuntime::instance_entity_id(const std::string_view instance_id) const {
    const auto instance=std::ranges::find(instances_,instance_id,&ManagedScriptInstance::id);
    return instance==instances_.end()?std::nullopt:std::optional<std::string>{instance->entity_id};
}

bool ManagedScriptRuntime::type_implements_callback(const std::string_view type_name,
                                                     const std::string_view callback) const {
    const auto catalog=Json::parse(discover_project_types_json(),nullptr,false);
    if(!catalog.is_object()||!catalog.value("success",false))return true;
    const auto& types=catalog.value("types",Json::array());
    const auto type=std::ranges::find_if(types,[&](const auto& value) {
        return value.is_object()&&value.value("fullName",std::string{})==type_name;
    });
    if(type==types.end()||!type->contains("callbacks")||!type->at("callbacks").is_array())return true;
    return std::ranges::any_of(type->at("callbacks"),[&](const auto& value) {
        return value.is_string()&&value.template get<std::string>()==callback;
    });
}

void ManagedScriptRuntime::synchronize_instances(std::vector<ManagedScriptInstance> desired) {
    std::ranges::sort(desired,{},&ManagedScriptInstance::id);
    for(const auto& instance:instances_) {
        if(!instance.scene_owned)continue;
        const auto replacement=std::ranges::find(desired,instance.id,&ManagedScriptInstance::id);
        const auto preserved=replacement!=desired.end()&&replacement->entity_id==instance.entity_id&&
            replacement->assembly_asset==instance.assembly_asset&&replacement->type_name==instance.type_name;
        if(!preserved)release_host_instance(instance.id);
    }
    std::vector<ManagedScriptInstance> synchronized;
    synchronized.reserve(desired.size()+instances_.size());
    for(const auto& instance:instances_)if(!instance.scene_owned)synchronized.push_back(instance);
    bool changed=desired.size()!=static_cast<std::size_t>(std::ranges::count_if(instances_,[](const auto& value){return value.scene_owned;}));
    for(auto& value:desired) {
        const auto existing=std::ranges::find(instances_,value.id,&ManagedScriptInstance::id);
        if(existing!=instances_.end()&&existing->entity_id==value.entity_id&&existing->assembly_asset==value.assembly_asset&&
           existing->type_name==value.type_name&&existing->scene_owned) {
            auto preserved=*existing;
            if(preserved.properties_json!=value.properties_json){preserved.properties_json=value.properties_json;changed=true;}
            synchronized.push_back(std::move(preserved));
        }
        else {synchronized.push_back(std::move(value));changed=true;}
    }
    std::ranges::sort(synchronized,{},&ManagedScriptInstance::id);
    if(!changed) for(std::size_t index=0;index<synchronized.size();++index)
        if(synchronized[index].id!=instances_[index].id){changed=true;break;}
    instances_=std::move(synchronized);
    if(changed)++revision_;
}

void ManagedScriptRuntime::release_host_instance(const std::string_view instance_id) {
#ifdef _WIN32
    if(managed_entry_==nullptr)return;
    const auto request=Json{{"operation","instance.release"},{"sessionId",session_id_},{"instanceId",instance_id}}.dump();
    std::array<std::uint8_t,1024> response{};
    static_cast<void>(reinterpret_cast<managed_entry_fn>(managed_entry_)(
        reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<int>(request.size()),response.data(),static_cast<int>(response.size())));
#else
    static_cast<void>(instance_id);
#endif
}

void ManagedScriptRuntime::release_entity_instances(const std::string_view entity_id) {
    for(const auto& instance:instances_)if(instance.entity_id==entity_id)release_host_instance(instance.id);
    const auto previous=instances_.size();
    std::erase_if(instances_,[&](const auto& instance){return instance.entity_id==entity_id;});
    if(instances_.size()!=previous)++revision_;
}

std::vector<ManagedScriptInstance> ManagedScriptRuntime::automatic_instances() const {
    std::vector<ManagedScriptInstance> result;
    for(const auto& instance:instances_)
        if(instance.state!="destroyed"&&instance.state!="faulted") result.push_back(instance);
    return result;
}

} // namespace noemancer
