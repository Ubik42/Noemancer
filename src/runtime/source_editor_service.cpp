#include "runtime/source_editor_service.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace noemancer {
namespace {

using Json=nlohmann::json;

Json failure(const std::string_view code,const std::string_view detail,const bool dry_run) {
    return {{"schemaVersion","noemancer.source-editor-action/0.1"},{"success",false},{"code",code},
        {"operation","source.open-external"},{"detail",detail},{"dryRun",dry_run}};
}

bool inside(const std::filesystem::path& root,const std::filesystem::path& source) {
    const auto relative=source.lexically_relative(root);
    return !relative.empty()&&relative.begin()->string()!="..";
}

#ifdef _WIN32
std::filesystem::path find_vscode() {
    std::array<wchar_t,32768> buffer{};
    const auto length=SearchPathW(nullptr,L"code.exe",nullptr,static_cast<DWORD>(buffer.size()),buffer.data(),nullptr);
    if(length==0||length>=buffer.size())return {};
    return std::filesystem::path(std::wstring_view(buffer.data(),length));
}
#endif

} // namespace

std::string launch_source_editor_json(const std::filesystem::path& project_root,
                                      const std::filesystem::path& source_path,
                                      const std::uint32_t line,const std::uint32_t column,
                                      const bool dry_run) {
    std::error_code error;const auto root=std::filesystem::weakly_canonical(project_root,error);
    if(error||root.empty()||!std::filesystem::is_directory(root,error))
        return failure("source-editor.project-root-invalid","Project root is unavailable.",dry_run).dump();
    auto source=source_path.is_absolute()?source_path:root/source_path;
    source=std::filesystem::weakly_canonical(source,error);
    if(error||!inside(root,source)||!std::filesystem::is_regular_file(source,error))
        return failure("source-editor.path-invalid","Source must be a regular file inside the active project.",dry_run).dump();
    static constexpr std::array<std::string_view,5> allowed{".cs",".csproj",".sln",".props",".targets"};
    auto extension=source.extension().string();
    std::ranges::transform(extension,extension.begin(),[](const unsigned char value){return static_cast<char>(std::tolower(value));});
    if(std::ranges::find(allowed,extension)==allowed.end())
        return failure("source-editor.type-not-supported","Only project C# source and build documents can be opened.",dry_run).dump();
    const auto safe_line=std::clamp(line,1U,10'000'000U);const auto safe_column=std::clamp(column,1U,1'000'000U);
    std::string adapter{"default-association"};bool precise{};
#ifdef _WIN32
    const auto vscode=extension==".cs"?find_vscode():std::filesystem::path{};
    if(!vscode.empty()){adapter="vscode";precise=true;}
    Json receipt{{"schemaVersion","noemancer.source-editor-action/0.1"},{"success",true},
        {"code",dry_run?"source-editor.plan-ready":"ok"},{"operation","source.open-external"},
        {"detail",dry_run?"External source editor launch is valid.":"Source was handed to the external editor."},
        {"dryRun",dry_run},{"source",source.generic_string()},{"projectRelativePath",source.lexically_relative(root).generic_string()},
        {"line",safe_line},{"column",safe_column},{"adapter",adapter},{"preciseLocation",precise}};
    if(dry_run)return receipt.dump();
    SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.fMask=SEE_MASK_FLAG_NO_UI;
    launch.nShow=SW_SHOWNORMAL;const auto working=root.wstring();launch.lpDirectory=working.c_str();
    std::wstring parameters;const auto source_wide=source.wstring();const auto vscode_wide=vscode.wstring();
    if(!vscode.empty()) {
        parameters=L"--goto \""+source_wide+L":"+std::to_wstring(safe_line)+L":"+std::to_wstring(safe_column)+L"\"";
        launch.lpFile=vscode_wide.c_str();launch.lpParameters=parameters.c_str();
    } else launch.lpFile=source_wide.c_str();
    if(!ShellExecuteExW(&launch)) {
        receipt["success"]=false;receipt["code"]="source-editor.launch-failed";
        receipt["detail"]="Windows could not launch an editor for this source.";receipt["win32Error"]=GetLastError();
    }
    return receipt.dump();
#else
    static_cast<void>(dry_run);static_cast<void>(safe_line);static_cast<void>(safe_column);
    return failure("source-editor.platform-pending","External source editor launch is currently implemented on Windows.",dry_run).dump();
#endif
}

} // namespace noemancer
