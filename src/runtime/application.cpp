#include "runtime/application.hpp"

#include "engine/play_world_apply.hpp"
#include "runtime/audio_output_backend.hpp"
#include "runtime/asset_thumbnail_gpu_cache.hpp"
#include "runtime/game_persistence_store.hpp"
#include "runtime/input_source_adapter.hpp"
#include "runtime/performance_evidence.hpp"

#include "engine/fbx_asset.hpp"
#include "engine/gltf_mesh.hpp"
#include "engine/animation_clip_asset.hpp"
#include "engine/mesh_runtime_artifact.hpp"
#include "engine/render_world.hpp"
#include "engine/render_reference_scene.hpp"
#include "engine/project_document.hpp"
#include "engine/project_hybrid_pixel_authoring.hpp"
#include "engine/project_input_authoring.hpp"
#include "engine/project_workspace.hpp"
#include "engine/retained_ui_runtime.hpp"
#include "engine/semantic_ui.hpp"
#include "runtime/scene_renderer.hpp"
#include "runtime/retained_ui_gpu_adapter.hpp"
#include "runtime/source_editor_service.hpp"
#include "runtime/windows_package_service.hpp"
#include "runtime/windows_runtime_dependencies.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef min
#undef max
#endif

namespace noemancer {

class DebugPlayerProcess final {
public:
    ~DebugPlayerProcess() { terminate(); }
    bool launch(const std::filesystem::path& executable) {
#ifdef _WIN32
        terminate();
        static std::atomic_uint64_t next_id{1U};
        const auto event_base="Local\\Noemancer.Player.Debug."+std::to_string(GetCurrentProcessId())+"."+
            std::to_string(next_id.fetch_add(1U));
        ready_event_name_=event_base+".Ready";release_event_name_=event_base+".Release";
        const auto wide=[](const std::string_view text) {
            if(text.empty())return std::wstring{};
            const auto count=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
            std::wstring result(static_cast<std::size_t>(count),L'\0');
            MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count);return result;
        };
        const auto quote=[](const std::wstring& value) {return L"\""+value+L"\"";};
        const auto ready_event_wide=wide(ready_event_name_);const auto release_event_wide=wide(release_event_name_);
        ready_event_=CreateEventW(nullptr,TRUE,FALSE,ready_event_wide.c_str());
        release_event_=CreateEventW(nullptr,TRUE,FALSE,release_event_wide.c_str());
        if(ready_event_==nullptr||release_event_==nullptr){error_="player.debug-event-create-failed";terminate();return false;}
        auto command_line=quote(executable.wstring())+L" --debug-ready "+quote(ready_event_wide)+L" --debug-wait "+quote(release_event_wide);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        const auto working_directory=executable.parent_path().wstring();
        if(!CreateProcessW(nullptr,command_line.data(),nullptr,nullptr,FALSE,0,nullptr,
            working_directory.empty()?nullptr:working_directory.c_str(),&startup,&process)) {
            error_="player.process-start-failed:"+std::to_string(GetLastError());terminate();return false;
        }
        CloseHandle(process.hThread);process_=process.hProcess;process_id_=process.dwProcessId;
        job_=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(job_==nullptr||!SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||
            !AssignProcessToJobObject(job_,process_)) {
            error_="player.job-ownership-failed:"+std::to_string(GetLastError());terminate();return false;
        }
        error_.clear();return true;
#else
        static_cast<void>(executable);error_="player.process-platform-pending";return false;
#endif
    }
    [[nodiscard]] bool wait_ready(const std::chrono::milliseconds timeout) const noexcept {
#ifdef _WIN32
        return ready_event_!=nullptr&&WaitForSingleObject(ready_event_,static_cast<DWORD>(std::max<std::int64_t>(0,timeout.count())))==WAIT_OBJECT_0;
#else
        static_cast<void>(timeout);return false;
#endif
    }
    void release() noexcept {
#ifdef _WIN32
        if(release_event_!=nullptr)SetEvent(release_event_);
#endif
    }
    void terminate() noexcept {
#ifdef _WIN32
        release();
        if(process_!=nullptr&&WaitForSingleObject(process_,0)==WAIT_TIMEOUT) {
            TerminateProcess(process_,125U);WaitForSingleObject(process_,5000U);
        }
        if(process_!=nullptr)CloseHandle(process_);if(ready_event_!=nullptr)CloseHandle(ready_event_);
        if(release_event_!=nullptr)CloseHandle(release_event_);if(job_!=nullptr)CloseHandle(job_);
        process_=nullptr;ready_event_=nullptr;release_event_=nullptr;job_=nullptr;process_id_=0U;
#endif
    }
    [[nodiscard]] std::uint64_t process_id() const noexcept {return process_id_;}
    [[nodiscard]] const std::string& error() const noexcept {return error_;}
private:
#ifdef _WIN32
    HANDLE process_{};
    HANDLE ready_event_{};
    HANDLE release_event_{};
    HANDLE job_{};
#endif
    std::uint64_t process_id_{};
    std::string ready_event_name_;
    std::string release_event_name_;
    std::string error_;
};

namespace {

bool supported_game_profile_schema(const nlohmann::json& profile) {
    if(!profile.is_object())return false;
    const auto schema=profile.value("schema",std::string{});
    return schema=="noemancer.game-profile/0.1"||schema=="noemancer.game-profile/0.2"||
        schema=="noemancer.game-profile/0.3"||schema=="noemancer.game-profile/0.4";
}

std::optional<HybridPixelProfile> hybrid_pixel_profile_from_game_profile(
    const nlohmann::json& profile, bool& valid) {
    valid = true;
    if (!profile.contains("hybridPixelProfile")) return std::nullopt;
    if (profile.value("schema", std::string{}) != "noemancer.game-profile/0.4" ||
        !profile.at("hybridPixelProfile").is_object()) {
        valid = false;
        return std::nullopt;
    }
    const auto parsed = HybridPixelProfileCodec::parse_json(
        profile.at("hybridPixelProfile").dump());
    if (!parsed) {
        valid = false;
        return std::nullopt;
    }
    return *parsed.document;
}

std::optional<std::vector<InputActionDefinition>> input_actions_from_profile(const nlohmann::json& profile) {
    if(!profile.contains("inputActions"))return default_input_action_definitions();
    const auto& actions=profile["inputActions"];
    if(!actions.is_array()||actions.empty()||actions.size()>64U)return std::nullopt;
    std::vector<InputActionDefinition> result;result.reserve(actions.size());
    for(const auto& action:actions) {
        if(!action.is_object()||!action.contains("id")||!action["id"].is_string()||
            !action.contains("kind")||!action["kind"].is_string()||
            !action.contains("bindings")||!action["bindings"].is_array())return std::nullopt;
        InputActionDefinition definition;definition.id=action["id"].get<std::string>();
        const auto kind=action["kind"].get<std::string>();
        if(kind=="button")definition.kind=InputActionKind::button;
        else if(kind=="axis1d")definition.kind=InputActionKind::axis_1d;
        else return std::nullopt;
        if(action["bindings"].empty()||action["bindings"].size()>16U)return std::nullopt;
        for(const auto& binding:action["bindings"]) {
            if(!binding.is_object()||!binding.contains("source")||!binding["source"].is_string())return std::nullopt;
            if((binding.contains("scale")&&!binding["scale"].is_number())||
                (binding.contains("deadZone")&&!binding["deadZone"].is_number()))return std::nullopt;
            const auto scale=binding.value("scale",1.0F);const auto dead_zone=binding.value("deadZone",0.0F);
            if(!std::isfinite(scale)||scale==0.0F||std::abs(scale)>4.0F||
                !std::isfinite(dead_zone)||dead_zone<0.0F||dead_zone>=1.0F)return std::nullopt;
            definition.bindings.push_back({binding["source"].get<std::string>(),scale,dead_zone});
        }
        result.push_back(std::move(definition));
    }
    InputActionRuntime validator;if(!validator.configure(result))return std::nullopt;
    return result;
}

std::optional<std::string> hud_document_from_profile(const std::filesystem::path& profile_path,
                                                      const nlohmann::json& profile,std::string& error_json) {
    const auto relative=std::filesystem::path(profile.value("hudDocument",std::string{})).lexically_normal();
    if(relative.empty())return std::string{};
    if(relative.is_absolute()||*relative.begin()==std::filesystem::path("..")) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.hud-document-invalid"}}.dump();return std::nullopt;
    }
    const auto package_root=profile_path.parent_path().parent_path();const auto path=(package_root/"content"/relative).lexically_normal();
    std::ifstream stream(path,std::ios::binary);if(!stream) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.hud-document-missing"},{"hudDocument",path.generic_string()}}.dump();return std::nullopt;
    }
    const std::string source{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
    const auto validation=nlohmann::json::parse(semantic_ui_validation_json(source),nullptr,false);
    if(!validation.is_object()||!validation.value("valid",false)) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.hud-document-invalid"},{"hudDocument",path.generic_string()}}.dump();return std::nullopt;
    }
    return source;
}

float normalized_gamepad_axis(const Sint16 value) {
    return value<0?static_cast<float>(value)/32768.0F:static_cast<float>(value)/32767.0F;
}

bool wait_for_debug_release(const std::string_view event_name) {
    if(event_name.empty())return true;
#ifdef _WIN32
    const auto count=MultiByteToWideChar(CP_UTF8,0,event_name.data(),static_cast<int>(event_name.size()),nullptr,0);
    std::wstring wide(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,event_name.data(),static_cast<int>(event_name.size()),wide.data(),count);
    const auto event=OpenEventW(SYNCHRONIZE,FALSE,wide.c_str());if(event==nullptr)return false;
    const auto result=WaitForSingleObject(event,60000U);CloseHandle(event);return result==WAIT_OBJECT_0;
#else
    return false;
#endif
}

bool signal_debug_ready(const std::string_view event_name) {
    if(event_name.empty())return true;
#ifdef _WIN32
    const auto count=MultiByteToWideChar(CP_UTF8,0,event_name.data(),static_cast<int>(event_name.size()),nullptr,0);
    std::wstring wide(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,event_name.data(),static_cast<int>(event_name.size()),wide.data(),count);
    const auto event=OpenEventW(EVENT_MODIFY_STATE,FALSE,wide.c_str());if(event==nullptr)return false;
    const auto result=SetEvent(event)!=FALSE;CloseHandle(event);return result;
#else
    return false;
#endif
}

RetainedUiKey retained_key_from_sdl(const SDL_Keycode key) {
    switch(key) {
    case SDLK_BACKSPACE: return RetainedUiKey::backspace;
    case SDLK_TAB: return RetainedUiKey::tab;
    case SDLK_RETURN: case SDLK_KP_ENTER: return RetainedUiKey::enter;
    case SDLK_ESCAPE: return RetainedUiKey::escape;
    case SDLK_HOME: return RetainedUiKey::home;
    case SDLK_END: return RetainedUiKey::end;
    case SDLK_LEFT: return RetainedUiKey::left;
    case SDLK_RIGHT: return RetainedUiKey::right;
    case SDLK_UP: return RetainedUiKey::up;
    case SDLK_DOWN: return RetainedUiKey::down;
    case SDLK_INSERT: return RetainedUiKey::insert_key;
    case SDLK_DELETE: return RetainedUiKey::delete_key;
    case SDLK_A: return RetainedUiKey::a;
    case SDLK_C: return RetainedUiKey::c;
    case SDLK_V: return RetainedUiKey::v;
    case SDLK_X: return RetainedUiKey::x;
    case SDLK_Y: return RetainedUiKey::y;
    case SDLK_Z: return RetainedUiKey::z;
    default: return RetainedUiKey::unknown;
    }
}

std::uint32_t retained_modifiers_from_sdl(const SDL_Keymod modifiers) {
    std::uint32_t result=retained_ui_modifier_none;
    if((modifiers&SDL_KMOD_CTRL)!=0) result|=retained_ui_modifier_ctrl;
    if((modifiers&SDL_KMOD_SHIFT)!=0) result|=retained_ui_modifier_shift;
    if((modifiers&SDL_KMOD_ALT)!=0) result|=retained_ui_modifier_alt;
    if((modifiers&SDL_KMOD_CAPS)!=0) result|=retained_ui_modifier_caps_lock;
    if((modifiers&SDL_KMOD_NUM)!=0) result|=retained_ui_modifier_num_lock;
    return result;
}

std::optional<std::filesystem::path> resolve_thumbnail_artifact(
    const AssetRegistry& registry,const std::string_view uri) {
    constexpr std::string_view prefix="cache://thumbnails/";
    if(!uri.starts_with(prefix)||registry.asset_roots().empty())return std::nullopt;
    const auto name=uri.substr(prefix.size());
    if(name.empty()||name.find('/')!=std::string_view::npos||name.find('\\')!=std::string_view::npos||
       name.find("..")!=std::string_view::npos)return std::nullopt;
    const auto root=(registry.asset_roots().front().parent_path()/"generated"/"thumbnail-cache").lexically_normal();
    const auto path=(root/std::string(name)).lexically_normal();const auto relative=path.lexically_relative(root);
    if(relative.empty()||*relative.begin()==std::filesystem::path(".."))return std::nullopt;
    return path;
}

std::optional<SceneDocument> load_player_scene(const std::filesystem::path& profile_path,
                                               std::string& display_name,std::string& error_json) {
    std::ifstream profile_stream(profile_path,std::ios::binary);
    if(!profile_stream) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.profile-missing"},{"profile",profile_path.generic_string()}}.dump();return std::nullopt;
    }
    const std::string profile_text{std::istreambuf_iterator<char>(profile_stream),std::istreambuf_iterator<char>()};
    const auto profile=nlohmann::json::parse(profile_text,nullptr,false);
    if(!supported_game_profile_schema(profile)) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.profile-invalid"},{"profile",profile_path.generic_string()}}.dump();return std::nullopt;
    }
    const auto relative=std::filesystem::path(profile.value("startupScene",std::string{})).lexically_normal();
    if(relative.empty()||relative.is_absolute()||*relative.begin()==std::filesystem::path("..")) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.startup-scene-invalid"}}.dump();return std::nullopt;
    }
    const auto package_root=profile_path.parent_path().parent_path();
    const auto scene_path=(package_root/"content"/relative).lexically_normal();
    std::ifstream scene_stream(scene_path,std::ios::binary);
    if(!scene_stream) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.startup-scene-missing"},{"scene",scene_path.generic_string()}}.dump();return std::nullopt;
    }
    const std::string scene_text{std::istreambuf_iterator<char>(scene_stream),std::istreambuf_iterator<char>()};
    auto parsed=SceneDocumentCodec::parse_json(scene_text,scene_path.generic_string());
    if(!parsed) {
        error_json=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.startup-scene-invalid"},{"scene",scene_path.generic_string()}}.dump();return std::nullopt;
    }
    display_name=profile.value("displayName",std::string{"Noemancer Player"});
    return std::move(*parsed.document);
}

