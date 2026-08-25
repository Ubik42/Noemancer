#include "engine/command_registry.hpp"
#include "engine/asset_registry.hpp"
#include "engine/process_diagnostics.hpp"
#include "engine/network_transport.hpp"
#include "engine/project_document.hpp"
#include "engine/project_ui_authoring.hpp"
#include "engine/project_workspace.hpp"
#include "engine/world.hpp"
#include "runtime/application.hpp"
#include "runtime/performance_evidence.hpp"
#include "runtime/windows_package_service.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
    char* raw=nullptr;
    std::size_t bytes{};
    if(_dupenv_s(&raw,&bytes,name)!=0||raw==nullptr)return std::nullopt;
    std::string value{raw};
    std::free(raw);
    return value.empty()?std::nullopt:std::optional<std::string>{std::move(value)};
#else
    const auto* raw=std::getenv(name);
    return raw!=nullptr&&*raw!='\0'?std::optional<std::string>{raw}:std::nullopt;
#endif
}

std::filesystem::path process_report_directory() {
    if(const auto configured=environment_value("NOEMANCER_DIAGNOSTICS_DIR"))
        return std::filesystem::path{*configured};
#ifdef _WIN32
    if(const auto local_app_data=environment_value("LOCALAPPDATA"))
        return std::filesystem::path{*local_app_data}/"Noemancer/Diagnostics";
#endif
    return std::filesystem::temp_directory_path()/"Noemancer/Diagnostics";
}

void print_usage() {
    std::cout
        << "Usage:\n"
        << "  noemancer run [--headless] [--frames N] [--input-sample SOURCE VALUE] [--input-event FRAME SOURCE VALUE] [--format human|json]\n"
        << "                 [--capture-frame PATH] [--capture-editor-frame PATH] [--probe-pixel X Y] [--exposure VALUE] [--render-scale VALUE]\n"
        << "                 [--editor-select-asset ASSET_ID] [--editor-project-settings]\n"
        << "                 [--shadow-quality low|medium|high]\n"
        << "                 [--texture-streaming-budget-kib N] [--texture-streaming-resident-budget-kib N]\n"
        << "                 [--texture-streaming-workload noemancer.texture-streaming.pressure/0.1]\n"
        << "                 [--temporal-debug final|motion|reactive|disocclusion|history-weight|history-clamp|linear-depth|normal]\n"
        << "                 [--ssr-quality low|medium|high] [--ssr-debug final|confidence|hit-distance|roughness|miss|normal]\n"
        << "                 [--reference-scene commercial-raster-v1]\n"
        << "                 [--render-stress-instances N]\n"
        << "                 [--animation-physics-stress]\n"
        << "                 [--vfx-respawn-interval N]\n"
        << "                 [--gpu-backend auto|direct3d12|vulkan|metal] [--gpu-debug] [--disable-gpu-driven] [--disable-ambient-occlusion] [--disable-auto-exposure] [--disable-ssr]\n"
        << "                 [--gpu-visibility-readback] [--render-stress-offscreen-percent N]\n"
        << "                 [--ui-locale LOCALE] [--ui-scale SCALE]\n"
        << "                 [--project PATH]\n"
        << "                 [--performance-evidence PATH] [--performance-hidden] [--performance-workload ID]\n"
        << "                 [--performance-warmup-frames N] [--performance-sample-frames N]\n"
        << "                 [--window-width N] [--window-height N]\n"
        << "  noemancer player --profile PATH [--frames N] [--gpu-backend auto|direct3d12|vulkan|metal]\n"
        << "  noemancer schema|world [--format human|json]\n"
        << "  noemancer bindings csharp\n"
        << "  noemancer tools list [--format json]\n"
        << "  noemancer tool call <name> [--input JSON]\n"
        << "  noemancer serve [--project PATH] --format jsonl\n"
        << "  noemancer project create --path PATH --name NAME [--preset starter|hybrid-pixel] [--format json]\n"
        << "  noemancer network-server --port PORT [--sessions N] [--timeout-ms N] --format json\n"
        << "  noemancer network-client --host IPv4 --port PORT [--peer-id ID] [--payload-bytes N] --format json\n"
        << "  noemancer package --project PATH --output PATH [--target-profile windows-x64-release|windows-x64-debug] [--dry-run] [--format json]\n";
}

