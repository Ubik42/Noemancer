#include "editor/recent_workspace_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;
constexpr std::string_view schema_version="noemancer.recent-workspaces/0.1";
constexpr std::size_t maximum_entries=128U;
constexpr std::size_t maximum_file_bytes=1024U*1024U;
constexpr std::size_t maximum_path_bytes=4096U;
constexpr std::size_t maximum_name_bytes=256U;

enum class ReadKind : std::uint8_t { valid, missing, invalid, io_failure };

struct ReadResult final {
    ReadKind kind{ReadKind::invalid};
    std::string code;
    std::string detail;
    std::uint64_t revision{};
    std::vector<StartupHubRecentProject> projects;
};

bool valid_utf8(const std::string_view text) {
    for(std::size_t index=0;index<text.size();) {
        const auto first=static_cast<unsigned char>(text[index]);
        if(first==0U)return false;
        std::size_t continuation{};std::uint32_t value{};
        if(first<0x80U){++index;continue;}
        if(first>=0xC2U&&first<=0xDFU){continuation=1U;value=first&0x1FU;}
        else if(first>=0xE0U&&first<=0xEFU){continuation=2U;value=first&0x0FU;}
        else if(first>=0xF0U&&first<=0xF4U){continuation=3U;value=first&0x07U;}
        else return false;
        if(index+continuation>=text.size())return false;
        for(std::size_t offset=1;offset<=continuation;++offset) {
            const auto byte=static_cast<unsigned char>(text[index+offset]);
            if((byte&0xC0U)!=0x80U)return false;
            value=(value<<6U)|(byte&0x3FU);
        }
        if((continuation==2U&&value<0x800U)||(continuation==3U&&value<0x10000U)||
           (value>=0xD800U&&value<=0xDFFFU)||value>0x10FFFFU)return false;
        index+=continuation+1U;
    }
    return true;
}

std::string utf8_path_string(const std::filesystem::path& path) {
    const auto value=path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()),value.size()};
}

std::filesystem::path path_from_utf8(const std::string_view source) {
    std::u8string value;value.reserve(source.size());
    for(const auto byte:source)value.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    return std::filesystem::path(value);
}

std::string normalize_workspace_path(const std::string_view source) {
    if(source.empty()||source.size()>maximum_path_bytes||!valid_utf8(source))return {};
    try {
        auto candidate=path_from_utf8(source);
        std::error_code error;
        candidate=std::filesystem::absolute(candidate,error);
        if(error)return {};
        const auto result=utf8_path_string(candidate.lexically_normal());
        return result.empty()||result.size()>maximum_path_bytes?std::string{}:result;
    } catch(...) {return {};}
}

std::string default_display_name(const std::string_view path) {
    try {
        auto result=utf8_path_string(path_from_utf8(path).filename());
        if(result.empty())result="Project";
        if(result.size()>maximum_name_bytes)result.resize(maximum_name_bytes);
        while(!valid_utf8(result)&&!result.empty())result.pop_back();
        return result.empty()?std::string("Project"):result;
    } catch(...) {return "Project";}
}