nlohmann::json load_player_profile_document(const std::filesystem::path& profile_path) {
    std::ifstream stream(profile_path,std::ios::binary);if(!stream)return nlohmann::json();
    const std::string text{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
    return nlohmann::json::parse(text,nullptr,false);
}

std::filesystem::path default_user_data_root() {
#ifdef _WIN32
    const auto required=GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);
    if(required>1U) {
        std::wstring local_app_data(required,L'\0');
        const auto written=GetEnvironmentVariableW(L"LOCALAPPDATA",local_app_data.data(),required);
        if(written>0U&&written<required) {
            local_app_data.resize(written);
            return std::filesystem::path(local_app_data)/"Noemancer"/"projects";
        }
    }
#else
    if(const auto* xdg_data_home=std::getenv("XDG_DATA_HOME");xdg_data_home!=nullptr&&*xdg_data_home!='\0')
        return std::filesystem::path(xdg_data_home)/"noemancer"/"projects";
    if(const auto* home=std::getenv("HOME");home!=nullptr&&*home!='\0')
        return std::filesystem::path(home)/".local"/"share"/"noemancer"/"projects";
#endif
    return std::filesystem::temp_directory_path()/"noemancer-user-data"/"projects";
}

} // namespace

Application::Application(RunOptions options)
    : options_(options), logger_(options.log_format), editor_ui_(world_, asset_registry_) {
    engine_host_.register_default_modules();
    if(options_.player_mode) {
        const auto scene=load_player_scene(options_.player_profile_path,options_.player_display_name,startup_error_json_);
        if(scene) {
            const auto profile=load_player_profile_document(options_.player_profile_path);
            if(profile.is_object())configure_persistence_store(profile.value("projectId",std::string{}));
            bool hybrid_profile_valid{};
            hybrid_pixel_profile_=hybrid_pixel_profile_from_game_profile(profile,hybrid_profile_valid);
            if(!hybrid_profile_valid)startup_error_json_=nlohmann::json{
                {"schemaVersion","noemancer.player-load/0.1"},{"success",false},
                {"code","player.hybrid-pixel-profile-invalid"}}.dump();
            if(const auto registry_relative=std::filesystem::path(profile.value("assetRegistry",std::string{})).lexically_normal();
                !registry_relative.empty()&&!registry_relative.is_absolute()&&*registry_relative.begin()!=std::filesystem::path("..")) {
                const auto package_root=std::filesystem::path(options_.player_profile_path).parent_path().parent_path();
                const auto registry_path=(package_root/registry_relative).lexically_normal();
                asset_registry_=AssetRegistry(registry_path.parent_path());
                if(startup_error_json_.empty()&&!asset_registry_.errors().empty())startup_error_json_=nlohmann::json{
                    {"schemaVersion","noemancer.player-load/0.1"},{"success",false},
                    {"code","player.asset-registry-invalid"},{"registry",registry_path.generic_string()},
                    {"errors",asset_registry_.errors()}}.dump();
            }
            if(startup_error_json_.empty()&&options_.headless)
                static_cast<void>(register_cooked_geometry_assets());
            if(startup_error_json_.empty()) {
                const auto input_actions=input_actions_from_profile(profile);
                if(!input_actions||!world_.configure_input_actions(*input_actions))
                    startup_error_json_=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
                        {"code","player.invalid-input-actions"}}.dump();
                else project_input_actions_=*input_actions;
            }
            if(startup_error_json_.empty()) {
                const auto hud_document=hud_document_from_profile(options_.player_profile_path,profile,startup_error_json_);
                if(hud_document) {project_hud_document_json_=*hud_document;
                    if(!world_.configure_project_hud(project_hud_document_json_))startup_error_json_=nlohmann::json{
                        {"schemaVersion","noemancer.player-load/0.1"},{"success",false},{"code","player.hud-document-invalid"}}.dump();}
            }
            const auto assembly_relative=profile.value("managedAssembly",std::string{});
            if(startup_error_json_.empty()&&!assembly_relative.empty()) {
                const auto assembly=(std::filesystem::path(options_.player_profile_path).parent_path().parent_path()/assembly_relative).lexically_normal();
                const auto load=nlohmann::json::parse(world_.scripting_project_load_assembly_json(
                    assembly,profile.value("managedConfiguration",std::string{"Release"})),nullptr,false);
                if(!load.is_object()||!load.value("success",false))startup_error_json_=load.dump();
            }
            if(startup_error_json_.empty()&&!signal_debug_ready(options_.debug_ready_event))
                startup_error_json_=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
                    {"code","player.debug-ready-failed"}}.dump();
            if(startup_error_json_.empty()&&!wait_for_debug_release(options_.debug_wait_event))
                startup_error_json_=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
                    {"code","player.debug-wait-failed"}}.dump();
            if(startup_error_json_.empty())static_cast<void>(world_.load_scene(*scene));
        }
    } else if (!options_.project_path.empty()) {
        const auto loaded=load_editor_project_json(options_.project_path);
        const auto receipt=nlohmann::json::parse(loaded,nullptr,false);
        if(!receipt.is_object()||!receipt.value("success",false))startup_error_json_=loaded;
    } else {
        project_input_actions_=default_input_action_definitions();
        const auto scene = !options_.reference_scene_id.empty()
            ? make_commercial_raster_reference_scene_document()
            : options_.animation_physics_stress
                ? make_animation_physics_stress_scene_document()
            : options_.render_stress_instances > 0
                ? make_render_stress_scene_document(options_.render_stress_instances,options_.render_stress_offscreen_percent)
                : make_bootstrap_scene_document();
        static_cast<void>(world_.load_scene(scene));
    }
    if(options_.player_mode&&options_.render_stress_instances>0)
        static_cast<void>(world_.load_scene(make_render_stress_scene_document(
            options_.render_stress_instances,options_.render_stress_offscreen_percent)));
    for (const auto& asset : asset_registry_.records()) {
        if (!asset.available || (asset.extension != ".glb" && asset.extension != ".fbx")) continue;
        if (options_.player_mode) {
            if(startup_error_json_.empty())startup_error_json_=nlohmann::json{
                {"schemaVersion","noemancer.player-load/0.1"},{"success",false},
                {"code","player.source-asset-forbidden"},{"assetId",asset.id},{"extension",asset.extension},
                {"detail","Packaged Player content must use cooked runtime artifacts, not source FBX/GLB."}}.dump();
            break;
        }
        const auto source = asset_registry_.source_path(asset);
        const auto decoded = asset.extension == ".fbx" ? decode_fbx_asset(source) : decode_glb_mesh(source);
        ++source_animation_decode_count_;
        ++source_geometry_decode_count_;
        if (decoded.valid && !decoded.skins.empty() && !decoded.animations.empty()) {
            ++offline_animation_compile_count_;
            static_cast<void>(world_.register_gltf_animations(asset.id, decoded));
        }
    }
    if(startup_error_json_.empty()&&!register_animation_clip_assets(world_))
        startup_error_json_=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},{"success",false},
            {"code","player.cooked-animation-load-failed"}}.dump();
    register_animation_state_machine_assets(world_);register_animation_graph_assets(world_);
    register_sprite_assets(world_);
    register_tilemap_assets(world_);
    register_audio_assets(world_);
    // Seed both blend groups so compute partitioning, alpha sorting, and the
    // dual-indirect graphics consumer remain visible and capture-testable.
    // The built-in bootstrap scene doubles as the renderer/VFX development
    // fixture. Project scenes own their effects and must never inherit debug
    // emitters merely because they are opened in the Editor.
    if (!options_.headless&&!options_.player_mode&&options_.project_path.empty()&&
        options_.reference_scene_id.empty()&&options_.render_stress_instances==0&&!options_.animation_physics_stress) {
        static_cast<void>(world_.vfx_spawn_json("vfx.debug-impact",{2.5F,2.2F,1.0F},0x4e4f454dULL));
        static_cast<void>(world_.vfx_spawn_json("vfx.debug-smoke",{1.65F,1.8F,0.55F},0x534d4f4b45ULL));
    }
    editor_ui_.refresh_world_model();
    if(!options_.editor_selected_asset_id.empty()&&!editor_ui_.select_asset(options_.editor_selected_asset_id))
        startup_error_json_=nlohmann::json{{"schemaVersion","noemancer.editor-startup/0.1"},{"success",false},
            {"code","editor.asset-selection-not-found"},{"assetId",options_.editor_selected_asset_id}}.dump();
    editor_ui_.set_project_settings_open(options_.editor_project_settings);
}

Application::~Application() = default;

std::string Application::load_editor_project_json(const std::filesystem::path& project_path) {
    using Json=nlohmann::json;
    if(package_busy_||play_world_||managed_debug_external_player_)
        return Json{{"schemaVersion","noemancer.editor-project-action/0.1"},{"success",false},
            {"code","project.session-busy"},{"detail","Stop Play, debugging and packaging before switching projects."}}.dump();
    const auto loaded=load_project(project_path);
    if(!loaded)return project_load_errors_json(loaded);

    AssetRegistry next_registry;
    if(!loaded.project->asset_roots.empty()) {
        next_registry=AssetRegistry(loaded.project->root/loaded.project->asset_roots.front());
        for(std::size_t index=1U;index<loaded.project->asset_roots.size();++index)
            static_cast<void>(next_registry.add_root(loaded.project->root/loaded.project->asset_roots[index]));
    }
    const auto scene_receipt=world_.load_scene(*loaded.startup_scene);
    if(!scene_receipt.success)return Json{{"schemaVersion","noemancer.editor-project-action/0.1"},{"success",false},
        {"code","project.scene-load-failed"},{"detail","The startup scene could not become the Edit World."}}.dump();
    if(!world_.configure_input_actions(loaded.project->input_actions))return Json{{"schemaVersion","noemancer.editor-project-action/0.1"},{"success",false},
        {"code","project.input-actions-invalid"},{"detail","Project input actions could not configure the Edit World."}}.dump();
    if(!world_.configure_project_hud(loaded.project->hud_document_json))return Json{{"schemaVersion","noemancer.editor-project-action/0.1"},{"success",false},
        {"code","project.hud-document-invalid"},{"detail","Project HUD could not configure the Edit World."}}.dump();
    asset_registry_=std::move(next_registry);project_root_=loaded.project->root;
    configure_persistence_store(loaded.project->project_id);
    project_input_actions_=loaded.project->input_actions;
    project_input_session_=std::make_unique<ProjectInputEditSession>(project_input_actions_,
        loaded.project->root/"noemancer.project.json");
    project_hud_document_json_=loaded.project->hud_document_json;
    hybrid_pixel_profile_=loaded.project->hybrid_pixel_profile;
    ++hybrid_pixel_profile_revision_;
    project_hybrid_pixel_session_=std::make_unique<ProjectHybridPixelAuthoring>(
        hybrid_pixel_profile_,loaded.project->root/"noemancer.project.json");
    script_project_root_.clear();script_project_path_.clear();Json compile=nullptr;
    if(loaded.project->script_project) {
        script_project_root_=loaded.project->root;script_project_path_=loaded.project->root / *loaded.project->script_project;
        const auto configured=Json::parse(world_.scripting_project_configure_json(script_project_root_,script_project_path_),nullptr,false);
        if(configured.is_object()&&configured.value("success",false))
            compile=Json::parse(world_.scripting_project_compile_json("Debug"),nullptr,false);
        else compile=configured;
    }
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.extension!=".glb"&&asset.extension!=".fbx"))continue;
        const auto source=asset_registry_.source_path(asset);
        const auto decoded=asset.extension==".fbx"?decode_fbx_asset(source):decode_glb_mesh(source);
        if(decoded.valid&&!decoded.skins.empty()&&!decoded.animations.empty())
            static_cast<void>(world_.register_gltf_animations(asset.id,decoded));
    }
    // Asset failures remain visible through the Registry/log, but cannot leave
    // project switching half-committed after the new World and Registry have
    // already become authoritative. Player startup remains fail-closed.
    static_cast<void>(register_animation_clip_assets(world_));
    register_animation_state_machine_assets(world_);register_animation_graph_assets(world_);
    register_sprite_assets(world_);register_tilemap_assets(world_);register_audio_assets(world_);
    EditorProjectContext context{.project_id=loaded.project->project_id,.name=loaded.project->name,
        .root=loaded.project->root.generic_string(),.startup_scene=loaded.project->startup_scene.generic_string()};
    if(loaded.project->script_project)context.script_project=loaded.project->script_project->generic_string();
    for(const auto& root:loaded.project->asset_roots)context.asset_roots.push_back(root.generic_string());
    context.input_actions=loaded.project->input_actions;
    context.hybrid_pixel_profile=loaded.project->hybrid_pixel_profile;
    context.hybrid_pixel_profile_revision=project_hybrid_pixel_session_->revision();
    context.hybrid_pixel_profile_can_undo=project_hybrid_pixel_session_->can_undo();
    context.hybrid_pixel_profile_can_redo=project_hybrid_pixel_session_->can_redo();
    editor_ui_.set_project_context(std::move(context));
    return Json{{"schemaVersion","noemancer.editor-project-action/0.1"},{"success",true},{"code","ok"},
        {"operation","project.open"},{"detail","Project opened in the Editor."},{"projectPath",project_root_.generic_string()},
        {"scene",loaded.project->startup_scene.generic_string()},{"scriptCompile",std::move(compile)}}.dump();
}

void Application::apply_project_request(const EditorProjectRequest& request) {
    using Json=nlohmann::json;std::string result;
    if(request.command==EditorProjectCommand::create) {
        const auto created=create_project_workspace_json({request.path,request.name});
        const auto receipt=Json::parse(created,nullptr,false);
        result=receipt.is_object()&&receipt.value("success",false)?load_editor_project_json(receipt.value("projectPath",std::string{})):created;
    } else result=load_editor_project_json(request.path);
    editor_ui_.set_project_status(result);
    const auto receipt=Json::parse(result,nullptr,false);
    if(receipt.is_object()&&receipt.value("success",false))logger_.info("project.editor",result);
    else logger_.error("project.editor",result);
}