bool parse_frames(const std::string_view value, std::uint32_t& output) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool parse_float(const std::string_view value, float& output) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool stdin_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

std::string read_tool_input(const std::string_view inline_input) {
    if (!inline_input.empty()) {
        return std::string(inline_input);
    }
    if (stdin_is_terminal()) {
        return "{}";
    }
    std::string input{
        std::istreambuf_iterator<char>(std::cin),
        std::istreambuf_iterator<char>()};
    return input.empty() ? "{}" : input;
}

int run_package_cli(const int argc, char** argv) {
    noemancer::WindowsPackageOptions options;
    if (argc > 0 && argv[0] != nullptr) options.runtime_executable = std::filesystem::absolute(argv[0]);
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--project" && index + 1 < argc) {
            options.project_path = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            options.output_path = argv[++index];
        } else if (argument == "--target-profile" && index + 1 < argc) {
            options.target_profile = argv[++index];
        } else if (argument == "--dry-run") {
            options.dry_run = true;
        } else if (argument == "--format" && index + 1 < argc &&
                   std::string_view(argv[++index]) == "json") {
        } else {
            std::cerr << "Expected: noemancer package --project PATH --output PATH [--target-profile windows-x64-release|windows-x64-debug] [--dry-run] [--format json]\n";
            return 2;
        }
    }
    if (options.project_path.empty() || options.output_path.empty()) {
        std::cerr << "Package project and output paths are required\n";
        return 2;
    }
    const auto output = noemancer::run_windows_package_json(options);
    std::cout << output << '\n';
    const auto parsed = nlohmann::json::parse(output, nullptr, false);
    return !parsed.is_discarded() && parsed.value("success", false) ? 0 : 30;
}

int run_project_cli(const int argc,char** argv) {
    if(argc<3||std::string_view(argv[2])!="create") {
        std::cerr<<"Expected: noemancer project create --path PATH --name NAME [--preset starter|hybrid-pixel] [--format json]\n";
        return 2;
    }
    noemancer::ProjectWorkspaceCreateRequest request;
    for(int index=3;index<argc;++index) {
        const std::string_view argument=argv[index];
        if(argument=="--path"&&index+1<argc)request.root=argv[++index];
        else if(argument=="--name"&&index+1<argc)request.name=argv[++index];
        else if(argument=="--preset"&&index+1<argc)request.preset=argv[++index];
        else if(argument=="--format"&&index+1<argc&&std::string_view(argv[++index])=="json") {}
        else {
            std::cerr<<"Expected: noemancer project create --path PATH --name NAME [--preset starter|hybrid-pixel] [--format json]\n";
            return 2;
        }
    }
    if(request.root.empty()||request.name.empty()) {
        std::cerr<<"Project path and name are required\n";
        return 2;
    }
    const auto output=noemancer::create_project_workspace_json(request);
    std::cout<<output<<'\n';
    const auto parsed=nlohmann::json::parse(output,nullptr,false);
    return !parsed.is_discarded()&&parsed.value("success",false)?0:30;
}

int run_agent_interface(const int argc, char** argv) {
    const std::string_view command = argv[1];
    const noemancer::CommandRegistry registry;

    if (command == "tools") {
        if (argc < 3 || std::string_view(argv[2]) != "list") {
            std::cerr << "Expected: noemancer tools list [--format json]\n";
            return 2;
        }
        if (argc > 3 &&
            !(argc == 5 && std::string_view(argv[3]) == "--format" &&
              std::string_view(argv[4]) == "json")) {
            std::cerr << "tools list only supports --format json\n";
            return 2;
        }
        std::cout << registry.manifest_json() << '\n';
        return 0;
    }

    if (argc < 4 || std::string_view(argv[2]) != "call") {
        std::cerr << "Expected: noemancer tool call <name> [--input JSON]\n";
        return 2;
    }

    std::string_view inline_input;
    if (argc > 4) {
        if (argc != 6 || std::string_view(argv[4]) != "--input") {
            std::cerr << "tool call only supports --input JSON\n";
            return 2;
        }
        inline_input = argv[5];
    }

    const auto invocation = registry.invoke(argv[3], read_tool_input(inline_input));
    std::cout << invocation.output_json << '\n';
    return invocation.exit_code;
}

