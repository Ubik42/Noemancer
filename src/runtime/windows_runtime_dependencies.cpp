#include "runtime/windows_runtime_dependencies.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace noemancer {

std::string native_runtime_dependencies_json() {
    using Json=nlohmann::json;
    Json modules=Json::array();std::size_t resolved{},app_local{};
#ifdef _WIN32
    std::array<wchar_t,32768> executable_buffer{};
    const auto executable_length=GetModuleFileNameW(nullptr,executable_buffer.data(),static_cast<DWORD>(executable_buffer.size()));
    const auto executable=executable_length>0&&executable_length<executable_buffer.size()?
        std::filesystem::path(executable_buffer.data()).lexically_normal():std::filesystem::path{};
    const auto application_directory=executable.parent_path();
    constexpr std::array<std::wstring_view,5> required_modules={
        L"MSVCP140.dll",L"MSVCP140_ATOMIC_WAIT.dll",L"MSVCP140_1.dll",L"VCRUNTIME140.dll",L"VCRUNTIME140_1.dll"};
    for(const auto name:required_modules) {
        const auto handle=GetModuleHandleW(std::wstring(name).c_str());std::filesystem::path path;
        if(handle!=nullptr) {
            std::array<wchar_t,32768> path_buffer{};const auto length=GetModuleFileNameW(handle,path_buffer.data(),static_cast<DWORD>(path_buffer.size()));
            if(length>0&&length<path_buffer.size())path=std::filesystem::path(path_buffer.data()).lexically_normal();
        }
        const bool available=!path.empty();const bool local=available&&path.parent_path()==application_directory;
        resolved+=available?1U:0U;app_local+=local?1U:0U;
        modules.push_back({{"name",std::filesystem::path(std::wstring(name)).string()},{"resolved",available},
            {"path",path.generic_string()},{"appLocal",local}});
    }
    return Json{{"schema","noemancer.native-runtime-dependencies/0.1"},{"platform","windows-x64"},
        {"executable",executable.generic_string()},{"applicationDirectory",application_directory.generic_string()},
        {"requiredCount",required_modules.size()},{"resolvedCount",resolved},{"appLocalCount",app_local},
        {"complete",resolved==required_modules.size()&&app_local==required_modules.size()},{"modules",std::move(modules)}}.dump();
#else
    return Json{{"schema","noemancer.native-runtime-dependencies/0.1"},{"platform","unsupported"},
        {"requiredCount",0},{"resolvedCount",0},{"appLocalCount",0},{"complete",false},{"modules",std::move(modules)}}.dump();
#endif
}

} // namespace noemancer