void Application::apply_project_input_map_request(
    const ProjectSettingsInputMapPanelRequest& request) {
    using Json = nlohmann::json;
    if (!request.intent || !project_input_session_) {
        const auto detail = project_input_session_
            ? std::string("The Input Map edit request did not contain a domain intent.")
            : std::string("Open a project before editing its Input Map.");
        editor_ui_.set_last_action_status(detail);
        logger_.error("project.input-map", Json{{"success", false},
            {"code", "project.input-map.session-unavailable"}, {"detail", detail}}.dump());
        return;
    }

    const auto& intent = *request.intent;
    const ProjectInputEditOptions options{.expected_revision = intent.base_revision};
    ProjectInputEditRequest edit;
    switch (intent.kind) {
    case InputMapIntentKind::add_action:
        edit = ProjectInputEditRequest::add_action(intent.action_id, intent.action_kind,
            {{intent.source, intent.scale, intent.dead_zone}}, options);
        break;
    case InputMapIntentKind::remove_action:
        edit = ProjectInputEditRequest::remove_action(intent.action_id, options);
        break;
    case InputMapIntentKind::add_binding:
        edit = ProjectInputEditRequest::add_binding(intent.action_id,
            {intent.source, intent.scale, intent.dead_zone}, options);
        break;
    case InputMapIntentKind::remove_binding:
        edit = ProjectInputEditRequest::remove_binding(intent.action_id, intent.previous_source, options);
        break;
    case InputMapIntentKind::rebind_binding:
        edit = ProjectInputEditRequest::remap_binding(intent.action_id, intent.previous_source,
            {intent.source, intent.scale, intent.dead_zone}, options);
        break;
    }

    const auto receipt = project_input_session_->apply(edit);
    Json diagnostics = Json::array();
    for (const auto& diagnostic : receipt.diagnostics) {
        diagnostics.push_back({{"code", diagnostic.code}, {"path", diagnostic.path},
            {"message", diagnostic.message},
            {"severity", diagnostic.severity == ProjectInputDiagnosticSeverity::error ? "error" : "warning"}});
    }
    const Json observation{{"schemaVersion", std::string(project_input_authoring_schema)},
        {"success", receipt.success}, {"changed", receipt.changed}, {"persisted", receipt.persisted},
        {"code", receipt.code}, {"detail", receipt.detail}, {"revision", receipt.revision},
        {"requestId", request.request_id}, {"diagnostics", std::move(diagnostics)}};
    if (!receipt) {
        editor_ui_.set_last_action_status(receipt.detail);
        logger_.error("project.input-map", observation.dump());
        return;
    }

    project_input_actions_ = receipt.actions;
    if (!world_.configure_input_actions(project_input_actions_) ||
        (play_world_ && !play_world_->configure_input_actions(project_input_actions_))) {
        const auto detail = std::string("Persisted Input Map could not be hot-applied to the active World.");
        editor_ui_.set_last_action_status(detail);
        logger_.error("project.input-map.hot-apply", detail);
        return;
    }
    editor_ui_.set_project_input_actions(project_input_actions_, receipt.revision);
    editor_ui_.set_last_action_status(receipt.detail);
    logger_.info("project.input-map", observation.dump());
}

void Application::apply_hybrid_pixel_profile_request(
    const HybridPixelProfilePanelRequest& request) {
    using Json=nlohmann::json;
    if(!project_hybrid_pixel_session_) {
        const auto detail=std::string("Open a project before editing its Hybrid Pixel Profile.");
        editor_ui_.set_last_action_status(detail);
        logger_.error("project.hybrid-pixel-profile",Json{{"success",false},
            {"code","project.hybrid-pixel-profile.session-unavailable"},{"detail",detail}}.dump());
        return;
    }
    const ProjectHybridPixelEditOptions options{
        .expected_revision=request.expected_revision,.dry_run=request.dry_run};
    ProjectHybridPixelEditReceipt receipt;
    switch(request.kind) {
    case HybridPixelProfilePanelRequestKind::apply:
        receipt=project_hybrid_pixel_session_->replace(request.profile,options);
        break;
    case HybridPixelProfilePanelRequestKind::disable_remove:
        receipt=project_hybrid_pixel_session_->remove(options);
        break;
    case HybridPixelProfilePanelRequestKind::undo:
        receipt=project_hybrid_pixel_session_->undo(options);
        break;
    case HybridPixelProfilePanelRequestKind::redo:
        receipt=project_hybrid_pixel_session_->redo(options);
        break;
    }
    const auto evidence=receipt.to_json();
    editor_ui_.set_last_action_status(receipt.detail);
    if(!receipt) {
        logger_.error("project.hybrid-pixel-profile",evidence);
        return;
    }
    if(request.dry_run) {
        logger_.info("project.hybrid-pixel-profile",evidence);
        return;
    }
    hybrid_pixel_profile_=receipt.profile;
    ++hybrid_pixel_profile_revision_;
    editor_ui_.set_project_hybrid_pixel_profile(project_hybrid_pixel_session_->profile(),
        project_hybrid_pixel_session_->revision(),project_hybrid_pixel_session_->can_undo(),
        project_hybrid_pixel_session_->can_redo());
    logger_.info("project.hybrid-pixel-profile",evidence);
}

void Application::apply_source_open_request(const EditorSourceOpenRequest& request) {
    const auto result=launch_source_editor_json(project_root_,request.path,request.line,request.column,false);
    const auto receipt=nlohmann::json::parse(result,nullptr,false);
    const auto detail=receipt.is_object()?receipt.value("detail",std::string{"External source editor request failed."}):
        std::string{"External source editor returned invalid evidence."};
    editor_ui_.set_last_action_status(detail);
    if(receipt.is_object()&&receipt.value("success",false))logger_.info("source.editor",result);
    else logger_.error("source.editor",result);
}

void Application::apply_script_build_completion(const EditorScriptBuildCompletion& completion) {
    using Json=nlohmann::json;
    if(!play_world_||completion.configuration!="Debug")return;
    const auto edit_result=Json::parse(completion.result_json,nullptr,false);
    Json status{{"schemaVersion","noemancer.play-script-reload/0.1"},{"success",false},
        {"code","play.script-build-failed"},{"detail","Play World kept the previous C# assembly because the new build failed."},
        {"trigger",completion.trigger},{"configuration",completion.configuration},{"editCompile",edit_result},
        {"playCompile",nullptr},{"activation","not-applied"}};
    if(edit_result.is_object()&&edit_result.value("success",false)) {
        const auto play_result=Json::parse(play_world_->scripting_project_compile_json("Debug"),nullptr,false);
        const auto success=play_result.is_object()&&play_result.value("success",false);
        status["success"]=success;status["code"]=success?"ok":"play.script-reload-compile-failed";
        status["detail"]=success?"New C# assembly is armed for Play World; state migrates on the next managed callback.":
            "Play World kept the previous C# assembly because reload preparation failed.";
        status["playCompile"]=play_result;status["activation"]=success?"next-managed-callback":"not-applied";
    }
    const auto encoded=status.dump();editor_ui_.set_play_script_reload_status(encoded);
    editor_ui_.set_last_action_status(status.value("detail",std::string{}));
    if(status.value("success",false))logger_.info("play.script_reload",encoded);else logger_.error("play.script_reload",encoded);
}

void Application::apply_package_request(const EditorPackageRequest& request) {
    if(package_busy_||project_root_.empty())return;
    package_busy_=true;editor_ui_.set_package_status(true,nlohmann::json{{"schema","noemancer.windows-package/0.1"},
        {"success",false},{"code","package.running"},{"detail","Package plan and atomic staging are running."}}.dump());
    WindowsPackageOptions options{.project_path=project_root_,.output_path=request.output_path,
        .runtime_executable=options_.runtime_executable,.target_profile=request.target_profile,.dry_run=request.dry_run};
    package_future_=std::async(std::launch::async,[options]{return run_windows_package_json(options);});
}

void Application::poll_package_job() {
    if(!package_busy_||!package_future_.valid()||package_future_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return;
    auto result=package_future_.get();package_busy_=false;editor_ui_.set_package_status(false,result);
    const auto parsed=nlohmann::json::parse(result,nullptr,false);
    if(parsed.is_object()&&parsed.value("success",false))logger_.info("package.editor",result);
    else logger_.error("package.editor",result);
}

void Application::register_sprite_assets(World& world) {
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.kind!="Sprite"&&!asset.relative_path.ends_with(".sprite.json")))continue;
        std::ifstream input(asset_registry_.source_path(asset),std::ios::binary);
        if(!input)continue;
        const std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        auto parsed=SpriteAssetCodec::parse_json(source);
        if(parsed)static_cast<void>(world.register_sprite_asset(std::move(*parsed.document)));
    }
}

bool Application::register_cooked_geometry_assets() {
    constexpr std::uintmax_t maximum_artifact_bytes = 512U * 1024U * 1024U;
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||asset.extension!=".meshbin")continue;
        const auto path=asset_registry_.source_path(asset);
        const auto fail=[&](const std::string_view code,const std::string_view detail,
                            const std::uintmax_t bytes=0U,const std::string_view payload_hash=std::string_view{}) {
            const auto artifact=nlohmann::json{{"schemaVersion","noemancer.player-geometry-loading/0.1"},
                {"success",false},{"assetId",asset.id},{"path",asset.relative_path},{"code",code},
                {"detail",detail},{"bytes",bytes},{"payloadHash",payload_hash}};
            logger_.error("geometry.artifact",artifact.dump());
            startup_error_json_=nlohmann::json{{"schemaVersion","noemancer.player-load/0.1"},
                {"success",false},{"code","player.cooked-geometry-load-failed"},{"assetId",asset.id},
                {"path",asset.relative_path},{"artifactCode",code},{"detail",detail},
                {"bytes",bytes},{"payloadHash",payload_hash}}.dump();
            return false;
        };
        std::error_code size_error;
        const auto bytes=std::filesystem::file_size(path,size_error);
        if(size_error||bytes==0U||bytes>maximum_artifact_bytes)
            return fail("geometry.artifact-source-invalid",
                "Cooked mesh payload is unavailable or exceeds the Runtime byte budget.",
                size_error?0U:bytes);
        std::ifstream input(path,std::ios::binary);
        if(!input)return fail("geometry.artifact-source-unavailable","Cooked mesh payload could not be opened.",bytes);
        const std::vector<char> payload{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        if(static_cast<std::uintmax_t>(payload.size())!=bytes)
            return fail("geometry.artifact-source-changed","Cooked mesh payload changed while it was being read.",bytes);
        const auto result=load_mesh_runtime_artifact(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),payload.size()),
            asset.id,{},asset.content_hash);
        if(!result.success)
            return fail(result.code,result.detail,bytes,result.payload_hash);
        ++cooked_geometry_load_count_;
        logger_.info("geometry.artifact",nlohmann::json{{"assetId",asset.id},{"code","ok"},
            {"payloadHash",result.payload_hash},{"lodCount",result.lod_count},
            {"primitiveCount",result.mesh.primitives.size()},{"imageCount",result.mesh.images.size()},
            {"sourceDecode",false},{"offlineCompile",false}}.dump());
    }
    return true;
}

bool Application::register_animation_clip_assets(World& world) {
    constexpr std::uintmax_t maximum_artifact_bytes = 256U * 1024U * 1024U;
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||asset.extension!=".animbin")continue;
        const auto path=asset_registry_.source_path(asset);
        std::error_code size_error;
        const auto bytes=std::filesystem::file_size(path,size_error);
        if(size_error||bytes==0U||bytes>maximum_artifact_bytes) {
            logger_.error("animation.artifact",nlohmann::json{{"assetId",asset.id},
                {"code","animation.artifact-source-invalid"},{"bytes",size_error?0U:bytes}}.dump());
            return false;
        }
        std::ifstream input(path,std::ios::binary);
        if(!input) {
            logger_.error("animation.artifact",nlohmann::json{{"assetId",asset.id},
                {"code","animation.artifact-source-unavailable"}}.dump());
            return false;
        }
        const std::vector<char> source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        if(source.size()!=bytes) {
            logger_.error("animation.artifact",nlohmann::json{{"assetId",asset.id},
                {"code","animation.artifact-source-changed"}}.dump());
            return false;
        }
        const auto result=world.register_cooked_animation(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(source.data()),source.size()),
            asset.id,{},asset.content_hash);
        if(!result.success) {
            logger_.error("animation.artifact",nlohmann::json{{"assetId",asset.id},{"code",result.code},
                {"detail",result.detail},{"payloadHash",result.payload_hash}}.dump());
            return false;
        }
        ++cooked_animation_load_count_;
        logger_.info("animation.artifact",nlohmann::json{{"assetId",asset.id},{"code","ok"},
            {"payloadHash",result.payload_hash},{"jointCount",result.joint_count},
            {"clipAssets",result.clip_assets},{"sourceDecode",false},{"offlineCompile",false}}.dump());
    }
    // A packaged Player consumes only .animbin records above. AnimationClip
    // descriptors and their FBX/GLB build inputs are Editor/Cook concerns.
    if(options_.player_mode)return true;
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||!asset.relative_path.ends_with(".animation-clip.json"))continue;
        const auto descriptor_path=asset_registry_.source_path(asset);
        std::error_code descriptor_size_error;
        const auto descriptor_bytes=std::filesystem::file_size(descriptor_path,descriptor_size_error);
        if(descriptor_size_error||descriptor_bytes>animation_clip_asset_max_source_bytes)return false;
        std::ifstream descriptor_input(descriptor_path,std::ios::binary);
        if(!descriptor_input)return false;
        const std::string descriptor_source{std::istreambuf_iterator<char>(descriptor_input),
            std::istreambuf_iterator<char>()};
        if(descriptor_source.size()>animation_clip_asset_max_source_bytes)return false;
        const auto descriptor=AnimationClipAssetCodec::parse_json(descriptor_source);
        if(!descriptor||descriptor.document->asset_id!=asset.id)return false;
        const auto* source_asset=asset_registry_.find(descriptor.document->source_asset);
        if(source_asset==nullptr||!source_asset->available||
            (source_asset->extension!=".fbx"&&source_asset->extension!=".glb"))return false;
        const auto source_path=asset_registry_.source_path(*source_asset);
        const auto decoded=source_asset->extension==".fbx"?decode_fbx_asset(source_path):decode_glb_mesh(source_path);
        ++source_animation_decode_count_;
        if(!decoded.valid)return false;
        const auto compression=descriptor.document->compression=="ozz_hierarchical_key_reduction"?
            AnimationCompressionMode::ozz_hierarchical_key_reduction:AnimationCompressionMode::ozz_runtime_baseline;
        AnimationRuntime cooker;
        ++offline_animation_compile_count_;
        const auto cooked=cooker.cook_gltf_animation_artifact(asset.id,source_asset->content_hash,decoded,
            descriptor.document->skin_index,descriptor.document->animation_index,compression);
        if(!cooked.success)return false;
        const auto loaded=world.register_cooked_animation(cooked.payload,asset.id,source_asset->content_hash,
            cooked.payload_hash);
        if(!loaded.success)return false;
        ++cooked_animation_load_count_;
        logger_.info("animation.artifact",nlohmann::json{{"assetId",asset.id},{"code","ok"},
            {"payloadHash",loaded.payload_hash},{"jointCount",loaded.joint_count},
            {"clipAssets",loaded.clip_assets},{"sourceDecode",true},{"offlineCompile",true},
            {"mode","editor-source-cook"}}.dump());
    }
    return true;
}

void Application::register_animation_state_machine_assets(World& world) {
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.kind!="AnimationStateMachine"&&
           !asset.relative_path.ends_with(".animation-state-machine.json")))continue;
        std::ifstream input(asset_registry_.source_path(asset),std::ios::binary);if(!input)continue;
        const std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        auto parsed=AnimationStateMachineCodec::parse_json(source);
        if(parsed)static_cast<void>(world.register_animation_state_machine(std::move(*parsed.document)));
    }
}