int run_agent_server(const int argc, char** argv) {
    std::filesystem::path project_path;
    bool jsonl{};
    for(int index=2;index<argc;++index) {
        const std::string_view argument=argv[index];
        if(argument=="--project"&&index+1<argc)project_path=argv[++index];
        else if(argument=="--format"&&index+1<argc&&std::string_view(argv[++index])=="jsonl")jsonl=true;
        else {
            std::cerr << "Expected: noemancer serve [--project PATH] --format jsonl\n";
            return 2;
        }
    }
    if(!jsonl) {
        std::cerr << "Expected: noemancer serve [--project PATH] --format jsonl\n";
        return 2;
    }
    std::unique_ptr<noemancer::World> project_world;
    std::unique_ptr<noemancer::AssetRegistry> project_assets;
    std::unique_ptr<noemancer::ProjectUiAuthoringSession> project_ui;
    std::unique_ptr<noemancer::CommandRegistry> registry;
    if(project_path.empty())registry=std::make_unique<noemancer::CommandRegistry>();
    else {
        const auto loaded=noemancer::load_project(project_path);
        if(!loaded) {std::cerr<<noemancer::project_load_errors_json(loaded)<<'\n';return 30;}
        project_world=std::make_unique<noemancer::World>();
        const auto scene=project_world->load_scene(*loaded.startup_scene);
        if(!scene.success||!project_world->configure_input_actions(loaded.project->input_actions)||
           !project_world->configure_project_hud(loaded.project->hud_document_json)) {
            std::cerr<<"Project could not initialize the attached Agent World.\n";return 30;
        }
        if(loaded.project->asset_roots.empty())project_assets=std::make_unique<noemancer::AssetRegistry>();
        else {
            project_assets=std::make_unique<noemancer::AssetRegistry>(
                loaded.project->root/loaded.project->asset_roots.front());
            for(std::size_t index=1;index<loaded.project->asset_roots.size();++index)
                static_cast<void>(project_assets->add_root(loaded.project->root/loaded.project->asset_roots[index]));
        }
        if(loaded.project->script_project)
            static_cast<void>(project_world->scripting_project_configure_json(
                loaded.project->root,loaded.project->root/ *loaded.project->script_project));
        registry=std::make_unique<noemancer::CommandRegistry>(*project_world,*project_assets);
        if(loaded.project->hud_document) {
            project_ui=std::make_unique<noemancer::ProjectUiAuthoringSession>(
                loaded.project->hud_document_json,loaded.project->root/ *loaded.project->hud_document,
                nlohmann::json::parse(loaded.project->hud_document_json).value("revision",1ULL));
            if(!project_ui->valid()) {std::cerr<<project_ui->observation_json()<<'\n';return 30;}
            registry->attach_project_ui_authoring(*project_ui);
        }
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nlohmann::json request_id = nullptr;
        nlohmann::json response;
        try {
            const auto request = nlohmann::json::parse(line);
            request_id = request.value("id", nlohmann::json(nullptr));
            if (!request.is_object() || !request.contains("name") || !request.at("name").is_string()) {
                throw std::invalid_argument("Request must contain a string name");
            }
            const auto arguments = request.value("arguments", nlohmann::json::object());
            const auto invocation = registry->invoke(
                request.at("name").get<std::string>(),
                arguments.dump());
            response = {
                {"id", request_id},
                {"exitCode", invocation.exit_code},
                {"response", nlohmann::json::parse(invocation.output_json)}
            };
        } catch (const std::exception& error) {
            response = {
                {"id", request_id},
                {"exitCode", 4},
                {"response", {
                    {"protocolVersion", "0.2"},
                    {"ok", false},
                    {"error", {{"code", "invalid_request"}, {"message", error.what()}}}
                }}
            };
        }
        std::cout << response.dump() << '\n' << std::flush;
    }
    return 0;
}