std::string lowercase_ascii(std::string value) {
    for(auto& character:value)character=static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::string path_key(const std::string& path) {
#ifdef _WIN32
    try {
        auto value=path_from_utf8(path).native();
        if(!value.empty())CharLowerBuffW(value.data(),static_cast<DWORD>(value.size()));
        return {reinterpret_cast<const char*>(value.data()),value.size()*sizeof(wchar_t)};
    } catch(...) {return lowercase_ascii(path);}
#else
    return path;
#endif
}

void sort_and_limit(std::vector<StartupHubRecentProject>& projects,const std::size_t limit) {
    std::unordered_map<std::string,StartupHubRecentProject> unique;
    unique.reserve(projects.size());
    for(auto& project:projects) {
        const auto key=path_key(project.path);
        const auto found=unique.find(key);
        if(found==unique.end()||project.last_opened_unix_seconds>found->second.last_opened_unix_seconds||
           (project.last_opened_unix_seconds==found->second.last_opened_unix_seconds&&
            lowercase_ascii(project.display_name)<lowercase_ascii(found->second.display_name)))
            unique[key]=std::move(project);
    }
    projects.clear();projects.reserve(unique.size());
    for(auto& [unused,project]:unique){static_cast<void>(unused);projects.push_back(std::move(project));}
    std::ranges::sort(projects,[](const auto& left,const auto& right) {
        if(left.last_opened_unix_seconds!=right.last_opened_unix_seconds)
            return left.last_opened_unix_seconds>right.last_opened_unix_seconds;
        const auto left_name=lowercase_ascii(left.display_name),right_name=lowercase_ascii(right.display_name);
        if(left_name!=right_name)return left_name<right_name;
        return path_key(left.path)<path_key(right.path);
    });
    if(projects.size()>limit)projects.resize(limit);
}

ReadResult read_store(const RecentWorkspaceStoreOptions& options) {
    if(options.storage_path.empty())return {ReadKind::io_failure,"recent-workspaces.storage-path-empty",
        "A storage path is required."};
    std::error_code error;
    const auto size=std::filesystem::file_size(options.storage_path,error);
    if(error) {
        if(error==std::errc::no_such_file_or_directory)
            return {ReadKind::missing,"recent-workspaces.missing","No recent workspace file exists yet."};
        return {ReadKind::io_failure,"recent-workspaces.read-failed","The recent workspace file could not be inspected."};
    }
    if(size>options.max_file_bytes)return {ReadKind::invalid,"recent-workspaces.file-too-large",
        "The recent workspace file exceeds its byte budget."};
    std::ifstream input(options.storage_path,std::ios::binary);
    if(!input)return {ReadKind::io_failure,"recent-workspaces.read-failed",
        "The recent workspace file could not be opened."};
    std::string text(static_cast<std::size_t>(size),'\0');
    if(!text.empty())input.read(text.data(),static_cast<std::streamsize>(text.size()));
    if(input.gcount()!=static_cast<std::streamsize>(text.size())||input.peek()!=std::char_traits<char>::eof())
        return {ReadKind::invalid,"recent-workspaces.truncated-or-raced",
            "The recent workspace file changed or was truncated while being read."};
    if(!valid_utf8(text))return {ReadKind::invalid,"recent-workspaces.invalid-utf8",
        "The recent workspace file is not strict UTF-8."};
    const auto document=Json::parse(text,nullptr,false);
    if(document.is_discarded())return {ReadKind::invalid,"recent-workspaces.invalid-json",
        "The recent workspace file is not valid JSON."};
    if(!document.is_object()||!document.contains("schemaVersion")||!document.at("schemaVersion").is_string()||
       document.at("schemaVersion").get<std::string>()!=schema_version)
        return {ReadKind::invalid,"recent-workspaces.schema-invalid","The recent workspace schema is unsupported."};
    if(!document.contains("revision")||!document.at("revision").is_number_unsigned()||
       !document.contains("projects")||!document.at("projects").is_array()||
       document.at("projects").size()>maximum_entries)
        return {ReadKind::invalid,"recent-workspaces.document-invalid","The recent workspace document has invalid fields."};
    ReadResult result{ReadKind::valid,"recent-workspaces.loaded","Recent workspaces loaded."};
    result.revision=document.at("revision").get<std::uint64_t>();
    for(const auto& item:document.at("projects")) {
        if(!item.is_object()||!item.contains("path")||!item.at("path").is_string()||
           !item.contains("displayName")||!item.at("displayName").is_string()||
           !item.contains("lastOpenedUnixSeconds")||!item.at("lastOpenedUnixSeconds").is_number_unsigned())
            return {ReadKind::invalid,"recent-workspaces.document-invalid","A recent workspace entry has invalid fields."};
        const auto raw_path=item.at("path").get<std::string>();
        auto name=item.at("displayName").get<std::string>();
        const auto path=normalize_workspace_path(raw_path);
        if(path.empty()||name.size()>maximum_name_bytes||!valid_utf8(name))
            return {ReadKind::invalid,"recent-workspaces.entry-invalid","A recent workspace entry violates its bounds."};
        if(name.empty())name=default_display_name(path);
        result.projects.push_back({path,std::move(name),item.at("lastOpenedUnixSeconds").get<std::uint64_t>()});
    }
    sort_and_limit(result.projects,options.max_entries);
    return result;
}

Json store_document(const std::uint64_t revision,const std::vector<StartupHubRecentProject>& projects) {
    Json entries=Json::array();
    for(const auto& project:projects)entries.push_back({{"path",project.path},{"displayName",project.display_name},
        {"lastOpenedUnixSeconds",project.last_opened_unix_seconds}});
    return {{"schemaVersion",schema_version},{"revision",revision},{"projects",std::move(entries)}};
}

bool atomic_write(const RecentWorkspaceStoreOptions& options,const std::string_view contents,std::string& code,std::string& detail) {
    if(contents.size()>options.max_file_bytes) {
        code="recent-workspaces.write-budget-exceeded";detail="The recent workspace document exceeds its byte budget.";return false;
    }
    std::error_code error;
    const auto parent=options.storage_path.parent_path();
    if(!parent.empty())std::filesystem::create_directories(parent,error);
    if(error){code="recent-workspaces.directory-create-failed";detail="The recent workspace directory could not be created.";return false;}
    static std::atomic<std::uint64_t> sequence{};
#ifdef _WIN32
    const auto process_id=static_cast<unsigned long>(_getpid());
#else
    const auto process_id=static_cast<unsigned long>(getpid());
#endif
    auto temporary=options.storage_path;
#ifdef _WIN32
    temporary+=std::wstring(L".tmp-")+std::to_wstring(process_id)+L"-"+std::to_wstring(++sequence);
#else
    temporary+=std::string(".tmp-")+std::to_string(process_id)+"-"+std::to_string(++sequence);
#endif
    {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output){code="recent-workspaces.write-failed";detail="The sibling temporary file could not be opened.";return false;}
        output.write(contents.data(),static_cast<std::streamsize>(contents.size()));output.flush();
        if(!output){output.close();std::filesystem::remove(temporary,error);code="recent-workspaces.write-failed";
            detail="The sibling temporary file could not be flushed.";return false;}
        output.close();
        if(!output){std::filesystem::remove(temporary,error);code="recent-workspaces.write-failed";
            detail="The sibling temporary file could not be closed.";return false;}
    }
#ifdef _WIN32
    if(MoveFileExW(temporary.c_str(),options.storage_path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0)return true;
    error=std::error_code(static_cast<int>(GetLastError()),std::system_category());
#else
    std::filesystem::rename(temporary,options.storage_path,error);if(!error)return true;
#endif
    std::error_code cleanup;std::filesystem::remove(temporary,cleanup);
    code="recent-workspaces.commit-failed";detail="The recent workspace file could not be atomically replaced.";return false;
}

RecentWorkspaceStoreReceipt receipt(const bool success,std::string code,std::string detail,
                                    const RecentWorkspaceStoreSnapshot& snapshot,const bool recovered=false) {
    return {success,std::move(code),std::move(detail),snapshot.revision,snapshot.projects,recovered};
}

} // namespace