void Application::register_animation_graph_assets(World& world) {
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.kind!="AnimationGraph"&&
           !asset.relative_path.ends_with(".animation-graph.json")))continue;
        std::ifstream input(asset_registry_.source_path(asset),std::ios::binary);if(!input)continue;
        const std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        auto parsed=AnimationGraphCodec::parse_json(source);
        if(parsed)static_cast<void>(world.register_animation_graph(std::move(*parsed.document)));
    }
}

void Application::register_tilemap_assets(World& world) {
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.kind!="TilePalette"&&!asset.relative_path.ends_with(".tile-palette.json")))continue;
        std::ifstream input(asset_registry_.source_path(asset),std::ios::binary);if(!input)continue;
        const std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        auto parsed=TilemapAssetCodec::parse_palette_json(source);if(parsed)static_cast<void>(world.register_tile_palette(std::move(*parsed.document)));
    }
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.kind!="Tilemap"&&!asset.relative_path.ends_with(".tilemap.json")))continue;
        std::ifstream input(asset_registry_.source_path(asset),std::ios::binary);if(!input)continue;
        const std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        auto parsed=TilemapAssetCodec::parse_tilemap_json(source);if(parsed)static_cast<void>(world.register_tilemap_asset(std::move(*parsed.document)));
    }
}

void Application::register_audio_assets(World& world) {
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.extension!=".wav"&&asset.extension!=".ogg"&&
            asset.extension!=".flac"&&asset.extension!=".mp3"))continue;
        const auto storage=asset.source_bytes>=1024U*1024U?AudioAssetStorage::stream:AudioAssetStorage::resident;
        static_cast<void>(world.register_audio_asset_json(asset.id,asset.content_hash,storage));
    }
}

int Application::run() {
    if (!startup_error_json_.empty()) {
        logger_.error("project.load", startup_error_json_);
        return 21;
    }
    if (!engine_host_.initialize(options_.headless)) {
        logger_.error("engine.initialize", engine_host_.last_error());
        return 20;
    }
    editor_ui_.set_engine_status(engine_host_.status_json());
    logger_.info("engine.modules", engine_host_.status_json());
    const auto result = options_.headless ? run_headless() : run_interactive();
    engine_host_.shutdown();
    return result;
}

void Application::tick_frame(const double delta_seconds) {
    engine_host_.run_frame(delta_seconds, [this] {
        if (options_.headless||options_.player_mode||options_.animation_physics_stress) world_.tick(1.0F / 60.0F);
        else if (play_world_ && (!play_paused_ || play_single_step_)) {
            play_world_->tick(1.0F / 60.0F);
            play_single_step_=false;
        }
    });
    process_persistence_requests(active_world());
}

void Application::configure_persistence_store(std::string project_id) {
    if(project_id.empty()){persistence_store_.reset();return;}
    persistence_store_=std::make_unique<GamePersistenceStore>(default_user_data_root(),std::move(project_id));
}

void Application::process_persistence_requests(World& world) {
    const auto requests=world.consume_persistence_requests();
    for(const auto& request:requests) {
        bool success=false;std::string code="persistence.store-unavailable";
        std::string detail="No project persistence store is configured.";
        if(persistence_store_) {
            if(request.action=="save") {
                const auto stored=persistence_store_->write(PersistedDocumentKind::save,request.slot_id,world.save_capture_json());
                success=stored.success;code=stored.code;detail=stored.detail;
            } else if(request.action=="load") {
                const auto loaded=persistence_store_->read(PersistedDocumentKind::save,request.slot_id);
                success=loaded.success;code=loaded.code;detail=loaded.detail;
                if(success) {
                    const auto receipt=nlohmann::json::parse(world.save_restore_json(loaded.document_json),nullptr,false);
                    success=receipt.is_object()&&receipt.value("success",false);
                    code=receipt.is_object()?receipt.value("code",std::string("persistence.restore-failed")):"persistence.restore-invalid-receipt";
                    detail=success?"Persistence slot restored into the active World.":receipt.dump();
                }
            } else if(request.action=="replay-start") {
                const auto receipt=nlohmann::json::parse(world.replay_start_json(),nullptr,false);
                success=receipt.is_object()&&receipt.value("success",false);
                code=receipt.is_object()?receipt.value("code",std::string("replay.start-failed")):"replay.start-invalid-receipt";
                detail=success?"Input replay recording started.":receipt.dump();
            } else if(request.action=="replay-stop") {
                const auto replay=world.replay_stop_json();
                const auto stored=persistence_store_->write(PersistedDocumentKind::replay,request.slot_id,replay);
                success=stored.success;code=stored.code;detail=stored.detail;
            } else if(request.action=="replay-play") {
                const auto loaded=persistence_store_->read(PersistedDocumentKind::replay,request.slot_id);
                success=loaded.success;code=loaded.code;detail=loaded.detail;
                if(success) {
                    const auto receipt=nlohmann::json::parse(world.replay_apply_json(loaded.document_json),nullptr,false);
                    success=receipt.is_object()&&receipt.value("success",false);
                    code=receipt.is_object()?receipt.value("code",std::string("replay.apply-failed")):"replay.apply-invalid-receipt";
                    detail=success?"Input replay applied through the fixed-tick World path.":receipt.dump();
                }
            } else {code="persistence.action-unsupported";detail="Runtime received an unknown persistence action.";}
        }
        world.complete_persistence_request(request,success,code,detail);
        if(!success)logger_.error("gameplay.persistence",nlohmann::json{{"requestSequence",request.sequence},
            {"action",request.action},{"slotId",request.slot_id},{"code",code},{"detail",detail}}.dump());
    }
}

World& Application::active_world() noexcept { return play_world_?*play_world_:world_; }

void Application::apply_simulation_command(const EditorSimulationCommand command) {
    if (command==EditorSimulationCommand::play && !play_world_) {
        play_base_scene_json_=world_.canonical_scene_json();
        play_base_revision_=world_.revision();
        const auto parsed=SceneDocumentCodec::parse_json(play_base_scene_json_,"play://session");
        if (!parsed) return;
        if(!script_project_path_.empty()) {
            const auto compile=nlohmann::json::parse(editor_ui_.compile_scripts("Debug"),nullptr,false);
            if(compile.is_discarded()||!compile.value("success",false)) {
                logger_.error("play.script_preflight",compile.is_discarded()?"invalid compile receipt":compile.dump());
                return;
            }
            logger_.info("play.script_preflight",compile.dump());
        }
        auto candidate=std::make_unique<World>();
        if(!candidate->configure_input_actions(project_input_actions_))return;
        if(!candidate->configure_project_hud(project_hud_document_json_))return;
        register_animation_state_machine_assets(*candidate);register_animation_graph_assets(*candidate);
        register_sprite_assets(*candidate);
        register_tilemap_assets(*candidate);
        register_audio_assets(*candidate);
        if(!script_project_path_.empty()) {
            static_cast<void>(candidate->scripting_project_configure_json(script_project_root_,script_project_path_));
            const auto play_compile=nlohmann::json::parse(candidate->scripting_project_compile_json("Debug"),nullptr,false);
            if(play_compile.is_discarded()||!play_compile.value("success",false)) {
                logger_.error("play.script_load",play_compile.is_discarded()?"invalid compile receipt":play_compile.dump());
                return;
            }
        }
        for (const auto& asset:asset_registry_.records()) {
            if (!asset.available || (asset.extension!=".glb"&&asset.extension!=".fbx")) continue;
            const auto source=asset_registry_.source_path(asset);
            const auto decoded=asset.extension==".fbx"?decode_fbx_asset(source):decode_glb_mesh(source);
            if(decoded.valid&&!decoded.skins.empty()&&!decoded.animations.empty())
                static_cast<void>(candidate->register_gltf_animations(asset.id,decoded));
        }
        if(!register_animation_clip_assets(*candidate))return;
        if (!candidate->load_scene(*parsed.document).success) return;
        for (const auto& entity:candidate->entity_views()) {
            if (!entity.mesh_renderer) continue;
            static_cast<void>(candidate->gameplay_ability_grant_json(entity.id,"ability.combat.impact"));
            break;
        }
        play_world_=std::move(candidate);
        play_paused_=false;
        editor_ui_.set_play_script_reload_status({});
        editor_ui_.set_simulation_state(EditorSimulationState::playing);
    } else if (command==EditorSimulationCommand::pause && play_world_) {
        play_paused_=true; editor_ui_.set_simulation_state(EditorSimulationState::paused);
    } else if (command==EditorSimulationCommand::resume && play_world_) {
        play_paused_=false; editor_ui_.set_simulation_state(EditorSimulationState::playing);
    } else if (command==EditorSimulationCommand::step && play_world_) {
        play_paused_=true; play_single_step_=true; editor_ui_.set_simulation_state(EditorSimulationState::paused);
    } else if (command==EditorSimulationCommand::apply_back_and_stop && play_world_) {
        const auto plan=plan_play_world_apply(play_base_scene_json_,play_world_->runtime_authoring_scene_json(),play_base_revision_);
        const auto selection=plan_play_world_apply_selection(plan,editor_ui_.selected_play_world_change_ids());
        logger_.info("play.apply_back.plan",plan.to_json());
        logger_.info("play.apply_back.selection",selection.to_json());
        if(!selection.valid) {
            editor_ui_.set_last_action_status(selection.detail);
            return;
        }
        if(selection.changes.empty()) {
            editor_ui_.set_last_action_status("No Play World changes were selected.");
        } else if(world_.canonical_scene_json()!=play_base_scene_json_) {
            editor_ui_.set_last_action_status("Apply Back refused: Edit World changed after Play began.");
            logger_.error("play.apply_back.conflict","Edit World diverged from the Play baseline.");
            return;
        } else {
            const auto preview=nlohmann::json::parse(world_.replace_scene_document_json(
                selection.candidate_scene_json,play_base_revision_,"editor.play-world.apply-back",true),nullptr,false);
            if(preview.is_discarded()||!preview.value("success",false)) {
                const auto detail=preview.is_discarded()?std::string("Apply Back preview returned invalid JSON."):
                    preview.value("detail",std::string("Apply Back preview failed."));
                editor_ui_.set_last_action_status(detail);logger_.error("play.apply_back.preview",detail);return;
            }
            const auto receipt=nlohmann::json::parse(world_.replace_scene_document_json(
                selection.candidate_scene_json,play_base_revision_,"editor.play-world.apply-back",false),nullptr,false);
            if(receipt.is_discarded()||!receipt.value("success",false)) {
                const auto detail=receipt.is_discarded()?std::string("Apply Back returned invalid JSON."):
                    receipt.value("detail",std::string("Apply Back failed."));
                editor_ui_.set_last_action_status(detail);logger_.error("play.apply_back.commit",detail);return;
            }
            editor_ui_.set_last_action_status("Applied "+std::to_string(selection.changes.size())+
                " selected Play World change(s) as one undoable transaction.");
            logger_.info("play.apply_back.commit",receipt.dump());
        }
        play_world_.reset();play_paused_=false;play_single_step_=false;
        play_base_scene_json_.clear();
        editor_ui_.set_simulation_state(EditorSimulationState::edit);editor_ui_.refresh_world_model();
    } else if (command==EditorSimulationCommand::stop && play_world_) {
        play_world_.reset(); play_paused_=false; play_single_step_=false;
        play_base_scene_json_.clear();
        editor_ui_.set_simulation_state(EditorSimulationState::edit);
        editor_ui_.refresh_world_model();
    }
}

void Application::apply_managed_debug_request(const EditorManagedDebugRequest& request) {
    using Json=nlohmann::json;
    auto invoke=[&](const std::string_view command,const Json& arguments=Json::object()) {
        return Json::parse(world_.scripting_debug_session_request_json(command,arguments.dump(),5000U),nullptr,false);
    };
    Json result;
    if(request.command==EditorManagedDebugCommand::start_attach) {
        Json stages=Json::array();
        const auto package_root=std::filesystem::path(request.package_output_path).lexically_normal();
        const auto profile_path=package_root/"config"/"game-profile.json";
        const auto profile=load_player_profile_document(profile_path);
        const auto executable_name=profile.value("executable",std::string{});
        const auto player_executable=(package_root/"bin"/executable_name).lexically_normal();
        const auto executable_relative=player_executable.lexically_relative(package_root/"bin");
        std::error_code player_error;
        const auto valid_target=supported_game_profile_schema(profile)&&
            !executable_name.empty()&&!executable_relative.empty()&&*executable_relative.begin()!=std::filesystem::path("..")&&
            std::filesystem::is_regular_file(player_executable,player_error);
        if(!valid_target) {
            result={{"schemaVersion","noemancer.editor-managed-debug-action/0.1"},{"success",false},
                {"code","scripting.player-debug-target-invalid"},{"operation","debug.editor.launch-player"},
                {"profile",profile_path.generic_string()},{"stages",std::move(stages)}};
            managed_debug_last_action_json_=result.dump();logger_.error("scripting.debug.editor",managed_debug_last_action_json_);return;
        }
        auto stage=Json::parse(world_.scripting_debug_session_start_json(),nullptr,false);stages.push_back(stage);
        bool success=!stage.is_discarded()&&stage.value("success",false);
        if(success){stage=invoke("initialize",{{"clientID","noemancer-editor"},{"adapterID","coreclr"},{"pathFormat","path"},
            {"linesStartAt1",true},{"columnsStartAt1",true}});stages.push_back(stage);success=stage.value("success",false);}
        if(success){managed_debug_player_=std::make_unique<DebugPlayerProcess>();success=managed_debug_player_->launch(player_executable)&&
                managed_debug_player_->wait_ready(std::chrono::seconds(10));
            stages.push_back({{"schemaVersion","noemancer.player-process-action/0.1"},{"success",success},
                {"code",success?"ok":managed_debug_player_->error().empty()?"player.debug-ready-timeout":managed_debug_player_->error()},
                {"processId",managed_debug_player_->process_id()}});}
        if(success){stage=invoke("attach",{{"processId",managed_debug_player_->process_id()},{"justMyCode",false}});
            stages.push_back(stage);success=stage.value("success",false);}
        if(success&&!request.source_path.empty()) {stage=invoke("setBreakpoints",{{"source",{{"path",request.source_path}}},
            {"breakpoints",Json::array({{{"line",request.line}}})},{"sourceModified",false}});
            stages.push_back(stage);success=stage.value("success",false);}
        if(success){stage=invoke("configurationDone");stages.push_back(stage);success=stage.value("success",false);}
        if(managed_debug_player_)managed_debug_player_->release();
        managed_debug_external_player_=success;
        if(!success) {static_cast<void>(world_.scripting_debug_session_stop_json(5000U));
            if(managed_debug_player_)managed_debug_player_->terminate();}
        result={{"schemaVersion","noemancer.editor-managed-debug-action/0.1"},{"success",success},
            {"code",success?"ok":"scripting.debug-player-launch-failed"},{"operation","debug.editor.launch-player"},
            {"target",{{"kind","packaged-player"},{"program",player_executable.generic_string()},
                {"profile",profile_path.generic_string()}}},{"stages",std::move(stages)}};
    } else if(request.command==EditorManagedDebugCommand::set_breakpoint) {
        result=invoke("setBreakpoints",{{"source",{{"path",request.source_path}}},
            {"breakpoints",Json::array({{{"line",request.line}}})},{"sourceModified",false}});
    } else if(request.command==EditorManagedDebugCommand::stop) {
        result=Json::parse(world_.scripting_debug_session_stop_json(5000U),nullptr,false);
        if(managed_debug_player_)managed_debug_player_->terminate();
        managed_debug_external_player_=false;
    } else if(request.command==EditorManagedDebugCommand::refresh_stack) {
        auto threads=invoke("threads");Json stack=nullptr;
        auto thread_id=request.thread_id;
        const auto body=threads.value("body",Json::object());const auto values=body.value("threads",Json::array());
        if(!values.empty())thread_id=values.front().value("id",thread_id);
        if(threads.value("success",false))stack=invoke("stackTrace",{{"threadId",thread_id},{"startFrame",0},{"levels",64}});
        const auto success=threads.value("success",false)&&stack.is_object()&&stack.value("success",false);
        result={{"schemaVersion","noemancer.editor-managed-debug-action/0.1"},{"success",success},
            {"code",success?"ok":"scripting.debug-stack-refresh-failed"},{"operation","debug.editor.refresh-stack"},
            {"threadId",thread_id},{"threads",threads},{"stack",stack}};
    } else {
        const auto command=request.command==EditorManagedDebugCommand::continue_execution?"continue":
            request.command==EditorManagedDebugCommand::pause?"pause":request.command==EditorManagedDebugCommand::step_over?"next":
            request.command==EditorManagedDebugCommand::step_in?"stepIn":"stepOut";
        result=invoke(command,{{"threadId",request.thread_id},{"singleThread",false}});
    }
    if(result.is_discarded())result={{"schemaVersion","noemancer.editor-managed-debug-action/0.1"},{"success",false},
        {"code","scripting.debug-invalid-action-result"}};
    managed_debug_last_action_json_=result.dump();logger_.info("scripting.debug.editor",managed_debug_last_action_json_);
}

