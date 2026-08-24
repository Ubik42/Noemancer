#include "runtime/game_persistence_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace noemancer {
namespace {

using Json=nlohmann::json;

bool valid_id(const std::string_view value) {
    if(value.empty()||value.size()>64U)return false;
    return std::ranges::all_of(value,[](const unsigned char character){
        return (character>='a'&&character<='z')||(character>='A'&&character<='Z')||
            (character>='0'&&character<='9')||character=='.'||character=='_'||character=='-';
    });
}

std::string kind_name(const PersistedDocumentKind kind){return kind==PersistedDocumentKind::save?"save":"replay";}
std::string expected_schema(const PersistedDocumentKind kind){return kind==PersistedDocumentKind::save?
    "noemancer.save-game/0.2":"noemancer.input-replay/0.2";}
std::size_t maximum_bytes(const PersistedDocumentKind kind){return kind==PersistedDocumentKind::save?
    16U*1024U*1024U:64U*1024U*1024U;}

PersistenceStoreReceipt failure(const std::string& project_id,const std::string_view slot_id,
                                std::string code,std::string detail,std::filesystem::path path={}) {
    return {false,std::move(code),std::move(detail),project_id,std::string(slot_id),std::move(path),{}};
}

} // namespace

GamePersistenceStore::GamePersistenceStore(std::filesystem::path user_data_root,std::string project_id)
    : root_(std::filesystem::absolute(std::move(user_data_root)).lexically_normal()),project_id_(std::move(project_id)) {}

std::filesystem::path GamePersistenceStore::slot_path(const PersistedDocumentKind kind,const std::string_view slot_id) const {
    const auto directory=kind==PersistedDocumentKind::save?"saves":"replays";
    const auto extension=kind==PersistedDocumentKind::save?".save.json":".replay.json";
    return (root_/project_id_/directory/(std::string(slot_id)+extension)).lexically_normal();
}

PersistenceStoreReceipt GamePersistenceStore::write(const PersistedDocumentKind kind,const std::string_view slot_id,
                                                      const std::string_view document_json) const {
    if(!valid_id(project_id_)||!valid_id(slot_id))return failure(project_id_,slot_id,"persistence.invalid-id",
        "Project and slot IDs must contain only ASCII letters, digits, dot, underscore or dash.");
    if(document_json.empty()||document_json.size()>maximum_bytes(kind))return failure(project_id_,slot_id,
        "persistence.document-size-invalid","Persistence document exceeds the bounded size contract.");
    const auto document=Json::parse(document_json,nullptr,false);
    if(document.is_discarded()||!document.is_object()||document.value("schemaVersion",std::string{})!=expected_schema(kind))
        return failure(project_id_,slot_id,"persistence.document-schema-invalid","Persistence document schema does not match its slot kind.");
    const auto path=slot_path(kind,slot_id);std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if(error)return failure(project_id_,slot_id,"persistence.directory-create-failed",error.message(),path);
    if(std::filesystem::is_symlink(path.parent_path(),error)||std::filesystem::is_symlink(path,error))
        return failure(project_id_,slot_id,"persistence.symlink-rejected","Persistence slots never traverse symbolic links.",path);
    static std::atomic<std::uint64_t> sequence{};
    const auto temporary=path.parent_path()/(path.filename().string()+".tmp-"+std::to_string(++sequence));
    {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output)return failure(project_id_,slot_id,"persistence.write-failed","Temporary slot file could not be opened.",path);
        output.write(document_json.data(),static_cast<std::streamsize>(document_json.size()));output.flush();
        if(!output){std::filesystem::remove(temporary,error);return failure(project_id_,slot_id,"persistence.write-failed",
            "Temporary slot file could not be fully written.",path);}}
#ifdef _WIN32
    if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)==0) {
        const auto detail="Atomic slot replacement failed with Win32 error "+std::to_string(GetLastError());
        std::filesystem::remove(temporary,error);return failure(project_id_,slot_id,"persistence.atomic-replace-failed",detail,path);}
#else
    std::filesystem::rename(temporary,path,error);
    if(error){std::filesystem::remove(temporary,error);return failure(project_id_,slot_id,"persistence.atomic-replace-failed",error.message(),path);}
#endif
    return {true,"ok","Persistence slot committed atomically.",project_id_,std::string(slot_id),path,{}};
}

PersistenceStoreReceipt GamePersistenceStore::read(const PersistedDocumentKind kind,const std::string_view slot_id) const {
    if(!valid_id(project_id_)||!valid_id(slot_id))return failure(project_id_,slot_id,"persistence.invalid-id",
        "Project and slot IDs must contain only ASCII letters, digits, dot, underscore or dash.");
    const auto path=slot_path(kind,slot_id);std::error_code error;
    if(std::filesystem::is_symlink(path.parent_path(),error)||std::filesystem::is_symlink(path,error))
        return failure(project_id_,slot_id,"persistence.symlink-rejected","Persistence slots never traverse symbolic links.",path);
    if(!std::filesystem::is_regular_file(path,error))return failure(project_id_,slot_id,"persistence.slot-not-found",
        "The requested persistence slot does not exist.",path);
    const auto bytes=std::filesystem::file_size(path,error);
    if(error||bytes>maximum_bytes(kind))return failure(project_id_,slot_id,"persistence.document-size-invalid",
        "Persistence slot exceeds the bounded size contract.",path);
    std::ifstream input(path,std::ios::binary);if(!input)return failure(project_id_,slot_id,"persistence.read-failed",
        "Persistence slot could not be opened.",path);
    std::string document{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    const auto parsed=Json::parse(document,nullptr,false);
    if(parsed.is_discarded()||!parsed.is_object()||parsed.value("schemaVersion",std::string{})!=expected_schema(kind))
        return failure(project_id_,slot_id,"persistence.document-schema-invalid","Persistence slot schema is invalid.",path);
    return {true,"ok","Persistence slot loaded.",project_id_,std::string(slot_id),path,std::move(document)};
}

std::string GamePersistenceStore::observe_json() const {
    Json kinds=Json::array();
    for(const auto kind:{PersistedDocumentKind::save,PersistedDocumentKind::replay}) {
        Json slots=Json::array();const auto directory=root_/project_id_/(kind==PersistedDocumentKind::save?"saves":"replays");
        std::error_code error;if(std::filesystem::is_directory(directory,error)&&!std::filesystem::is_symlink(directory,error))
            for(const auto& entry:std::filesystem::directory_iterator(directory,error))if(entry.is_regular_file(error)&&slots.size()<64U)
                slots.push_back({{"file",entry.path().filename().generic_string()},{"bytes",entry.file_size(error)}});
        kinds.push_back({{"kind",kind_name(kind)},{"slots",std::move(slots)}});
    }
    return Json{{"schemaVersion","noemancer.persistence-store/0.1"},{"projectId",project_id_},
        {"root",root_.generic_string()},{"kinds",std::move(kinds)}}.dump();
}

} // namespace noemancer
