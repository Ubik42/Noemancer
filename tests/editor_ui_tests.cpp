#include "editor/editor_ui.hpp"
#include "engine/process_diagnostics.hpp"

#include <imgui.h>

#include <iostream>
#include <chrono>
#include <cstdlib>
#include <process.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef min
#undef max
#endif

int main() {
    noemancer::configure_process_diagnostics("test.editor-ui");
    noemancer::World world;
    if (!world.load_scene(noemancer::make_bootstrap_scene_document()).success) {
        std::cerr << "Bootstrap scene did not load for the editor UI\n";
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1440.0F, 900.0F);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    noemancer::AssetRegistry assets;
    const auto fixture_root=std::filesystem::path(__FILE__).parent_path()/"fixtures/managed-project";
    const auto script_root=std::filesystem::absolute("generated/editor-ui-managed-project");
    std::filesystem::create_directories(script_root);
    for(const auto& entry:std::filesystem::directory_iterator(fixture_root)) if(entry.is_regular_file()&&
        (entry.path().extension()==".cs"||entry.path().extension()==".csproj"))
        std::filesystem::copy_file(entry.path(),script_root/entry.path().filename(),std::filesystem::copy_options::overwrite_existing);
    const auto script_project=script_root/"ManagedFixture.csproj";
    const auto configure_result=world.scripting_project_configure_json(script_project.parent_path(),script_project);
    if(configure_result.find(R"("success":true)")==std::string::npos){std::cerr<<configure_result<<'\n';return 3;}
    noemancer::EditorUi editor(world, assets);
    if(editor.compile_scripts("Debug").find(R"("success":true)")==std::string::npos)return 4;
    if(const auto* real_debug_probe=std::getenv("NOEMANCER_REAL_NETCOREDBG_PROBE");real_debug_probe&&std::string_view(real_debug_probe)=="1") {
        const auto succeeded=[](const std::string_view receipt){return receipt.find(R"("success":true)")!=std::string_view::npos;};
        const auto start=world.scripting_debug_session_start_json();
        if(!succeeded(start)){std::cerr<<start<<'\n';return 40;}
        std::string initialize;
        try {initialize=world.scripting_debug_session_request_json("initialize",
            R"({"clientID":"noemancer-test","adapterID":"coreclr","pathFormat":"path"})",10000U);}
        catch(const std::exception& exception){std::cerr<<exception.what()<<'\n';return 42;}
        const auto attach=world.scripting_debug_session_request_json("attach",
            std::string{"{\"processId\":"}+std::to_string(_getpid())+R"(,"justMyCode":false})",10000U);
        auto breakpoint_path=(script_root/"PlayerController.cs").string();
        for(std::size_t offset=0;(offset=breakpoint_path.find('\\',offset))!=std::string::npos;offset+=2U)breakpoint_path.replace(offset,1U,"\\\\");
        const auto breakpoint=world.scripting_debug_session_request_json("setBreakpoints",
            std::string{"{\"source\":{\"path\":\""}+breakpoint_path+
                R"("},"breakpoints":[{"line":13}],"sourceModified":false})",10000U);
        const auto configured=world.scripting_debug_session_request_json("configurationDone","{}",10000U);
        const auto resolved_breakpoint=world.scripting_debug_session_request_json("setBreakpoints",
            std::string{"{\"source\":{\"path\":\""}+breakpoint_path+
                R"("},"breakpoints":[{"line":13}],"sourceModified":false})",10000U);
        const auto script_attached=world.scripting_attach_json("script.real-debug-probe","entity.demo-cube","project.script","ManagedFixture.PlayerController");
        auto invocation=std::async(std::launch::async,[&world]{return world.scripting_invoke_json(
            "script.real-debug-probe","OnCreate",R"({"properties":{}})");});
        bool stopped_event{};std::string events;
        const auto stopped_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(std::chrono::steady_clock::now()<stopped_deadline) {
            events=world.scripting_debug_session_events_json();
            if(events.find(R"("event":"stopped")")!=std::string::npos){stopped_event=true;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto thread_marker=events.find(R"("threadId":)");std::uint64_t debug_thread_id{1};
        if(thread_marker!=std::string::npos)debug_thread_id=std::strtoull(events.c_str()+thread_marker+11,nullptr,10);
        const auto threads=world.scripting_debug_session_request_json("threads","{}",10000U);
        const auto stack=world.scripting_debug_session_request_json("stackTrace",
            std::string{"{\"threadId\":"}+std::to_string(debug_thread_id)+R"(,"startFrame":0,"levels":64})",10000U);
        const auto continued=world.scripting_debug_session_request_json("continue",
            std::string{"{\"threadId\":"}+std::to_string(debug_thread_id)+R"(,"singleThread":false})",10000U);
        auto invocation_ready=invocation.wait_for(std::chrono::seconds(10))==std::future_status::ready;
        const auto stopped=world.scripting_debug_session_stop_json(10000U);
        if(!invocation_ready)invocation_ready=invocation.wait_for(std::chrono::seconds(5))==std::future_status::ready;
        const auto invocation_result=invocation_ready?invocation.get():std::string{};
        if(!succeeded(initialize)||!succeeded(attach)||!succeeded(breakpoint)||!succeeded(configured)||!succeeded(resolved_breakpoint)||
           resolved_breakpoint.find(R"("verified":true)")==std::string::npos||!succeeded(script_attached)||!stopped_event||!succeeded(threads)||!succeeded(stack)||
           stack.find("PlayerController.OnCreate")==std::string::npos||!succeeded(continued)||!invocation_ready||!succeeded(invocation_result)||!succeeded(stopped)) {
            std::cerr<<"initialize="<<initialize<<"\nattach="<<attach<<"\nbreakpoint="<<breakpoint<<"\nconfigured="<<configured<<
                "\nresolvedBreakpoint="<<resolved_breakpoint<<
                "\nscriptAttached="<<script_attached<<"\nevents="<<events<<"\nthreads="<<threads<<"\nstack="<<stack<<
                "\ncontinued="<<continued<<"\ninvocation="<<invocation_result<<"\nstopped="<<stopped<<
                "\nstate="<<world.scripting_debug_session_status_json()<<'\n';return 41;
        }
        const auto* player_program=std::getenv("NOEMANCER_REAL_PLAYER_PROGRAM");
        const auto* player_source=std::getenv("NOEMANCER_REAL_PLAYER_SOURCE");
        if(player_program&&*player_program&&player_source&&*player_source) {
            const auto json_path=[](std::string value) {
                for(std::size_t offset=0;(offset=value.find('\\',offset))!=std::string::npos;offset+=2U)value.replace(offset,1U,"\\\\");
                return value;
            };
            const auto player_source_json=json_path(std::filesystem::path(player_source).string());
            const auto player_start=world.scripting_debug_session_start_json();
            const auto player_initialize=world.scripting_debug_session_request_json("initialize",
                R"({"clientID":"noemancer-player-test","adapterID":"coreclr","pathFormat":"path","linesStartAt1":true,"columnsStartAt1":true})",10000U);
#ifdef _WIN32
            const auto event_base="Local\\Noemancer.Player.Test."+std::to_string(_getpid());
            const auto ready_event=CreateEventA(nullptr,TRUE,FALSE,(event_base+".Ready").c_str());
            const auto release_event=CreateEventA(nullptr,TRUE,FALSE,(event_base+".Release").c_str());
            STARTUPINFOA startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION player_process{};
            auto command_line="\""+std::string(player_program)+"\" --debug-ready \""+event_base+".Ready\" --debug-wait \""+event_base+".Release"+
                "\" --headless --frames 6000 --format json";
            std::vector<char> command_buffer(command_line.begin(),command_line.end());command_buffer.push_back('\0');
            const auto process_started=ready_event!=nullptr&&release_event!=nullptr&&CreateProcessA(nullptr,command_buffer.data(),nullptr,nullptr,FALSE,
                CREATE_NO_WINDOW,nullptr,std::filesystem::path(player_program).parent_path().string().c_str(),&startup,&player_process);
            if(process_started)CloseHandle(player_process.hThread);
            const auto player_ready=process_started&&WaitForSingleObject(ready_event,10000U)==WAIT_OBJECT_0;
            const auto launched_player_id=process_started?static_cast<std::uint64_t>(player_process.dwProcessId):0U;
#else
            const auto process_started=false;
            const auto player_ready=false;
            const std::uint64_t launched_player_id{};
#endif
            const auto player_attach=player_ready?world.scripting_debug_session_request_json("attach",
                std::string{"{\"processId\":"}+std::to_string(launched_player_id)+",\"justMyCode\":false}",10000U):std::string{};
            const auto player_breakpoint=world.scripting_debug_session_request_json("setBreakpoints",
                std::string{"{\"source\":{\"path\":\""}+player_source_json+
                "\"},\"breakpoints\":[{\"line\":14}],\"sourceModified\":false}",10000U);
            const auto player_configured=world.scripting_debug_session_request_json("configurationDone","{}",10000U);
#ifdef _WIN32
            if(release_event!=nullptr)SetEvent(release_event);
#endif
            bool player_stopped{};std::string player_events;std::uint64_t player_thread_id{1};
            const auto player_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(std::chrono::steady_clock::now()<player_deadline) {
                player_events=world.scripting_debug_session_events_json();
                if(player_events.find(R"("event":"stopped")")!=std::string::npos){player_stopped=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto player_thread_marker=player_events.find(R"("threadId":)");
            if(player_thread_marker!=std::string::npos)player_thread_id=std::strtoull(player_events.c_str()+player_thread_marker+11,nullptr,10);
            const auto player_stack=player_stopped?world.scripting_debug_session_request_json("stackTrace",
                std::string{"{\"threadId\":"}+std::to_string(player_thread_id)+",\"startFrame\":0,\"levels\":64}",10000U):std::string{};
            const auto player_terminated=world.scripting_debug_session_stop_json(10000U);
#ifdef _WIN32
            if(process_started){WaitForSingleObject(player_process.hProcess,5000U);CloseHandle(player_process.hProcess);}
            if(ready_event!=nullptr)CloseHandle(ready_event);
            if(release_event!=nullptr)CloseHandle(release_event);
#endif
            if(!succeeded(player_start)||!succeeded(player_initialize)||!process_started||!player_ready||!succeeded(player_attach)||!succeeded(player_breakpoint)||
               !succeeded(player_configured)||!player_stopped||!succeeded(player_stack)||
               player_stack.find("CourierController.OnCreate")==std::string::npos||!succeeded(player_terminated)) {
                std::cerr<<"playerStart="<<player_start<<"\nplayerInitialize="<<player_initialize<<"\nplayerAttach="<<player_attach<<
                    "\nplayerBreakpoint="<<player_breakpoint<<"\nplayerConfigured="<<player_configured<<"\nplayerEvents="<<player_events<<
                    "\nplayerStack="<<player_stack<<"\nplayerTerminated="<<player_terminated<<'\n';return 43;
            }
        }
    }
    if(!editor.open_script_source((script_project.parent_path()/"PlayerController.cs").generic_string(),3,5))return 5;
    if(editor.open_script_source(std::filesystem::path(__FILE__).generic_string(),1,1))return 6;
    if(!editor.begin_compile_scripts("Release")||!editor.script_compile_busy()||editor.begin_compile_scripts("Debug")||
       editor.compile_scripts("Debug").find("scripting.compile-job-busy")==std::string::npos)return 7;
    const auto running_snapshot=editor.semantic_snapshot_json();
    if(running_snapshot.find("noemancer.editor-script-build-job/0.1")==std::string::npos||
       running_snapshot.find(R"("state":"running")")==std::string::npos||
       running_snapshot.find(R"("editWorldWritable":false)")==std::string::npos)return 7;
    const auto build_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(editor.script_compile_busy()&&std::chrono::steady_clock::now()<build_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ImGui::NewFrame();editor.render();ImGui::Render();
    }
    const auto manual_completion=editor.consume_script_build_completion();
    if(editor.script_compile_busy()||editor.semantic_snapshot_json().find(R"("state":"succeeded")")==std::string::npos||
       !manual_completion||manual_completion->configuration!="Release"||manual_completion->trigger!="manual"||
       manual_completion->result_json.find(R"("success":true)")==std::string::npos)return 7;
    {
        std::ofstream changed_source(script_root/"PlayerController.cs",std::ios::binary|std::ios::app);
        changed_source<<"\n// editor auto-build fingerprint probe\n";
    }
    const auto auto_build_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    bool observed_auto_build{};
    while(std::chrono::steady_clock::now()<auto_build_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ImGui::NewFrame();editor.render();ImGui::Render();
        const auto status=editor.semantic_snapshot_json();
        if(status.find(R"("trigger":"auto-source-change")")!=std::string::npos&&!editor.script_compile_busy()) {
            observed_auto_build=status.find(R"("state":"succeeded")")!=std::string::npos;break;
        }
    }
    const auto auto_completion=editor.consume_script_build_completion();
    if(!observed_auto_build||!editor.auto_compile_scripts()||!auto_completion||
       auto_completion->trigger!="auto-source-change"||auto_completion->configuration!="Debug"||
       auto_completion->result_json.find(R"("success":true)")==std::string::npos) {
        std::cerr<<editor.semantic_snapshot_json()<<'\n';return 8;
    }
    editor.set_project_context({.project_id="project.test",.name="Test Project",.root="D:/test-project",
        .startup_scene="scenes/start.scene.json",.asset_roots={"assets"},
        .hybrid_pixel_profile=noemancer::HybridPixelProfile{}});
    editor.set_exposure(2.0F);
    editor.set_render_surface(1, 960, 540);
    editor.set_render_status(R"({"passes":["shadow-depth","opaque-lit"]})");
    editor.set_managed_debug_context(
        R"({"schemaVersion":"noemancer.managed-debug-session-events/0.1","success":true,"events":[{"kind":"event","event":"stopped","body":{"reason":"breakpoint","threadId":1}}]})",
        R"({"schemaVersion":"noemancer.editor-managed-debug-action/0.1","success":true,"code":"ok","command":"stackTrace"})");
    ImGui::NewFrame();
    editor.render();
    ImGui::Render();

    const auto snapshot = editor.semantic_snapshot_json();
    const bool has_live_selection = snapshot.find("entity.demo-cube") != std::string::npos;
    const bool has_gizmo_contract=snapshot.find(R"("transformTool":"translate")")!=std::string::npos &&
        snapshot.find("ImGuizmo 1.10 + Noemancer transaction adapter")!=std::string::npos;
    const bool has_tile_brush_contract=snapshot.find(R"("gridRadiusCells":8)")!=std::string::npos&&
        snapshot.find(R"("preview":"projected-grid-and-cell-ghost")")!=std::string::npos&&snapshot.find("pendingCellsTruncated")!=std::string::npos;
    const bool has_palette_rule_contract=snapshot.find("paletteRuleEditor")!=std::string::npos&&
        snapshot.find("asset.tile-palette.autotile")!=std::string::npos&&snapshot.find("preview-then-atomic-commit")!=std::string::npos;
    const bool has_editor_camera=snapshot.find(R"("source":"editor-ephemeral")")!=std::string::npos &&
        snapshot.find(R"("frameSelected":"F")")!=std::string::npos && editor.render_camera_override().has_value();
    const bool has_project_context=snapshot.find(R"("id":"project.test")")!=std::string::npos&&
        snapshot.find("scenes/start.scene.json")!=std::string::npos&&
        snapshot.find("noemancer.hybrid-pixel-profile/0.1")!=std::string::npos&&
        snapshot.find(R"("virtualWidth":320)")!=std::string::npos;
    const bool has_hybrid_profile_authoring=snapshot.find("projectSettingsHybridPixelProfile")!=std::string::npos&&
        snapshot.find("noemancer.hybrid-pixel-profile-panel/0.1")!=std::string::npos&&
        snapshot.find("editor.project-settings.hybrid-pixel-profile.apply")!=std::string::npos&&
        snapshot.find(R"("physicalWidth":960)")!=std::string::npos&&
        snapshot.find(R"("physicalHeight":540)")!=std::string::npos&&
        snapshot.find(R"("expectedRevision":)")==std::string::npos;
    const bool has_editor_chrome=snapshot.find("noemancer.editor-chrome/0.1")!=std::string::npos&&
        snapshot.find("modern-precision-tool/0.1")!=std::string::npos&&
        snapshot.find(R"("id":"editor.command-bar")")!=std::string::npos&&
        snapshot.find(R"("id":"editor.scene-view")")!=std::string::npos&&
        snapshot.find(R"("authority":"edit-world")")!=std::string::npos&&
        snapshot.find(R"("selectedEntityId":"entity.demo-cube")")!=std::string::npos&&
        snapshot.find(R"("componentCount":)")!=std::string::npos&&
        snapshot.find(R"("id":"editor.asset-browser")")!=std::string::npos&&
        snapshot.find(R"("id":"editor.animation-graph")")!=std::string::npos&&
        snapshot.find(R"("role":"asset-workspace")")!=std::string::npos&&
        snapshot.find("noemancer.editor-retained-panels/0.1")!=std::string::npos&&
        snapshot.find("noemancer.retained-ui-actions/0.1")!=std::string::npos&&
        snapshot.find(R"("dispatch":"runtime-adapter-to-domain-plan-apply-receipt")")!=std::string::npos&&
        snapshot.find(R"("role":"main-workspace")")!=std::string::npos&&
        snapshot.find(R"("currentProject":"Test Project")")!=std::string::npos;
    const bool has_scene_lifecycle=snapshot.find(R"("sceneLifecycle":{)")!=std::string::npos&&
        snapshot.find("asset-free-root-camera-sun")!=std::string::npos&&
        snapshot.find("noemancer.scene-recovery-candidates/0.1")!=std::string::npos;
    const bool has_scripting_context=snapshot.find("noemancer.editor-scripting-status/0.1")!=std::string::npos&&
        snapshot.find("noemancer.script-project-state/0.3")!=std::string::npos&&snapshot.find("ManagedFixture.csproj")!=std::string::npos&&
        snapshot.find(R"("sourceDocuments":{)")!=std::string::npos&&snapshot.find("PlayerController.cs")!=std::string::npos&&
        snapshot.find(R"("lastCompile":{)")!=std::string::npos&&
        snapshot.find(R"("needsCompile":false)")!=std::string::npos&&snapshot.find("noemancer.editor-source-location/0.1")!=std::string::npos&&
        snapshot.find("PlayerController.cs")!=std::string::npos&&snapshot.find("noemancer.managed-debug-attach/0.1")!=std::string::npos&&
        snapshot.find(R"("targetReady":true)")!=std::string::npos&&snapshot.find("noemancer.editor-managed-debug-events/0.1")!=std::string::npos&&
        snapshot.find(R"("event":"stopped")")!=std::string::npos&&snapshot.find(R"("command":"stackTrace")")!=std::string::npos;
    editor.set_simulation_state(noemancer::EditorSimulationState::paused);
    editor.set_play_world_context(
        R"({"schemaVersion":"noemancer.world-observation/0.1","entities":[{"id":"entity.runtime-created","name":"Runtime Created","parentId":null}]})",
        R"({"schemaVersion":"noemancer.inspector-document/0.1","entity":{"id":"entity.runtime-created","name":"Runtime Created"},"components":[]})",
        R"({"schemaVersion":"noemancer.play-world-apply-plan/0.2","valid":true,"changes":[{"changeId":"play-change-runtime-created","entityId":"entity.runtime-created","field":"engine.scene.entity.added","operation":"add-entity","before":null,"after":{}}]})");
    const auto paused_snapshot=editor.semantic_snapshot_json();
    const bool has_play_state=paused_snapshot.find(R"("state":"paused")")!=std::string::npos &&
        paused_snapshot.find(R"("editWorldWritable":false)")!=std::string::npos &&
        paused_snapshot.find("apply-back-and-stop")!=std::string::npos &&
        paused_snapshot.find("diff-preview-then-atomic-commit")!=std::string::npos &&
        paused_snapshot.find(R"("runtimeSelection":"entity.runtime-created")")!=std::string::npos &&
        paused_snapshot.find(R"("selectedChangeIds":["play-change-runtime-created"])")!=std::string::npos &&
        paused_snapshot.find("noemancer.inspector-document/0.1")!=std::string::npos &&
        !editor.render_camera_override().has_value();
    const bool produced_draw_data = ImGui::GetDrawData() != nullptr;
    ImGui::DestroyContext();

    if (!has_live_selection || !has_gizmo_contract || !has_tile_brush_contract || !has_palette_rule_contract || !has_editor_camera || !has_project_context || !has_hybrid_profile_authoring || !has_editor_chrome || !has_scene_lifecycle || !has_scripting_context || !has_play_state || !produced_draw_data || editor.requested_exposure() != 2.0F ||
        editor.requested_scene_width() < 64 || editor.requested_scene_height() < 64) {
        std::cerr << "Editor UI did not render a semantic frame from the live World\n";
        return 2;
    }
    return 0;
}