int run_network_session_cli(const int argc,char** argv,const bool server) {
    std::uint32_t port{};
    std::uint32_t timeout_milliseconds{5000};
    std::uint32_t sessions{1};
    std::uint32_t payload_bytes{256};
    std::string host{"127.0.0.1"};
    std::string peer_id{"client.cli"};
    int index=2;
    while(index<argc) {
        const std::string_view argument=argv[index++];
        auto parse_value=[&](std::uint32_t& target) {
            return index<argc&&parse_frames(argv[index++],target);
        };
        if(argument=="--port") { if(!parse_value(port)) { std::cerr<<"Invalid network port\n"; return 2; } }
        else if(argument=="--timeout-ms") { if(!parse_value(timeout_milliseconds)) { std::cerr<<"Invalid network timeout\n"; return 2; } }
        else if(server&&argument=="--sessions") { if(!parse_value(sessions)) { std::cerr<<"Invalid session budget\n"; return 2; } }
        else if(!server&&argument=="--payload-bytes") { if(!parse_value(payload_bytes)) { std::cerr<<"Invalid state payload size\n"; return 2; } }
        else if(!server&&argument=="--host"&&index<argc) host=argv[index++];
        else if(!server&&argument=="--peer-id"&&index<argc) peer_id=argv[index++];
        else if(argument=="--format"&&index<argc&&std::string_view(argv[index++])=="json") {}
        else { std::cerr<<"Unknown network session argument: "<<argument<<'\n'; return 2; }
    }
    if(port==0||port>65535U||timeout_milliseconds<100U||timeout_milliseconds>60000U||
       sessions==0||sessions>64U||payload_bytes==0||payload_bytes>1200U) {
        std::cerr<<"Network session arguments are outside their bounded ranges\n"; return 2;
    }
    const auto document=server
        ? noemancer::run_network_server_json(static_cast<std::uint16_t>(port),sessions,timeout_milliseconds)
        : noemancer::run_network_client_json(host,static_cast<std::uint16_t>(port),peer_id,payload_bytes,timeout_milliseconds);
    std::cout<<document<<'\n';
    const auto parsed=nlohmann::json::parse(document,nullptr,false);
    return !parsed.is_discarded()&&parsed.value("success",false)?0:30;
}

} // namespace