int Application::run_headless() {
    const auto frame_count = options_.frames == 0 ? 3U : options_.frames;
    logger_.info("runtime.start", "headless");
    logger_.info("runtime.native_dependencies",native_runtime_dependencies_json());
    nlohmann::json input_injections = nlohmann::json::array();
    for (const auto& sample : options_.input_samples) {
        input_injections.push_back(nlohmann::json::parse(
            active_world().inject_input_json(sample.source, sample.value), nullptr, false));
    }
    std::ranges::stable_sort(options_.input_events,{},&RuntimeInputEvent::frame);
    std::size_t input_event_index{};
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        while(input_event_index<options_.input_events.size()&&options_.input_events[input_event_index].frame==frame) {
            const auto& event=options_.input_events[input_event_index++];
            input_injections.push_back(nlohmann::json::parse(
                active_world().inject_input_json(event.source,event.value),nullptr,false));
        }
        if (options_.vfx_respawn_interval > 0 && frame > 0 && frame % options_.vfx_respawn_interval == 0) {
            static_cast<void>(active_world().vfx_spawn_json("vfx.debug-impact",{2.5F,2.2F,1.0F},0x4e4f454dULL+frame));
        }
        tick_frame(1.0 / 60.0);
    }
    if (!options_.input_samples.empty()||!options_.input_events.empty()) {
        logger_.info("runtime.input_probe", nlohmann::json{
            {"schemaVersion", "noemancer.runtime-input-probe/0.1"},
            {"injections", std::move(input_injections)},
            {"actions", nlohmann::json::parse(active_world().input_observation_json(), nullptr, false)}}.dump());
    }
    const auto parse_observation=[](const std::string& value) {
        auto parsed=nlohmann::json::parse(value,nullptr,false);
        return parsed.is_discarded()?nlohmann::json(nullptr):std::move(parsed);
    };
    nlohmann::json hybrid_profile=nullptr;
    if(hybrid_pixel_profile_)
        hybrid_profile=parse_observation(HybridPixelProfileCodec::write_canonical_json(*hybrid_pixel_profile_));
    const auto scripting_observation=parse_observation(active_world().scripting_observation_json());
    const auto input_observation=parse_observation(active_world().input_observation_json());
    const auto project_ui_observation=parse_observation(
        active_world().semantic_ui_project_document_json(options_.ui_locale));
    logger_.info("runtime.production_state",nlohmann::json{
        {"schemaVersion","noemancer.runtime-production-state/0.1"},
        {"mode",options_.player_mode?"packaged-player":
            options_.project_path.empty()?"engine-fixture":"source-project"},
        {"entityCount",active_world().entity_count()},
        {"hybridPixelProfile",std::move(hybrid_profile)},
        {"input",input_observation},
        {"scripting",scripting_observation},
        {"projectUi",project_ui_observation}}
        .dump());
    std::ostringstream message;
    message << "frames=" << frame_count << " entities=" << world_.entity_count();
    if(options_.player_mode) {
        logger_.info("player.animation_loading",nlohmann::json{
            {"schemaVersion","noemancer.player-animation-loading/0.1"},
            {"cookedArtifactLoads",cooked_animation_load_count_},
            {"sourceAssetDecodes",source_animation_decode_count_},
            {"offlineCompiles",offline_animation_compile_count_},
            {"sourceDecodeAtRuntime",source_animation_decode_count_!=0U},
            {"offlineCompileAtRuntime",offline_animation_compile_count_!=0U}}.dump());
        logger_.info("player.geometry_loading",nlohmann::json{
            {"schemaVersion","noemancer.player-geometry-loading/0.1"},
            {"cookedGeometryLoads",cooked_geometry_load_count_},
            {"sourceGeometryDecodes",source_geometry_decode_count_},
            {"offlineGeometryCompiles",offline_geometry_compile_count_},
            {"sourceDecodeAtRuntime",source_geometry_decode_count_!=0U},
            {"offlineCompileAtRuntime",offline_geometry_compile_count_!=0U}}.dump());
        logger_.info("player.scripting",world_.scripting_observation_json());
        logger_.info("player.scripting_abi",world_.scripting_abi_json());
    }
    logger_.info("runtime.stop", message.str());
    return 0;
}