RecentWorkspaceStore::RecentWorkspaceStore(RecentWorkspaceStoreOptions options):options_(std::move(options)) {
    options_.max_entries=std::clamp<std::size_t>(options_.max_entries,1U,maximum_entries);
    options_.max_file_bytes=std::clamp<std::size_t>(options_.max_file_bytes,1U,maximum_file_bytes);
}

const RecentWorkspaceStoreOptions& RecentWorkspaceStore::options() const noexcept{return options_;}

RecentWorkspaceStoreReceipt RecentWorkspaceStore::load() {
    const auto read=read_store(options_);
    snapshot_.revision=read.kind==ReadKind::valid?read.revision:0U;
    snapshot_.projects=read.kind==ReadKind::valid?read.projects:std::vector<StartupHubRecentProject>{};
    snapshot_.loaded=read.kind==ReadKind::valid||read.kind==ReadKind::missing;
    snapshot_.healthy=read.kind==ReadKind::valid||read.kind==ReadKind::missing;
    snapshot_.code=read.code;snapshot_.detail=read.detail;
    return receipt(snapshot_.healthy,read.code,read.detail,snapshot_);
}

RecentWorkspaceStoreReceipt RecentWorkspaceStore::record_opened(
    const std::string_view path,const std::string_view display_name,const std::uint64_t unix_seconds) {
    const auto normalized=normalize_workspace_path(path);
    if(normalized.empty())return receipt(false,"recent-workspaces.path-invalid",
        "The workspace path must be non-empty, strict UTF-8, and at most 4096 bytes.",snapshot_);
    if(display_name.size()>maximum_name_bytes||(!display_name.empty()&&!valid_utf8(display_name)))
        return receipt(false,"recent-workspaces.display-name-invalid",
            "The display name must be strict UTF-8 and at most 256 bytes.",snapshot_);
    const auto live=read_store(options_);
    if(live.kind==ReadKind::io_failure)return receipt(false,live.code,live.detail,snapshot_);
    const auto recovered=live.kind==ReadKind::invalid;
    if(live.kind==ReadKind::valid&&live.revision==std::numeric_limits<std::uint64_t>::max())
        return receipt(false,"recent-workspaces.revision-overflow","The recent workspace revision cannot advance.",snapshot_);
    auto projects=live.kind==ReadKind::valid?live.projects:std::vector<StartupHubRecentProject>{};
    projects.push_back({normalized,display_name.empty()?default_display_name(normalized):std::string(display_name),unix_seconds});
    sort_and_limit(projects,options_.max_entries);
    const auto revision=(live.kind==ReadKind::valid?live.revision:0U)+1U;
    const auto contents=store_document(revision,projects).dump(2)+"\n";
    std::string code,detail;
    if(!atomic_write(options_,contents,code,detail))return receipt(false,std::move(code),std::move(detail),snapshot_);
    snapshot_={revision,std::move(projects),true,true,"recent-workspaces.recorded",
        recovered?"The invalid recent workspace file was replaced with a valid document.":"The workspace was recorded."};
    return receipt(true,snapshot_.code,snapshot_.detail,snapshot_,recovered);
}

const RecentWorkspaceStoreSnapshot& RecentWorkspaceStore::snapshot() const noexcept{return snapshot_;}

std::string RecentWorkspaceStore::observation_json() const {
    auto detail=snapshot_.detail;if(detail.size()>512U)detail.resize(512U);
    return Json{{"schemaVersion",schema_version},{"revision",snapshot_.revision},
        {"entryCount",snapshot_.projects.size()},{"healthy",snapshot_.healthy},{"code",snapshot_.code},
        {"detail",std::move(detail)},{"loaded",snapshot_.loaded}}.dump();
}

} // namespace noemancer