int main(int argc, char** argv) {
    const auto diagnostics_directory=process_report_directory().string();
    noemancer::configure_process_diagnostics(noemancer::process_diagnostics_options{
        "noemancer.runtime",diagnostics_directory});
    noemancer::StartupTelemetry startup_telemetry;
    startup_telemetry.begin_phase("process.argument-parse");
    if (argc > 1) {
        const std::string_view first_argument = argv[1];
        if (first_argument == "package") return run_package_cli(argc, argv);
        if(first_argument=="project")return run_project_cli(argc,argv);
        if(first_argument=="network-server") return run_network_session_cli(argc,argv,true);
        if(first_argument=="network-client") return run_network_session_cli(argc,argv,false);
        if (first_argument == "serve") {
            return run_agent_server(argc, argv);
        }
        if (first_argument == "tools" || first_argument == "tool") {
            return run_agent_interface(argc, argv);
        }
        if (first_argument == "bindings") {
            if (argc != 3 || std::string_view(argv[2]) != "csharp") {
                std::cerr << "Expected: noemancer bindings csharp\n";
                return 2;
            }
            const noemancer::World world;
            std::cout << world.managed_bindings_source();
            return 0;
        }
    }

    std::string_view command = "run";
    noemancer::RunOptions options{};
    options.startup_telemetry=&startup_telemetry;
    if(argc>0&&argv[0]!=nullptr)options.runtime_executable=std::filesystem::absolute(argv[0]).string();
    if((argc==1||(argc>1&&argv[1][0]=='-'))&&!options.runtime_executable.empty()) {
        const auto executable=std::filesystem::path(options.runtime_executable);
        const auto packaged_profile=(executable.parent_path().parent_path()/"config"/"game-profile.json").lexically_normal();
        std::error_code profile_error;
        if(std::filesystem::is_regular_file(packaged_profile,profile_error)) {
            command="player";options.player_mode=true;options.player_profile_path=packaged_profile.string();
        }
    }

    int index = 1;
    if (index < argc && argv[index][0] != '-') {
        command = argv[index++];
    }
    if(command=="player")options.player_mode=true;
    while (index < argc) {
        const std::string_view argument = argv[index++];
        if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--frames" && index < argc) {
            if (!parse_frames(argv[index++], options.frames)) {
                std::cerr << "Invalid frame count\n";
                return 2;
            }
        } else if (argument == "--format" && index < argc) {
            const std::string_view format = argv[index++];
            if (format == "json") {
                options.log_format = noemancer::LogFormat::Json;
            } else if (format != "human") {
                std::cerr << "Unknown format\n";
                return 2;
            }
        } else if (argument == "--debug-wait" && index < argc) {
            options.debug_wait_event = argv[index++];
        } else if (argument == "--debug-ready" && index < argc) {
            options.debug_ready_event = argv[index++];
        } else if (argument == "--capture-frame" && index < argc) {
            options.capture_frame_path = argv[index++];
        } else if (argument == "--capture-editor-frame" && index < argc) {
            options.capture_editor_frame_path = argv[index++];
        } else if(argument=="--editor-select-asset"&&index<argc) {
            options.editor_selected_asset_id=argv[index++];
        } else if (argument == "--editor-project-settings") {
            options.editor_project_settings = true;
        } else if (argument == "--probe-pixel" && index + 1 < argc) {
            std::uint32_t x{};
            std::uint32_t y{};
            if (!parse_frames(argv[index++], x) || !parse_frames(argv[index++], y)) {
                std::cerr << "Invalid probe pixel\n";
                return 2;
            }
            options.probe_pixel = noemancer::ScenePickRequest{x, y};
        } else if (argument == "--exposure" && index < argc) {
            float exposure{};
            if (!parse_float(argv[index++], exposure) || exposure < 0.125F || exposure > 8.0F) {
                std::cerr << "Exposure must be in [0.125, 8.0]\n";
                return 2;
            }
            options.exposure = exposure;
        } else if (argument == "--render-scale" && index < argc) {
            float render_scale{};
            if (!parse_float(argv[index++], render_scale) || render_scale < 0.5F || render_scale > 1.0F) {
                std::cerr << "Render scale must be in [0.5, 1.0]\n";
                return 2;
            }
            options.render_scale = render_scale;
        } else if (argument == "--shadow-quality" && index < argc) {
            options.shadow_quality = argv[index++];
            if (options.shadow_quality != "low" && options.shadow_quality != "medium" && options.shadow_quality != "high") {
                std::cerr << "Shadow quality must be low, medium, or high\n";
                return 2;
            }
        } else if (argument == "--texture-streaming-budget-kib" && index < argc) {
            if (!parse_frames(argv[index++], options.texture_streaming_budget_kib) ||
                options.texture_streaming_budget_kib<1U || options.texture_streaming_budget_kib>65536U) {
                std::cerr << "Texture streaming budget must be in [1, 65536] KiB\n";
                return 2;
            }
        } else if (argument == "--texture-streaming-resident-budget-kib" && index < argc) {
            if (!parse_frames(argv[index++], options.texture_streaming_resident_budget_kib) ||
                options.texture_streaming_resident_budget_kib<1024U) {
                std::cerr << "Texture streaming resident budget must be at least 1024 KiB\n";
                return 2;
            }
        } else if (argument == "--texture-streaming-workload" && index < argc) {
            options.texture_streaming_workload=argv[index++];
            if(options.texture_streaming_workload!="noemancer.texture-streaming.pressure/0.1") {
                std::cerr << "Unknown texture streaming workload\n";
                return 2;
            }
        } else if (argument == "--temporal-debug" && index < argc) {
            const std::string mode=argv[index++];
            if (mode!="final" && mode!="motion" && mode!="reactive" && mode!="disocclusion" && mode!="history-weight" &&
                mode!="history-clamp" && mode!="linear-depth" && mode!="normal") {
                std::cerr << "Unknown temporal debug mode\n";
                return 2;
            }
            options.temporal_debug_mode=mode;
        } else if(argument=="--ssr-quality"&&index<argc) {
            options.ssr_quality=argv[index++];
            if(options.ssr_quality!="low"&&options.ssr_quality!="medium"&&options.ssr_quality!="high") {
                std::cerr<<"SSR quality must be low, medium, or high\n";return 2;
            }
        } else if(argument=="--ssr-debug"&&index<argc) {
            options.ssr_debug_mode=argv[index++];
            if(options.ssr_debug_mode!="final"&&options.ssr_debug_mode!="confidence"&&
               options.ssr_debug_mode!="hit-distance"&&options.ssr_debug_mode!="roughness"&&
               options.ssr_debug_mode!="miss"&&options.ssr_debug_mode!="normal") {
                std::cerr<<"Unknown SSR debug mode\n";return 2;
            }
        } else if (argument == "--reference-scene" && index < argc) {
            options.reference_scene_id=argv[index++];
            if(options.reference_scene_id!="commercial-raster-v1") {
                std::cerr << "Unknown reference scene\n";
                return 2;
            }
        } else if (argument == "--render-stress-instances" && index < argc) {
            if (!parse_frames(argv[index++], options.render_stress_instances) ||
                options.render_stress_instances == 0 || options.render_stress_instances > 4096) {
                std::cerr << "Render stress instance count must be in [1, 4096]\n";
                return 2;
            }
        } else if (argument == "--animation-physics-stress") {
            options.animation_physics_stress = true;
        } else if (argument == "--vfx-respawn-interval" && index < argc) {
            if (!parse_frames(argv[index++], options.vfx_respawn_interval) ||
                options.vfx_respawn_interval == 0 || options.vfx_respawn_interval > 3600) {
                std::cerr << "VFX respawn interval must be in [1, 3600]\n";
                return 2;
            }
        } else if (argument == "--gpu-backend" && index < argc) {
            options.gpu_backend = argv[index++];
            if (options.gpu_backend != "auto" && options.gpu_backend != "direct3d12" &&
                options.gpu_backend != "vulkan" && options.gpu_backend != "metal") {
                std::cerr << "GPU backend must be auto, direct3d12, vulkan, or metal\n";
                return 2;
            }
        } else if (argument == "--gpu-debug") {
            options.gpu_debug = true;
        } else if(argument=="--disable-gpu-driven") {
            options.disable_gpu_driven=true;
        } else if(argument=="--disable-ambient-occlusion") {
            options.disable_ambient_occlusion=true;
        } else if(argument=="--disable-auto-exposure") {
            options.disable_auto_exposure=true;
        } else if(argument=="--disable-ssr") {
            options.disable_ssr=true;
        } else if(argument=="--gpu-visibility-readback") {
            options.gpu_visibility_readback=true;
        } else if(argument=="--render-stress-offscreen-percent"&&index<argc) {
            if(!parse_frames(argv[index++],options.render_stress_offscreen_percent)||
               options.render_stress_offscreen_percent<25U||options.render_stress_offscreen_percent>50U) {
                std::cerr<<"Render stress offscreen percent must be in [25, 50]\n";return 2;
            }
        } else if (argument == "--ui-locale" && index < argc) {
            options.ui_locale=argv[index++];
            if(options.ui_locale.empty()||options.ui_locale.size()>32||
               !std::ranges::all_of(options.ui_locale,[](const unsigned char value) {
                   return std::isalnum(value)||value=='-'||value=='_';
               })) {
                std::cerr<<"UI locale must contain only letters, digits, '-' or '_'\n";
                return 2;
            }
        } else if (argument == "--ui-scale" && index < argc) {
            float ui_scale{};
            if(!parse_float(argv[index++],ui_scale)||!std::isfinite(ui_scale)||
               ui_scale<0.75F||ui_scale>3.0F) {
                std::cerr<<"UI scale must be finite and in [0.75, 3.0]\n";
                return 2;
            }
            options.ui_scale=ui_scale;
        } else if (argument == "--project" && index < argc) {
            options.project_path = argv[index++];
            if (options.project_path.empty()) {
                std::cerr << "Project path must not be empty\n";
                return 2;
            }
        } else if (argument == "--input-sample" && index + 1 < argc) {
            std::string source = argv[index++];
            float value{};
            if (source.empty() || source.size() > 96U ||
                !std::ranges::all_of(source, [](const unsigned char byte) {
                    return std::isalnum(byte) || byte == '.' || byte == '-' || byte == '_';
                }) || !parse_float(argv[index++], value) || !std::isfinite(value) ||
                value < -1.0F || value > 1.0F || options.input_samples.size() >= 64U) {
                std::cerr << "Input sample requires a stable SOURCE and VALUE in [-1, 1] (maximum 64)\n";
                return 2;
            }
            options.input_samples.push_back({std::move(source), value});
        } else if (argument == "--input-event" && index + 2 < argc) {
            std::uint32_t frame{};
            if(!parse_frames(argv[index++],frame)) {
                std::cerr << "Input event FRAME must be a non-negative integer\n";
                return 2;
            }
            std::string source=argv[index++];
            float value{};
            if(source.empty()||source.size()>96U||
               !std::ranges::all_of(source,[](const unsigned char byte) {
                   return std::isalnum(byte)||byte=='.'||byte=='-'||byte=='_';
               })||!parse_float(argv[index++],value)||!std::isfinite(value)||value< -1.0F||value>1.0F||
               options.input_events.size()>=1024U) {
                std::cerr << "Input event requires FRAME, stable SOURCE, and VALUE in [-1, 1] (maximum 1024)\n";
                return 2;
            }
            options.input_events.push_back({frame,std::move(source),value});
        } else if(argument=="--performance-evidence"&&index<argc) {
            options.performance_evidence_path=argv[index++];
            if(options.performance_evidence_path.empty()) { std::cerr<<"Performance evidence path must not be empty\n";return 2; }
        } else if(argument=="--performance-hidden") {
            options.performance_hidden=true;
        } else if(argument=="--performance-workload"&&index<argc) {
            options.performance_workload_id=argv[index++];
            if(options.performance_workload_id.empty()||options.performance_workload_id.size()>128||
               !std::ranges::all_of(options.performance_workload_id,[](const unsigned char value) {
                   return std::isalnum(value)||value=='.'||value=='-'||value=='_'||value=='/';
               })) { std::cerr<<"Performance workload ID contains unsupported characters\n";return 2; }
        } else if(argument=="--performance-warmup-frames"&&index<argc) {
            if(!parse_frames(argv[index++],options.performance_warmup_frames)||options.performance_warmup_frames>10000U) {
                std::cerr<<"Performance warmup frames must be in [0, 10000]\n";return 2;
            }
        } else if(argument=="--performance-sample-frames"&&index<argc) {
            if(!parse_frames(argv[index++],options.performance_sample_frames)||options.performance_sample_frames<60U||
               options.performance_sample_frames>10000U) {
                std::cerr<<"Performance sample frames must be in [60, 10000]\n";return 2;
            }
        } else if(argument=="--window-width"&&index<argc) {
            if(!parse_frames(argv[index++],options.window_width)||options.window_width<640U||options.window_width>7680U) {
                std::cerr<<"Window width must be in [640, 7680]\n";return 2;
            }
        } else if(argument=="--window-height"&&index<argc) {
            if(!parse_frames(argv[index++],options.window_height)||options.window_height<360U||options.window_height>4320U) {
                std::cerr<<"Window height must be in [360, 4320]\n";return 2;
            }
        } else if(argument=="--profile"&&index<argc) {
            options.player_profile_path=argv[index++];
            if(options.player_profile_path.empty()){std::cerr<<"Player profile path must not be empty\n";return 2;}
        } else if (argument == "--help") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            print_usage();
            return 2;
        }
    }

    if (command == "schema") {
        const noemancer::World world;
        std::cout << world.schema_json() << '\n';
        return 0;
    }
    if (command == "world") {
        noemancer::World world;
        static_cast<void>(world.load_scene(noemancer::make_bootstrap_scene_document()));
        std::cout << world.snapshot_json() << '\n';
        return 0;
    }
    if (command != "run"&&command!="player") {
        std::cerr << "Unknown command: " << command << '\n';
        print_usage();
        return 2;
    }
    if(options.player_mode&&options.player_profile_path.empty()) {
        std::cerr<<"Player requires --profile PATH\n";return 2;
    }
    if(!options.capture_frame_path.empty()&&!options.capture_editor_frame_path.empty()) {
        std::cerr<<"Scene and Editor capture must be requested in separate runs\n";return 2;
    }
    if(options.player_mode&&!options.capture_editor_frame_path.empty()) {
        std::cerr<<"Editor frame capture is only available in Editor run mode\n";return 2;
    }
    const auto generated_workload_count=(options.reference_scene_id.empty()?0U:1U)+(options.render_stress_instances>0?1U:0U)+
        (options.animation_physics_stress?1U:0U);
    if(generated_workload_count>1U||options.animation_physics_stress&&(!options.project_path.empty()||options.player_mode)) {
        std::cerr<<"Reference scene cannot be combined with a project, Player profile, or render stress scene\n";return 2;
    }

    if (options.headless && (!options.capture_frame_path.empty() || !options.capture_editor_frame_path.empty() || options.probe_pixel)) {
        std::cerr << "GPU capture and pixel probing require the interactive GPU runtime\n";
        return 2;
    }
    if(!options.performance_evidence_path.empty() && (options.headless||options.frames!=0||
       !options.capture_frame_path.empty()||!options.capture_editor_frame_path.empty()||options.probe_pixel||options.gpu_visibility_readback)) {
        std::cerr<<"Performance evidence owns the interactive frame budget and cannot be combined with headless, frames, capture, or probe options\n";
        return 2;
    }
    if(options.performance_hidden&&options.performance_evidence_path.empty()) {
        std::cerr<<"Hidden performance mode requires --performance-evidence\n";return 2;
    }
    if(options.gpu_visibility_readback&&(options.headless||options.disable_gpu_driven||options.probe_pixel||
       options.render_stress_instances<32U||options.render_stress_offscreen_percent==0U)) {
        std::cerr<<"GPU visibility readback requires interactive GPU-driven rendering with at least 32 stress instances and a 25-50% offscreen workload\n";
        return 2;
    }
    if(options.render_stress_offscreen_percent>0U&&!options.gpu_visibility_readback) {
        std::cerr<<"Partial visibility stress is reserved for the explicit GPU visibility readback probe\n";return 2;
    }

    startup_telemetry.finish_phase();
    try {
        noemancer::Application application(options);
        return application.run();
    } catch (const std::exception& error) {
        nlohmann::json evidence{
            {"level", "fatal"},
            {"event", "runtime.unhandled-standard-exception"},
            {"role", "noemancer.runtime"},
            {"exitCode", 70},
            {"detail", error.what()}
        };
        std::cerr << evidence.dump() << '\n';
        startup_telemetry.finish_phase();
        std::cerr << nlohmann::json{
            {"level", "info"}, {"event", "runtime.startup_telemetry"},
            {"message", startup_telemetry.json(options.player_mode ? "player" :
                options.project_path.empty() ? "editor" : "source-project", "unhandled-exception")}
        }.dump() << '\n';
        return 70;
    } catch (...) {
        std::cerr << R"({"level":"fatal","event":"runtime.unhandled-nonstandard-exception","role":"noemancer.runtime","exitCode":71})" << '\n';
        startup_telemetry.finish_phase();
        std::cerr << nlohmann::json{
            {"level", "info"}, {"event", "runtime.startup_telemetry"},
            {"message", startup_telemetry.json(options.player_mode ? "player" :
                options.project_path.empty() ? "editor" : "source-project", "unhandled-exception")}
        }.dump() << '\n';
        return 71;
    }
}