int Application::run_interactive() {
    const bool performance_run=!options_.performance_evidence_path.empty();
    const bool runtime_surface_mode=options_.player_mode||options_.animation_physics_stress;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        logger_.error("sdl.init", SDL_GetError());
        return 10;
    }
    InputSourceAdapter input_sources;
    static_cast<void>(input_sources.connect_device({InputDeviceKind::keyboard,0,"keyboard.0","Keyboard",true}));
    static_cast<void>(input_sources.connect_device({InputDeviceKind::mouse,0,"mouse.0","Mouse",true}));
    const auto input_capture_observation = [&]() {
        ProjectSettingsInputMapCaptureObservation observation;
        observation.request_id = input_sources.capture_request_id();
        switch (input_sources.capture_state()) {
        case InputCaptureState::armed:
            observation.state = ProjectSettingsInputMapCaptureState::armed;
            break;
        case InputCaptureState::captured:
            observation.state = ProjectSettingsInputMapCaptureState::captured;
            break;
        case InputCaptureState::cancelled:
            observation.state = ProjectSettingsInputMapCaptureState::cancelled;
            break;
        case InputCaptureState::idle:
            observation.state = ProjectSettingsInputMapCaptureState::idle;
            break;
        }
        if (const auto& captured = input_sources.captured_input()) {
            observation.source = captured->source.id;
            observation.value = captured->value;
            switch (captured->source.device) {
            case InputDeviceKind::keyboard: observation.device = "keyboard"; break;
            case InputDeviceKind::mouse: observation.device = "mouse"; break;
            case InputDeviceKind::gamepad: observation.device = "gamepad"; break;
            }
        }
        return observation;
    };
    std::vector<SDL_Gamepad*> open_gamepads;
    const auto open_gamepad=[&](const SDL_JoystickID id) {
        if(std::ranges::any_of(open_gamepads,[&](SDL_Gamepad* pad){return SDL_GetGamepadID(pad)==id;}))return;
        if(auto* pad=SDL_OpenGamepad(id)) {
            open_gamepads.push_back(pad);
            const auto* name=SDL_GetGamepadName(pad);
            static_cast<void>(input_sources.connect_device({InputDeviceKind::gamepad,
                static_cast<std::uint64_t>(id),"gamepad."+std::to_string(id),name?name:"Gamepad",true}));
        }
        else logger_.error("input.gamepad.open",SDL_GetError());
    };
    int gamepad_count{};
    if(auto* gamepads=SDL_GetGamepads(&gamepad_count)) {
        for(int index=0;index<gamepad_count;++index)open_gamepad(gamepads[index]);
        SDL_free(gamepads);
    }

    const float display_scale = (performance_run||!options_.capture_frame_path.empty())
        ?1.0F:SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const auto window_flags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
        (performance_run?(SDL_WINDOW_UTILITY|SDL_WINDOW_NOT_FOCUSABLE):SDL_WINDOW_HIGH_PIXEL_DENSITY));
    SDL_Window* window = SDL_CreateWindow(
        options_.player_mode?(options_.player_display_name.empty()?"Noemancer Player":options_.player_display_name.c_str()):"Noemancer Editor",
        static_cast<int>(options_.window_width * display_scale),
        static_cast<int>(options_.window_height * display_scale),
        window_flags);
    if (window == nullptr) {
        logger_.error("sdl.window", SDL_GetError());
        SDL_Quit();
        return 11;
    }

    constexpr SDL_GPUShaderFormat shader_formats = static_cast<SDL_GPUShaderFormat>(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL);
    const char* requested_backend = options_.gpu_backend == "auto" ? nullptr : options_.gpu_backend.c_str();
    SDL_GPUDevice* device = SDL_CreateGPUDevice(shader_formats, options_.gpu_debug, requested_backend);
    if (device == nullptr) {
        logger_.error("sdl.gpu_device", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 12;
    }
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        logger_.error("sdl.gpu_window", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 13;
    }

    // The editor owns an embedded game viewport and must not tie simulation,
    // semantic projections, and tool input to a slow desktop presentation
    // interval. Prefer immediate presentation for the editor; player builds
    // keep conservative vsync until presentation becomes a project setting.
    const auto present_mode = (performance_run||!options_.player_mode) &&
            SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE)
        ? SDL_GPU_PRESENTMODE_IMMEDIATE
        : SDL_GPU_PRESENTMODE_VSYNC;
    if (!SDL_SetGPUSwapchainParameters(
            device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present_mode)) {
        logger_.error("render.swapchain_parameters", SDL_GetError());
    }
    logger_.info("render.present_mode",
        present_mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediate" : "vsync");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    io.IniFilename = nullptr;
#ifdef _WIN32
    // Use the native Windows UI face when present. The default embedded ImGui
    // bitmap font remains the deterministic fallback for minimal deployments.
    constexpr auto editor_font_path="C:\\Windows\\Fonts\\segoeui.ttf";
    if(std::filesystem::exists(editor_font_path))
        static_cast<void>(io.Fonts->AddFontFromFileTTF(editor_font_path,15.0F));
#endif

    if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
        logger_.error("editor.imgui_platform", "Failed to initialize the SDL3 platform backend");
        ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 14;
    }

    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = present_mode;
    if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
        logger_.error("editor.imgui_renderer", "Failed to initialize the SDL_GPU renderer backend");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 15;
    }

    TextureResourceTable texture_resources;
    auto scene_renderer = std::make_unique<SceneRenderer>(device, asset_registry_, texture_resources, options_.gpu_debug);
    if (!scene_renderer->set_hybrid_pixel_profile(hybrid_pixel_profile_)) {
        logger_.error("render.hybrid_pixel_profile", scene_renderer->last_error());
        scene_renderer.reset();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 16;
    }
    std::uint64_t applied_hybrid_pixel_profile_revision=hybrid_pixel_profile_revision_;
    scene_renderer->set_gpu_driven_enabled(!options_.disable_gpu_driven);
    scene_renderer->set_ambient_occlusion_enabled(!options_.disable_ambient_occlusion);
    scene_renderer->set_texture_streaming_budget_kib(options_.texture_streaming_budget_kib);
    scene_renderer->set_texture_streaming_resident_budget_kib(options_.texture_streaming_resident_budget_kib);
    scene_renderer->set_texture_streaming_workload(options_.texture_streaming_workload);
    if (!scene_renderer->set_shadow_quality(options_.shadow_quality)) {
        logger_.error("render.shadow_quality", scene_renderer->last_error());
        scene_renderer.reset();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 16;
    }
    if (!scene_renderer->initialize()) {
        logger_.error("render.scene_initialize", scene_renderer->last_error());
        scene_renderer.reset();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 16;
    }
    RetainedUiRuntime retained_ui;
    RetainedUiGpuAdapter retained_ui_gpu(device,texture_resources,"ui.game");
    RetainedUiGpuAdapter retained_inspector_gpu(device,texture_resources,"ui.editor.inspector");
    SDL_GPUTexture* retained_inspector_texture{};
    TextureResourceHandle retained_inspector_texture_handle{};
    std::uint32_t retained_inspector_width{384},retained_inspector_height{640};
    SDL_GPUTexture* editor_capture_texture{};
    std::uint32_t editor_capture_width{},editor_capture_height{};
    const auto editor_capture_format=SDL_GetGPUSwapchainTextureFormat(device,window);
    std::string ui_entity_id;
    for (const auto& entity : world_.entity_views()) {
        if (entity.id == "entity.demo-cube") { ui_entity_id = entity.id; break; }
        if (entity.character_motor_2d) { ui_entity_id = entity.id; break; }
        if (ui_entity_id.empty() && entity.transform && entity.mesh_renderer) ui_entity_id = entity.id;
    }
    if (ui_entity_id.empty()) {
        logger_.error("ui.no-focus-entity", "The project scene has no renderable entity for the initial Inspector/HUD focus.");
        retained_ui_gpu.shutdown(); scene_renderer.reset(); ImGui_ImplSDLGPU3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device,window); SDL_DestroyGPUDevice(device); SDL_DestroyWindow(window); SDL_Quit(); return 17;
    }
    if(options_.project_path.empty())
        static_cast<void>(world_.gameplay_ability_grant_json(ui_entity_id,"ability.combat.impact"));
    const auto project_hud_document=[&](const World& source) {
        if(source.has_project_hud())return source.semantic_ui_project_document_json(options_.ui_locale);
        if(options_.player_mode)return semantic_ui_game_hud_document(
            source.gameplay_ability_observation_json(ui_entity_id),ui_entity_id,options_.ui_locale);
        return nlohmann::json{{"schemaVersion","noemancer.ui-document/0.1"},{"valid",true},{"code","ok"},
            {"documentId","ui.game-hud.empty"},{"surface","game"},{"kind","hud"},{"revision",0},
            {"nodes",nlohmann::json::array()}}.dump();
    };
    const auto hud_document=project_hud_document(world_);
    const auto hud_markup=retained_ui_rml_from_semantic_document(hud_document);
    std::string hud_markup_cache=hud_markup;
    if(!retained_ui.initialize(scene_renderer->width(),scene_renderer->height())||
       !retained_ui.load_document("ui.game-hud",hud_markup)||
       !retained_ui_gpu.initialize(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)||
       (!options_.player_mode&&(!retained_ui.create_surface("editor.inspector",retained_inspector_width,retained_inspector_height)||
        !retained_ui.load_surface_document("editor.inspector","ui.editor-inspector",retained_ui_rml_from_semantic_document(
            editor_ui_.retained_inspector_document_json()))||
        !retained_inspector_gpu.initialize(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)))) {
        logger_.error("ui.retained_gpu_initialize",retained_ui.last_error().empty()?retained_ui_gpu.last_error():std::string(retained_ui.last_error()));
        retained_inspector_gpu.shutdown();retained_ui_gpu.shutdown(); scene_renderer.reset(); ImGui_ImplSDLGPU3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device,window); SDL_DestroyGPUDevice(device); SDL_DestroyWindow(window); SDL_Quit(); return 17;
    }
    const auto ensure_inspector_texture=[&](const std::uint32_t width,const std::uint32_t height) {
        if(options_.player_mode)return true;
        if(retained_inspector_texture&&retained_inspector_width==width&&retained_inspector_height==height)return true;
        retained_inspector_width=width;retained_inspector_height=height;
        SDL_GPUTextureCreateInfo info{};info.type=SDL_GPU_TEXTURETYPE_2D;info.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;info.width=width;info.height=height;
        info.layer_count_or_depth=1;info.num_levels=1;info.sample_count=SDL_GPU_SAMPLECOUNT_1;
        auto* next_texture=SDL_CreateGPUTexture(device,&info);
        if(!next_texture)return false;
        const TextureResourceMetadata metadata{width,height,1U,0U,static_cast<std::uint64_t>(width)*height*4U};
        if(retained_inspector_texture_handle.valid()) {
            if(!texture_resources.stage_replacement(retained_inspector_texture_handle,next_texture,metadata)) {
                SDL_ReleaseGPUTexture(device,next_texture);return false;
            }
            if(auto* previous=texture_resources.commit_replacement(retained_inspector_texture_handle))
                SDL_ReleaseGPUTexture(device,previous);
        } else {
            retained_inspector_texture_handle=texture_resources.acquire({.stable_id="ui.target.editor.inspector",
                .semantic="ui-render-target",.owner="ui.editor.inspector",.source="editor-dock-surface",
                .residency="resident",.metadata=metadata},next_texture);
            if(!retained_inspector_texture_handle.valid()) {
                SDL_ReleaseGPUTexture(device,next_texture);return false;
            }
        }
        retained_inspector_texture=texture_resources.resolve(retained_inspector_texture_handle);
        editor_ui_.set_retained_inspector_surface(reinterpret_cast<std::uintptr_t>(retained_inspector_texture),width,height);
        return true;
    };
    if(!ensure_inspector_texture(retained_inspector_width,retained_inspector_height)) {
        logger_.error("ui.inspector_texture",SDL_GetError());retained_inspector_gpu.shutdown();retained_ui_gpu.shutdown();
        scene_renderer.reset();ImGui_ImplSDLGPU3_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();
        SDL_ReleaseWindowFromGPUDevice(device,window);SDL_DestroyGPUDevice(device);SDL_DestroyWindow(window);SDL_Quit();return 17;
    }
    const auto ensure_editor_capture_texture=[&] {
        if(options_.capture_editor_frame_path.empty())return true;
        int width{},height{};SDL_GetWindowSizeInPixels(window,&width,&height);
        if(width<=0||height<=0)return false;
        const auto next_width=static_cast<std::uint32_t>(width),next_height=static_cast<std::uint32_t>(height);
        if(editor_capture_texture&&editor_capture_width==next_width&&editor_capture_height==next_height)return true;
        if(editor_capture_texture)SDL_ReleaseGPUTexture(device,editor_capture_texture);
        SDL_GPUTextureCreateInfo info{};info.type=SDL_GPU_TEXTURETYPE_2D;info.format=editor_capture_format;
        info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;info.width=next_width;info.height=next_height;
        info.layer_count_or_depth=1;info.num_levels=1;info.sample_count=SDL_GPU_SAMPLECOUNT_1;
        editor_capture_texture=SDL_CreateGPUTexture(device,&info);
        editor_capture_width=next_width;editor_capture_height=next_height;return editor_capture_texture!=nullptr;
    };
    AssetThumbnailGpuCache thumbnail_gpu_cache(device,texture_resources);
    constexpr std::uint32_t audio_sample_rate=48000;
    AudioOutputBackend audio_output;
    std::vector<AudioOutputSource> audio_sources;
    for(const auto& asset:asset_registry_.records()) {
        if(!asset.available||(asset.extension!=".wav"&&asset.extension!=".ogg"&&
            asset.extension!=".flac"&&asset.extension!=".mp3"))continue;
        const auto storage=asset.source_bytes>=1024U*1024U?AudioAssetStorage::stream:AudioAssetStorage::resident;
        audio_sources.push_back({asset.id,asset_registry_.source_path(asset),asset.content_hash,storage});
    }
    if(audio_output.initialize(audio_sample_rate,2,std::move(audio_sources)))logger_.info("audio.device",audio_output.status_json());
    else logger_.info("audio.device",std::string("degraded: ")+audio_output.last_error());
    const World* published_audio_world=nullptr;
    std::uint64_t published_audio_revision{};
    if (options_.exposure) editor_ui_.set_exposure(*options_.exposure);
    if (options_.render_scale) scene_renderer->set_render_scale(*options_.render_scale);
    scene_renderer->set_temporal_debug_mode(options_.temporal_debug_mode);
    if(!options_.reference_scene_id.empty())
        scene_renderer->set_capture_contract_json(commercial_raster_reference_contract_json());
    editor_ui_.set_render_surface(
        reinterpret_cast<std::uintptr_t>(scene_renderer->color_texture()),
        scene_renderer->width(),
        scene_renderer->height());
    editor_ui_.set_render_status(scene_renderer->status_json());
    auto last_render_status_publish=std::chrono::steady_clock::now();

    if (options_.capture_frame_path.empty() && options_.capture_editor_frame_path.empty() && !options_.probe_pixel) {
        SDL_SetWindowPosition(window,performance_run?0:SDL_WINDOWPOS_CENTERED,
            performance_run?0:SDL_WINDOWPOS_CENTERED);
        // External presentation telemetry still needs a compositor-visible
        // surface; acceptance-only CPU sampling can explicitly stay hidden.
        if(performance_run&&!options_.performance_hidden)static_cast<void>(SDL_SetWindowOpacity(window,0.01F));
        if(!performance_run||!options_.performance_hidden)SDL_ShowWindow(window);
    }
    logger_.info("runtime.start", SDL_GetGPUDeviceDriver(device));
    logger_.info("runtime.native_dependencies",native_runtime_dependencies_json());
    if(!options_.player_mode)logger_.info("editor.semantic_snapshot", editor_ui_.semantic_snapshot_json());
    logger_.info("ui.text_capabilities",retained_ui_text_capabilities_json(options_.ui_locale));

    bool running = true;
    bool retained_pointer_captured = false;
    bool retained_input_is_inspector = false;
    bool retained_text_input_active = false;
    std::uint64_t retained_keyboard_revision = 0;
    std::string retained_inspector_document_cache;
    const auto sync_retained_keyboard=[&] {
        const auto request=retained_ui.keyboard_request();
        if(request.revision==retained_keyboard_revision) return;
        retained_keyboard_revision=request.revision;
        if(request.active) {
            SDL_Rect area{request.caret_x,request.caret_y,1,std::max(1,request.line_height)};
            if(!options_.player_mode)if(const auto position=retained_input_is_inspector?
                editor_ui_.retained_inspector_window_at(request.caret_x,request.caret_y):
                editor_ui_.scene_window_at(request.caret_x,request.caret_y)) {
                area.x=position->x; area.y=position->y;
                area.h=std::max(1,static_cast<int>(std::lround(request.line_height*position->scale_y)));
            }
            static_cast<void>(SDL_SetTextInputArea(window,&area,0));
            static_cast<void>(SDL_StartTextInput(window));
            retained_text_input_active=true;
        } else if(retained_text_input_active) {
            static_cast<void>(SDL_StopTextInput(window));
            retained_text_input_active=false;
        }
    };
    std::uint32_t frame = 0;
    std::vector<double> performance_frame_milliseconds;
    std::vector<double> performance_swapchain_wait_milliseconds;
    std::vector<double> performance_submit_wait_milliseconds;
    std::vector<double> performance_preparation_milliseconds;
    std::vector<double> performance_event_processing_milliseconds;
    std::vector<double> performance_simulation_milliseconds;
    std::vector<double> performance_command_record_milliseconds;
    std::vector<double> performance_render_extract_milliseconds;
    std::vector<double> performance_scene_render_record_milliseconds;
    if(performance_run) {
        performance_frame_milliseconds.reserve(options_.performance_sample_frames);
        performance_swapchain_wait_milliseconds.reserve(options_.performance_sample_frames);
        performance_submit_wait_milliseconds.reserve(options_.performance_sample_frames);
        performance_preparation_milliseconds.reserve(options_.performance_sample_frames);
        performance_event_processing_milliseconds.reserve(options_.performance_sample_frames);
        performance_simulation_milliseconds.reserve(options_.performance_sample_frames);
        performance_command_record_milliseconds.reserve(options_.performance_sample_frames);
        performance_render_extract_milliseconds.reserve(options_.performance_sample_frames);
        performance_scene_render_record_milliseconds.reserve(options_.performance_sample_frames);
    }
    auto previous_frame_time = std::chrono::steady_clock::now();
    const auto has_gpu_probe = performance_run||!options_.capture_frame_path.empty()||!options_.capture_editor_frame_path.empty()||
        options_.probe_pixel.has_value()||options_.gpu_visibility_readback;
    const auto requested_frames = performance_run
        ? options_.performance_warmup_frames+options_.performance_sample_frames
        : (!has_gpu_probe || options_.frames != 0 ? options_.frames : 3U);
    for (const auto& sample : options_.input_samples) {
        static_cast<void>(active_world().inject_input_json(sample.source, sample.value));
    }
    std::ranges::stable_sort(options_.input_events, {}, &RuntimeInputEvent::frame);
    std::size_t input_event_index{};
    while (running && (requested_frames == 0 || frame < requested_frames)) {
        const auto performance_frame_start=std::chrono::steady_clock::now();
        double performance_swapchain_wait{};
        double performance_submit_wait{};
        double performance_preparation{};
        double performance_event_processing{};
        double performance_simulation{};
        double performance_command_record{};
        double performance_render_extract{};
        double performance_scene_render_record{};
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                static_cast<void>(input_sources.ingest({InputPhysicalKind::mouse_axis,
                    static_cast<std::uint32_t>(MouseAxis::x),event.motion.xrel,InputEventPhase::changed}));
                static_cast<void>(input_sources.ingest({InputPhysicalKind::mouse_axis,
                    static_cast<std::uint32_t>(MouseAxis::y),event.motion.yrel,InputEventPhase::changed}));
                const auto inspector_pointer=options_.player_mode?std::optional<ScenePointerPosition>{}:
                    editor_ui_.retained_inspector_pointer_at(event.motion.x,event.motion.y);
                std::optional<ScenePointerPosition> pointer;
                if(options_.player_mode) {
                    int width{},height{};SDL_GetWindowSize(window,&width,&height);
                    if(width>0&&height>0)pointer=ScenePointerPosition{static_cast<std::int32_t>(event.motion.x*scene_renderer->width()/width),
                        static_cast<std::int32_t>(event.motion.y*scene_renderer->height()/height)};
                } else pointer=editor_ui_.scene_pointer_at(event.motion.x,event.motion.y);
                if(inspector_pointer) {
                    static_cast<void>(retained_ui.pointer_leave());
                    retained_pointer_captured=!retained_ui.surface_pointer_move("editor.inspector",inspector_pointer->x,inspector_pointer->y);
                    retained_input_is_inspector=true;
                } else if (pointer) {
                    static_cast<void>(retained_ui.surface_pointer_leave("editor.inspector"));
                    retained_pointer_captured = !retained_ui.pointer_move(pointer->x, pointer->y);
                    retained_input_is_inspector=false;
                } else {
                    static_cast<void>(retained_ui.pointer_leave());
                    if(!options_.player_mode)static_cast<void>(retained_ui.surface_pointer_leave("editor.inspector"));
                    retained_pointer_captured = false;
                }
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                const auto input_phase=event.type==SDL_EVENT_MOUSE_BUTTON_DOWN?InputEventPhase::pressed:InputEventPhase::released;
                InputSample input_sample{InputPhysicalKind::mouse_button,event.button.button,
                    event.type==SDL_EVENT_MOUSE_BUTTON_DOWN?1.0F:0.0F,input_phase};
                static_cast<void>(input_sources.ingest(input_sample));
                if(options_.player_mode)if(const auto source=normalize_input(input_sample))
                    static_cast<void>(active_world().inject_input_json(source->id,input_sample.value));
                const auto inspector_pointer=options_.player_mode?std::optional<ScenePointerPosition>{}:
                    editor_ui_.retained_inspector_pointer_at(event.button.x,event.button.y);
                std::optional<ScenePointerPosition> pointer;
                if(options_.player_mode) {
                    int width{},height{};SDL_GetWindowSize(window,&width,&height);
                    if(width>0&&height>0)pointer=ScenePointerPosition{static_cast<std::int32_t>(event.button.x*scene_renderer->width()/width),
                        static_cast<std::int32_t>(event.button.y*scene_renderer->height()/height)};
                } else pointer=editor_ui_.scene_pointer_at(event.button.x,event.button.y);
                if (inspector_pointer||pointer) {
                    std::uint32_t button = 3;
                    if (event.button.button == SDL_BUTTON_LEFT) button = 0;
                    else if (event.button.button == SDL_BUTTON_RIGHT) button = 1;
                    else if (event.button.button == SDL_BUTTON_MIDDLE) button = 2;
                    if(inspector_pointer) {
                        static_cast<void>(retained_ui.surface_pointer_move("editor.inspector",inspector_pointer->x,inspector_pointer->y));
                        if(button<=2)retained_pointer_captured=!retained_ui.surface_pointer_button(
                            "editor.inspector",button,event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
                        retained_input_is_inspector=true;
                    } else {
                        static_cast<void>(retained_ui.pointer_move(pointer->x,pointer->y));
                        if(button<=2)retained_pointer_captured=!retained_ui.pointer_button(button,event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
                        retained_input_is_inspector=false;
                    }
                }
                sync_retained_keyboard();
            }
            if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                const auto retained_key=retained_key_from_sdl(event.key.key);
                const auto retained_propagating=retained_input_is_inspector?
                    retained_ui.surface_key("editor.inspector",retained_key,event.type==SDL_EVENT_KEY_DOWN,
                        retained_modifiers_from_sdl(event.key.mod)):
                    retained_ui.key(retained_key,event.type==SDL_EVENT_KEY_DOWN,retained_modifiers_from_sdl(event.key.mod));
                InputSample input_sample{InputPhysicalKind::keyboard_scancode,
                    static_cast<std::uint32_t>(event.key.scancode),event.type==SDL_EVENT_KEY_DOWN?1.0F:0.0F,
                    event.type==SDL_EVENT_KEY_DOWN?InputEventPhase::pressed:InputEventPhase::released,
                    0,0,event.key.repeat};
                static_cast<void>(input_sources.ingest(input_sample));
                const auto normalized=normalize_input(input_sample);
                const auto source=normalized?std::optional<std::string>{normalized->id}:std::nullopt;
                if(source&&retained_propagating&&!retained_ui.keyboard_request().active)
                    static_cast<void>(active_world().inject_input_json(*source,event.type==SDL_EVENT_KEY_DOWN?1.0F:0.0F));
                sync_retained_keyboard();
            }
            if(event.type==SDL_EVENT_GAMEPAD_ADDED)open_gamepad(event.gdevice.which);
            if(event.type==SDL_EVENT_GAMEPAD_REMOVED) {
                static_cast<void>(input_sources.disconnect_device(InputDeviceKind::gamepad,
                    static_cast<std::uint64_t>(event.gdevice.which)));
                std::erase_if(open_gamepads,[&](SDL_Gamepad* pad) {
                    if(SDL_GetGamepadID(pad)!=event.gdevice.which)return false;SDL_CloseGamepad(pad);return true;
                });
            }
            if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN||event.type==SDL_EVENT_GAMEPAD_BUTTON_UP) {
                InputSample sample{InputPhysicalKind::gamepad_button,event.gbutton.button,
                    event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN?1.0F:0.0F,
                    event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN?InputEventPhase::pressed:InputEventPhase::released,
                    0,static_cast<std::uint64_t>(event.gbutton.which)};
                static_cast<void>(input_sources.ingest(sample));
                if(const auto source=normalize_input(sample))
                    static_cast<void>(active_world().inject_input_json(source->id,sample.value));
            }
            if(event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                InputSample sample{InputPhysicalKind::gamepad_axis,event.gaxis.axis,
                    normalized_gamepad_axis(event.gaxis.value),InputEventPhase::changed,0,
                    static_cast<std::uint64_t>(event.gaxis.which)};
                static_cast<void>(input_sources.ingest(sample));
                if(const auto source=normalize_input(sample))
                    static_cast<void>(active_world().inject_input_json(source->id,sample.value));
            }
            if(event.type==SDL_EVENT_TEXT_INPUT) {
                if(retained_input_is_inspector)static_cast<void>(retained_ui.surface_text_input("editor.inspector",event.text.text));
                else static_cast<void>(retained_ui.text_input(event.text.text));
                sync_retained_keyboard();
            }
            if(event.type==SDL_EVENT_TEXT_EDITING) {
                if(retained_input_is_inspector)static_cast<void>(retained_ui.surface_text_composition(
                    "editor.inspector",event.edit.text,event.edit.start,event.edit.length));
                else static_cast<void>(retained_ui.text_composition(event.edit.text,event.edit.start,event.edit.length));
                sync_retained_keyboard();
            }
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(window))) {
                if(options_.player_mode)running=false;else editor_ui_.request_close();
            }
        }
        performance_event_processing=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-performance_frame_start).count();

        while (input_event_index < options_.input_events.size() &&
               options_.input_events[input_event_index].frame == frame) {
            const auto& input_event = options_.input_events[input_event_index++];
            static_cast<void>(active_world().inject_input_json(input_event.source, input_event.value));
        }

        for(const auto& action:retained_ui.consume_action_events()) {
            if(action.kind!=RetainedUiActionKind::value_changed||action.action_id!="world.property.plan")continue;
            if(!options_.player_mode&&editor_ui_.simulation_state()!=EditorSimulationState::edit) {
                logger_.error("ui.retained_action","World property editing is disabled outside Edit World.");
                continue;
            }
            const auto binding=nlohmann::json::parse(action.binding_json,nullptr,false);
            if(!binding.is_object()||binding.value("kind",std::string{})!="world-property") {
                logger_.error("ui.retained_action",std::string("Invalid world-property binding for ")+action.node_id);
                continue;
            }
            auto& action_world=active_world();
            const auto plan=action_world.plan_property_update(binding.value("entityId",std::string{}),
                binding.value("property",std::string{}),action.value_json,binding.value("revision",0ULL),"ui.retained");
            const auto receipt=action_world.apply_property_plan(plan,false);
            const auto receipt_json=World::action_receipt_json(receipt);
            if(receipt.success) {
                editor_ui_.refresh_world_model();
                editor_ui_.set_last_action_status(receipt.detail);
                logger_.info("ui.retained_action",receipt_json);
            } else logger_.error("ui.retained_action",receipt_json);
        }

        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0U) {
            SDL_Delay(10);
            continue;
        }

        const auto frame_time = std::chrono::steady_clock::now();
        const std::chrono::duration<double> frame_delta = frame_time - previous_frame_time;
        previous_frame_time = frame_time;
        const auto simulation_start=std::chrono::steady_clock::now();
        if (options_.vfx_respawn_interval > 0 && frame > 0 && frame % options_.vfx_respawn_interval == 0) {
            static_cast<void>(active_world().vfx_spawn_json("vfx.debug-impact",{2.5F,2.2F,1.0F},0x4e4f454dULL+frame));
        }
        // Automated visual evidence advances deterministic simulation time instead of wall-clock time.
        tick_frame(has_gpu_probe ? 1.0 / 60.0 : frame_delta.count());
        performance_simulation=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-simulation_start).count();
        if(frame>0&&frame%6U==0U) {
            const auto current_hud_document=project_hud_document(active_world());
            const auto current_hud_markup=retained_ui_rml_from_semantic_document(current_hud_document);
            if(current_hud_markup!=hud_markup_cache) {
                if(!retained_ui.reload_document("ui.game-hud",current_hud_markup)) {
                    logger_.error("ui.resource_reload",retained_ui.last_error()); break;
                }
                hud_markup_cache=current_hud_markup;
                logger_.info("ui.hud_reload",current_hud_document);
            }
        }
        auto& audio_world=active_world();
        if(&audio_world!=published_audio_world||audio_world.audio_revision()!=published_audio_revision) {
            published_audio_world=&audio_world;published_audio_revision=audio_world.audio_revision();
            audio_output.publish(audio_world.audio_render_snapshot());
        }
        if(applied_hybrid_pixel_profile_revision!=hybrid_pixel_profile_revision_) {
            if(!scene_renderer->set_hybrid_pixel_profile(hybrid_pixel_profile_)) {
                logger_.error("render.hybrid_pixel_profile",scene_renderer->last_error());break;
            }
            applied_hybrid_pixel_profile_revision=hybrid_pixel_profile_revision_;
        }
        std::uint32_t requested_width=editor_ui_.requested_scene_width(),requested_height=editor_ui_.requested_scene_height();
        if(runtime_surface_mode||!options_.capture_frame_path.empty()||!options_.reference_scene_id.empty()) {int width{},height{};SDL_GetWindowSizeInPixels(window,&width,&height);
            requested_width=static_cast<std::uint32_t>(std::max(1,width));requested_height=static_cast<std::uint32_t>(std::max(1,height));}
        if (!scene_renderer->resize(requested_width,requested_height)) {
            logger_.error("render.scene_resize", scene_renderer->last_error());
            break;
        }
        if(!retained_ui.resize(scene_renderer->width(),scene_renderer->height(),1.0F)||!retained_ui.update()||!retained_ui.render()) {
            logger_.error("ui.retained_update",retained_ui.last_error()); break;
        }
        if(!runtime_surface_mode) {
            const auto inspector_width=editor_ui_.requested_inspector_width();
            const auto inspector_height=editor_ui_.requested_inspector_height();
            if(!ensure_inspector_texture(inspector_width,inspector_height)||
               !retained_ui.resize_surface("editor.inspector",inspector_width,inspector_height,1.0F)) {
                logger_.error("ui.inspector_surface",retained_ui.last_error().empty()?SDL_GetError():std::string(retained_ui.last_error()));break;
            }
            auto inspector_document=nlohmann::json::parse(editor_ui_.retained_inspector_document_json(),nullptr,false);
            if(inspector_document.is_object()) {
                inspector_document["designTokens"]["surfaceWidthPx"]=inspector_width;
                inspector_document["designTokens"]["surfaceColor"]="#0b111b";
                inspector_document["designTokens"]["groupColor"]="#111b29";
                inspector_document["designTokens"]["textColor"]="#dce5f1";
                inspector_document["designTokens"]["accentColor"]="#77b7ff";
                inspector_document["designTokens"]["embeddedSurface"]=true;
                const auto document_source=inspector_document.dump();
                if(document_source!=retained_inspector_document_cache) {
                    if(!retained_ui.reload_surface_document("editor.inspector","ui.editor-inspector",
                        retained_ui_rml_from_semantic_document(document_source))) {
                        logger_.error("ui.inspector_reload",retained_ui.last_error());break;
                    }
                    retained_inspector_document_cache=document_source;
                }
            }
            if(!retained_ui.render_surface("editor.inspector")) {
                logger_.error("ui.inspector_render",retained_ui.last_error());break;
            }
        }
        sync_retained_keyboard();
        scene_renderer->set_exposure(editor_ui_.requested_exposure());
        if(!runtime_surface_mode)editor_ui_.set_render_surface(
            reinterpret_cast<std::uintptr_t>(scene_renderer->color_texture()),
            scene_renderer->width(),
            scene_renderer->height());
        // Renderer status is a rich semantic observation (render graph,
        // resources and evidence), not frame data. Publishing it at a bounded
        // cadence keeps Agent visibility without serializing a large document
        // on the render hot path.
        if(!runtime_surface_mode&&frame_time-last_render_status_publish>=std::chrono::milliseconds(250)) {
            editor_ui_.set_render_status(scene_renderer->status_json());
            last_render_status_publish=frame_time;
        }
        if(play_world_) {
            const auto selected=editor_ui_.play_world_selected_entity_id();
            const auto inspector=selected.empty()?std::string{"{}"}:play_world_->inspector_document_json(selected);
            const auto apply_plan=plan_play_world_apply(play_base_scene_json_,play_world_->runtime_authoring_scene_json(),play_base_revision_);
            editor_ui_.set_play_world_context(play_world_->observe_json(ObservationQuery{
                .fields={"identity","hierarchy"},.depth=0,.byte_budget=128U*1024U,.cursor=0}),
                inspector,apply_plan.to_json());
        }
        performance_preparation=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-performance_frame_start).count();
        const auto command_record_start=std::chrono::steady_clock::now();
        SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (command_buffer == nullptr) {
            logger_.error("render.command_buffer", SDL_GetError());
            break;
        }
        if(!ensure_editor_capture_texture()) {
            logger_.error("editor.capture_texture",SDL_GetError());scene_renderer->rollback_texture_streaming_frame();SDL_CancelGPUCommandBuffer(command_buffer);break;
        }
        std::vector<AssetThumbnailGpuCache::Request> thumbnail_requests;
        if(!runtime_surface_mode)for(const auto& artifact:editor_ui_.asset_thumbnail_artifacts())
            if(const auto path=resolve_thumbnail_artifact(asset_registry_,artifact.uri))
                thumbnail_requests.push_back({artifact.asset_id,*path});
        if(!thumbnail_requests.empty()) {
            const auto sync=thumbnail_gpu_cache.sync(command_buffer,thumbnail_requests);
            for(const auto& request:thumbnail_requests)editor_ui_.set_asset_thumbnail_texture(request.asset_id,
                reinterpret_cast<std::uintptr_t>(thumbnail_gpu_cache.texture_for(request.asset_id)));
            if(sync.uploaded_count>0U)logger_.info("editor.thumbnail_gpu",thumbnail_gpu_cache.status_json());
            if(!sync.success&&sync.failed_count>0U)logger_.error("editor.thumbnail_gpu",thumbnail_gpu_cache.status_json());
        }
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if(runtime_surface_mode) {
            const auto& player_io=ImGui::GetIO();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(player_io.DisplaySize);
            constexpr auto flags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|
                ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});ImGui::Begin("##NoemancerPlayer",nullptr,flags);
            ImGui::Image(scene_renderer->color_texture(),ImGui::GetContentRegionAvail());ImGui::End();ImGui::PopStyleVar();
        } else {
            auto debug_events=world_.scripting_debug_session_events_json();
            if(managed_debug_external_player_&&(debug_events.find(R"("event":"exited")")!=std::string::npos||
                debug_events.find(R"("event":"terminated")")!=std::string::npos)) {
                static_cast<void>(world_.scripting_debug_session_stop_json(2000U));
                if(managed_debug_player_)managed_debug_player_->terminate();
                managed_debug_external_player_=false;
            }
            editor_ui_.set_managed_debug_context(std::move(debug_events),managed_debug_last_action_json_);
            editor_ui_.set_input_status(input_sources.observe_json());
            editor_ui_.set_project_input_capture(input_capture_observation());
            poll_package_job();editor_ui_.render();
            if (const auto request = editor_ui_.consume_project_input_request()) {
                if (request->kind == ProjectSettingsInputMapPanelRequestKind::begin_capture) {
                    if (!input_sources.begin_capture(request->capture_request_id)) {
                        editor_ui_.set_last_action_status("Input capture could not be armed.");
                        logger_.error("project.input-map.capture", "Input capture could not be armed.");
                    }
                } else if (request->kind == ProjectSettingsInputMapPanelRequestKind::cancel_capture) {
                    if (!input_sources.cancel_capture(request->capture_request_id)) {
                        editor_ui_.set_last_action_status("Input capture cancellation was stale or invalid.");
                        logger_.error("project.input-map.capture", "Input capture cancellation was stale or invalid.");
                    }
                } else {
                    apply_project_input_map_request(*request);
                }
            }
            if(const auto request=editor_ui_.consume_hybrid_pixel_profile_request())
                apply_hybrid_pixel_profile_request(*request);
            if (const auto command=editor_ui_.consume_simulation_command()) apply_simulation_command(*command);
            if (const auto request=editor_ui_.consume_managed_debug_request()) apply_managed_debug_request(*request);
            if (const auto request=editor_ui_.consume_package_request()) apply_package_request(*request);
            if (const auto request=editor_ui_.consume_project_request()) apply_project_request(*request);
            if(const auto request=editor_ui_.consume_source_open_request())apply_source_open_request(*request);
            if(const auto completion=editor_ui_.consume_script_build_completion())apply_script_build_completion(*completion);
            if(editor_ui_.consume_exit_request())running=false;
        }
        auto scene_pick_request = runtime_surface_mode?std::optional<ScenePickRequest>{}:editor_ui_.consume_scene_pick_request();
        if (retained_pointer_captured) scene_pick_request.reset();
        if (options_.probe_pixel && frame + 1 == requested_frames) scene_pick_request = options_.probe_pixel;
        ImGui::Render();

        ImDrawData* draw_data = ImGui::GetDrawData();
        SDL_GPUTexture* swapchain_texture = nullptr;
        const auto swapchain_wait_start=std::chrono::steady_clock::now();
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(
                command_buffer,
                window,
                &swapchain_texture,
                nullptr,
                nullptr)) {
            logger_.error("render.swapchain", SDL_GetError());
            scene_renderer->rollback_texture_streaming_frame();
            thumbnail_gpu_cache.rollback_uploads();retained_ui_gpu.rollback_uploads();retained_inspector_gpu.rollback_uploads();
            SDL_CancelGPUCommandBuffer(command_buffer);
            break;
        }
        performance_swapchain_wait=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-swapchain_wait_start).count();

        const bool capture_editor_this_frame=!options_.capture_editor_frame_path.empty()&&frame+1==requested_frames;
        const bool drawable_ui=(swapchain_texture != nullptr||capture_editor_this_frame) &&
            draw_data->DisplaySize.x > 0.0F && draw_data->DisplaySize.y > 0.0F;
        const bool visibility_readback_this_frame=options_.gpu_visibility_readback&&frame+1==requested_frames;
        if (has_gpu_probe || drawable_ui) {
            auto& render_source=active_world();
            const bool hybrid_pixel_scene_camera=hybrid_pixel_profile_&&hybrid_pixel_profile_->enabled;
            const auto editor_camera=(runtime_surface_mode||!options_.capture_frame_path.empty()||
                !options_.reference_scene_id.empty()||hybrid_pixel_scene_camera)
                ?std::optional<RenderCameraSnapshot>{}:editor_ui_.render_camera_override();
            const auto render_view_width = hybrid_pixel_scene_camera
                ? hybrid_pixel_profile_->virtual_width : scene_renderer->width();
            const auto render_view_height = hybrid_pixel_scene_camera
                ? hybrid_pixel_profile_->virtual_height : scene_renderer->height();
            TilemapViewQuery tilemap_query{render_view_width,render_view_height};
            if(editor_camera)tilemap_query.camera_override=TilemapVisibilityCamera{editor_camera->position,editor_camera->target,
                editor_camera->vertical_fov_degrees,editor_camera->near_clip,editor_camera->far_clip,
                editor_camera->projection,editor_camera->orthographic_height};
            const auto render_extract_start=std::chrono::steady_clock::now();
            auto render_world = RenderWorldExtractor::extract(
                  render_source.revision(), engine_host_.frame_index(), render_source.entity_views(tilemap_query), render_source.vfx_particles(),&tilemap_render_bake_cache_);
            if(editor_camera) render_world.camera=*editor_camera;
            HybridPixelRenderProjection hybrid_projection;
            if(hybrid_pixel_profile_)
                hybrid_projection=apply_hybrid_pixel_render_projection(render_world,*hybrid_pixel_profile_);
            scene_renderer->set_hybrid_pixel_projection(std::move(hybrid_projection));
            RenderWorldExtractor::cull_tilemap_chunks(
                render_world,render_view_width,render_view_height);
            // A capture is also an evidence boundary. Publish the exact final
            // Render World consumed by the renderer so CLI/Agent acceptance
            // can correlate semantic inputs with pixels without mirroring
            // World state or dumping it every interactive frame.
            if(!options_.capture_frame_path.empty()&&frame+1==requested_frames)
                logger_.info("render.world.final",render_world_json(render_world));
            performance_render_extract=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-render_extract_start).count();
            const auto scene_render_record_start=std::chrono::steady_clock::now();
            scene_renderer->render(command_buffer, render_world);
            if(visibility_readback_this_frame&&!scene_renderer->enqueue_gpu_visibility_readback(command_buffer))
                logger_.error("render.gpu_visibility_readback_enqueue",scene_renderer->last_error());
            performance_scene_render_record=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-scene_render_record_start).count();
            if(!scene_renderer->last_error().empty())
                logger_.error("render.scene_record",scene_renderer->last_error());
            if(!retained_ui_gpu.upload(command_buffer,retained_ui.render_packet())) {
                logger_.error("ui.retained_upload",retained_ui_gpu.last_error());
                if(SDL_SubmitGPUCommandBuffer(command_buffer)) {
                    scene_renderer->commit_texture_streaming_frame();thumbnail_gpu_cache.commit_uploads();retained_ui_gpu.commit_uploads();
                    retained_inspector_gpu.commit_uploads();
                } else {
                    logger_.error("render.submit_after_ui_failure",SDL_GetError());scene_renderer->rollback_texture_streaming_frame();
                    thumbnail_gpu_cache.rollback_uploads();retained_ui_gpu.rollback_uploads();retained_inspector_gpu.rollback_uploads();
                }
                running=false;break;
            }
            retained_ui_gpu.render(command_buffer,scene_renderer->color_texture(),scene_renderer->width(),scene_renderer->height());
            if(!runtime_surface_mode&&retained_inspector_texture) {
                if(!retained_inspector_gpu.upload(command_buffer,retained_ui.surface_render_packet("editor.inspector"))) {
                    logger_.error("ui.inspector_upload",retained_inspector_gpu.last_error());
                    if(SDL_SubmitGPUCommandBuffer(command_buffer)) {
                        scene_renderer->commit_texture_streaming_frame();thumbnail_gpu_cache.commit_uploads();retained_ui_gpu.commit_uploads();
                        retained_inspector_gpu.commit_uploads();
                    } else {
                        logger_.error("render.submit_after_inspector_failure",SDL_GetError());scene_renderer->rollback_texture_streaming_frame();
                        thumbnail_gpu_cache.rollback_uploads();retained_ui_gpu.rollback_uploads();retained_inspector_gpu.rollback_uploads();
                    }
                    running=false;break;
                }
                retained_inspector_gpu.render(command_buffer,retained_inspector_texture,
                    retained_inspector_width,retained_inspector_height,true);
            }
            if (frame == 0) {
                logger_.info("ui.retained_gpu",retained_ui_gpu.status_json());
                if(!runtime_surface_mode)logger_.info("ui.retained_inspector_gpu",retained_inspector_gpu.status_json());
            }
            const bool capture_this_frame = !options_.capture_frame_path.empty() && frame + 1 == requested_frames;
            if (capture_this_frame && !scene_renderer->enqueue_color_capture(command_buffer)) {
                logger_.error("render.capture_enqueue", scene_renderer->last_error());
            }
            if (scene_pick_request && !scene_renderer->enqueue_pick(
                    command_buffer, scene_pick_request->x, scene_pick_request->y)) {
                if (!scene_renderer->last_error().empty())
                    logger_.error("render.pick_enqueue", scene_renderer->last_error());
                // A click in a Hybrid Pixel letterbox is a normal no-op.  A
                // real enqueue failure is also no longer allowed to acquire
                // or attach a fence for a transfer that was never recorded.
                scene_pick_request.reset();
            }
        }
        if (drawable_ui) {
            ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
            SDL_GPUColorTargetInfo color_target{};
            color_target.texture = capture_editor_this_frame?editor_capture_texture:swapchain_texture;
            color_target.clear_color = SDL_FColor{0.025F, 0.032F, 0.045F, 1.0F};
            color_target.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(
                command_buffer,
                &color_target,
                1,
                nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, pass);
            SDL_EndGPURenderPass(pass);
            if(capture_editor_this_frame&&!scene_renderer->enqueue_texture_capture(command_buffer,editor_capture_texture,
                editor_capture_width,editor_capture_height,editor_capture_format,
                std::string_view(SDL_GetGPUDeviceDriver(device))=="direct3d12"))
                logger_.error("editor.capture_enqueue",scene_renderer->last_error());
        }
        const auto submit_wait_start=std::chrono::steady_clock::now();
        performance_command_record=std::chrono::duration<double,std::milli>(
            submit_wait_start-command_record_start).count();
        if (scene_pick_request||visibility_readback_this_frame) {
            auto* readback_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
            if (!readback_fence) {
                logger_.error(scene_pick_request?"render.pick_submit":"render.gpu_visibility_readback_submit", SDL_GetError());
                scene_renderer->rollback_texture_streaming_frame();
                thumbnail_gpu_cache.rollback_uploads();retained_ui_gpu.rollback_uploads();retained_inspector_gpu.rollback_uploads();
                break;
            }
            scene_renderer->commit_texture_streaming_frame();
            thumbnail_gpu_cache.commit_uploads();retained_ui_gpu.commit_uploads();retained_inspector_gpu.commit_uploads();
            if(scene_pick_request)scene_renderer->attach_pick_fence(readback_fence);
            else scene_renderer->attach_gpu_visibility_readback_fence(readback_fence);
        } else if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
            logger_.error("render.submit", SDL_GetError());
            scene_renderer->rollback_texture_streaming_frame();
            thumbnail_gpu_cache.rollback_uploads();retained_ui_gpu.rollback_uploads();retained_inspector_gpu.rollback_uploads();
            break;
        } else {
            scene_renderer->commit_texture_streaming_frame();
            thumbnail_gpu_cache.commit_uploads();retained_ui_gpu.commit_uploads();retained_inspector_gpu.commit_uploads();
        }
        if(frame==0)logger_.info("render.scene",scene_renderer->status_json());
        performance_submit_wait=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-submit_wait_start).count();
        if (scene_pick_request) {
            const auto picked_entity = scene_renderer->resolve_pick();
            logger_.info("render.pixel_evidence", scene_renderer->last_pixel_evidence_json());
            if (!picked_entity.empty() && editor_ui_.select_entity(picked_entity)) {
                logger_.info("render.pick", picked_entity);
            }
        }
        if (!options_.capture_frame_path.empty() && frame + 1 == requested_frames) {
            if (scene_renderer->save_color_capture(options_.capture_frame_path)) {
                logger_.info("render.capture", options_.capture_frame_path);
            } else {
                logger_.error("render.capture_save", scene_renderer->last_error());
            }
        }
        if(capture_editor_this_frame) {
            if(scene_renderer->save_color_capture(options_.capture_editor_frame_path))
                logger_.info("editor.capture",options_.capture_editor_frame_path);
            else logger_.error("editor.capture_save",scene_renderer->last_error());
        }
        if(performance_run&&frame>=options_.performance_warmup_frames) {
            performance_frame_milliseconds.push_back(std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-performance_frame_start).count());
            performance_swapchain_wait_milliseconds.push_back(performance_swapchain_wait);
            performance_submit_wait_milliseconds.push_back(performance_submit_wait);
            performance_preparation_milliseconds.push_back(performance_preparation);
            performance_event_processing_milliseconds.push_back(performance_event_processing);
            performance_simulation_milliseconds.push_back(performance_simulation);
            performance_command_record_milliseconds.push_back(performance_command_record);
            performance_render_extract_milliseconds.push_back(performance_render_extract);
            performance_scene_render_record_milliseconds.push_back(performance_scene_render_record);
        }
        ++frame;
    }

    bool gpu_visibility_readback_passed=true;
    if(options_.gpu_visibility_readback) {
        gpu_visibility_readback_passed=scene_renderer->resolve_gpu_visibility_readback()&&
            scene_renderer->gpu_visibility_readback_passed();
        if(gpu_visibility_readback_passed)logger_.info("render.gpu_visibility_readback",scene_renderer->status_json());
        else logger_.error("render.gpu_visibility_readback",scene_renderer->status_json());
    }
    SDL_WaitForGPUIdle(device);
    if (has_gpu_probe) logger_.info("render.scene.final", scene_renderer->status_json());
    bool performance_evidence_written=true;
    if(performance_run) {
        const auto profile=options_.player_mode?load_player_profile_document(options_.player_profile_path):nlohmann::json::object();
        std::string evidence_error;
        const PerformanceEvidenceInput evidence{
            .workload_id=options_.performance_workload_id,
            .project_id=profile.value("projectId",std::string{"editor.bootstrap"}),
            .target_profile=profile.value("targetProfile",std::string{"windows-x64-release"}),
            .backend=SDL_GetGPUDeviceDriver(device),
            .present_mode=present_mode==SDL_GPU_PRESENTMODE_IMMEDIATE?"immediate":"vsync",
            .width=scene_renderer->width(),.height=scene_renderer->height(),
            .warmup_frames=options_.performance_warmup_frames,
            .sampled_frame_milliseconds=performance_frame_milliseconds,
            .sampled_swapchain_wait_milliseconds=performance_swapchain_wait_milliseconds,
            .sampled_submit_wait_milliseconds=performance_submit_wait_milliseconds,
            .sampled_preparation_milliseconds=performance_preparation_milliseconds,
            .sampled_event_processing_milliseconds=performance_event_processing_milliseconds,
            .sampled_simulation_milliseconds=performance_simulation_milliseconds,
            .sampled_command_record_milliseconds=performance_command_record_milliseconds,
            .sampled_render_extract_milliseconds=performance_render_extract_milliseconds,
            .sampled_scene_render_record_milliseconds=performance_scene_render_record_milliseconds,
            .renderer_status_json=scene_renderer->status_json()};
        if(write_performance_evidence(options_.performance_evidence_path,evidence,evidence_error))
            logger_.info("performance.evidence",options_.performance_evidence_path);
        else { logger_.error("performance.evidence",evidence_error);performance_evidence_written=false; }
    }
    logger_.info("audio.output.final",audio_output.status_json());audio_output.shutdown();
    thumbnail_gpu_cache.shutdown();
    if(editor_capture_texture)SDL_ReleaseGPUTexture(device,editor_capture_texture);
    if(retained_inspector_texture_handle.valid())
        if(auto* texture=texture_resources.remove(retained_inspector_texture_handle))SDL_ReleaseGPUTexture(device,texture);
    retained_inspector_gpu.shutdown();
    retained_ui_gpu.shutdown();
    scene_renderer.reset();
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    for(auto* gamepad:open_gamepads)SDL_CloseGamepad(gamepad);
    SDL_Quit();
    logger_.info("runtime.stop", "interactive");
    if(!performance_evidence_written)return 24;
    return gpu_visibility_readback_passed?0:25;
}

} // namespace noemancer
