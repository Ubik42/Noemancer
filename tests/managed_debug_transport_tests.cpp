#include "engine/managed_debug_protocol.hpp"
#include "engine/managed_debug_transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

using Json=nlohmann::json;

void send_message(const Json& message) {
    std::cout<<noemancer::dap_message_frame(message.dump())<<std::flush;
}

int run_long_lived_fake_adapter() {
#ifdef _WIN32
    _setmode(_fileno(stdin),_O_BINARY);
    _setmode(_fileno(stdout),_O_BINARY);
#endif
    std::cerr<<"fake-long-lived-ready\n"<<std::flush;
    noemancer::DapStreamDecoder decoder;
    std::uint64_t sequence=100U;
    char buffer[4096];
    for(;;) {
        int read_count{};
#ifdef _WIN32
        read_count=_read(_fileno(stdin),buffer,sizeof(buffer));
#else
        read_count=static_cast<int>(::read(STDIN_FILENO,buffer,sizeof(buffer)));
#endif
        if(read_count<=0) return 0;
        const auto decoded=decoder.feed(std::string_view(buffer,static_cast<std::size_t>(read_count)));
        if(decoded.error) return 2;
        for(const auto& raw:decoded.messages) {
            const auto request=Json::parse(raw);
            const auto request_sequence=request.at("seq").get<std::uint64_t>();
            const auto command=request.at("command").get<std::string>();
            const auto response=[&](Json body=Json::object(),const bool success=true) {
                send_message(Json{{"seq",sequence++},{"type","response"},{"request_seq",request_sequence},
                    {"success",success},{"command",command},{"body",std::move(body)}});
            };
            if(command=="initialize") {
                response(Json{{"supportsConfigurationDoneRequest",true},{"supportsTerminateRequest",true}});
                send_message(Json{{"seq",sequence++},{"type","event"},{"event","initialized"},
                    {"body",Json::object()}});
            } else if(command=="launch") response(Json::object());
            else if(command=="configurationDone") response(Json::object());
            else if(command=="setBreakpoints") response(Json{{"breakpoints",Json::array({Json{{"verified",true},{"line",7}}})}});
            else if(command=="continue") {
                response(Json{{"allThreadsContinued",true}});
                send_message(Json{{"seq",sequence++},{"type","event"},{"event","continued"},
                    {"body",Json{{"threadId",1},{"allThreadsContinued",true}}}});
            } else if(command=="pause") {
                response(Json::object());
                send_message(Json{{"seq",sequence++},{"type","event"},{"event","stopped"},
                    {"body",Json{{"reason","pause"},{"threadId",1},{"allThreadsStopped",true}}}});
            } else if(command=="next"||command=="stepIn") {
                response(Json::object());
                send_message(Json{{"seq",sequence++},{"type","event"},{"event","stopped"},
                    {"body",Json{{"reason","step"},{"threadId",1},{"allThreadsStopped",true}}}});
            } else if(command=="threads") {
                response(Json{{"threads",Json::array({Json{{"id",1},{"name","fake-main"}}})}});
            } else if(command=="stackTrace") {
                response(Json{{"stackFrames",Json::array({Json{{"id",11},{"name","main"},
                    {"line",7},{"column",1},{"source",Json{{"path","fake.cs"}}}}})}});
            } else if(command=="hang") {
                // Keep the session alive without answering. The parent test
                // must receive a deterministic request-timeout and still be
                // able to send terminate afterwards.
            } else if(command=="terminate") {
                response(Json::object());
                send_message(Json{{"seq",sequence++},{"type","event"},{"event","terminated"},
                    {"body",Json::object()}});
                return 0;
            } else if(command=="disconnect") {
                response(Json::object());
                send_message(Json{{"seq",sequence++},{"type","event"},{"event","terminated"},
                    {"body",Json{{"reason","disconnect"}}}});
                return 0;
            } else response(Json::object());
        }
    }
}

bool has_event(const std::vector<noemancer::DapSessionMessage>& events,const std::string_view name) {
    return std::ranges::any_of(events,[name](const auto& event) {return event.event==name;});
}

bool wait_for_event(noemancer::ManagedDebugSession& session,const std::string_view name) {
    // Process startup and pipe-reader scheduling can exceed one second while
    // the full MSVC test graph is under load. Keep the protocol request's
    // dedicated 100 ms timeout strict, but give asynchronous adapter events
    // the same bounded startup budget used by the process exchange itself.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {
        if(has_event(session.drain_events(),name)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool wait_for_process_exit(noemancer::ManagedDebugSession& session) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {
        if(!session.active()&&session.exit_code()!=-1) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool expect_success(const noemancer::DapSessionReply& reply,const std::string_view command) {
    if(!reply.sent||!reply.received||!reply.success||reply.command!=command) {
        std::cerr<<"DAP command failed: "<<command<<" error="<<reply.error<<" message="<<reply.message<<'\n';
        return false;
    }
    return true;
}

} // namespace

int main(const int argc,char** argv) {
    if(argc==2&&std::string_view(argv[1])=="--fake-long-lived") return run_long_lived_fake_adapter();
    if(argc!=2) return 1;
    const std::filesystem::path adapter=argv[1];

    // Keep the original one-shot API covered for callers that have not moved
    // to a long-lived session yet.
    const auto requests=noemancer::dap_request_frame(1,"initialize",R"({"clientID":"noemancer"})")+
        noemancer::dap_request_frame(2,"attach",R"({"processId":42})");
    const auto exchange=noemancer::run_dap_process_exchange(adapter,{},requests,std::chrono::seconds(5));
    if(!exchange.started||exchange.timed_out||exchange.exit_code!=0||!exchange.error.empty()||
       exchange.stderr_text.find("fake-adapter-ready")==std::string::npos) return 2;
    noemancer::DapStreamDecoder decoder;
    const auto messages=decoder.feed(exchange.stdout_bytes);
    if(messages.error||messages.messages.size()!=2) {
        std::cerr<<"stdout bytes="<<exchange.stdout_bytes.size()<<" stderr="<<exchange.stderr_text
            <<" decode="<<(messages.error?*messages.error:"none")<<" messages="<<messages.messages.size()<<'\n';return 3;
    }
    const auto initialize=Json::parse(messages.messages[0]);
    const auto attach=Json::parse(messages.messages[1]);
    if(initialize.at("request_seq")!=1||initialize.at("command")!="initialize"||!initialize.at("success")||
       attach.at("request_seq")!=2||attach.at("command")!="attach"||!attach.at("body").at("fake")) return 4;
    const auto timeout=noemancer::run_dap_process_exchange(adapter,{"--hang"},"",std::chrono::milliseconds(100));
    if(!timeout.started||!timeout.timed_out||timeout.exit_code!=124) return 5;

    const auto self=std::filesystem::absolute(argv[0]);
    noemancer::ManagedDebugSession session;
    if(!session.start(self,{"--fake-long-lived"},std::chrono::seconds(2))) return 6;
    if(!expect_success(session.initialize(),"initialize")) return 7;
    if(!wait_for_event(session,"initialized")) return 8;
    if(!expect_success(session.launch(R"({"program":"fake-game"})"),"launch")) return 9;
    if(!expect_success(session.configuration_done(),"configurationDone")) return 10;
    if(!expect_success(session.set_breakpoints(R"({"source":{"path":"fake.cs"},"breakpoints":[{"line":7}]})"),"setBreakpoints")) return 11;
    if(!expect_success(session.continue_execution(1U),"continue")) return 12;
    if(!wait_for_event(session,"continued")) return 13;
    if(Json::parse(session.state_json()).at("state")!="running") return 14;
    if(!expect_success(session.pause(1U),"pause")) return 15;
    if(!wait_for_event(session,"stopped")) return 16;
    if(Json::parse(session.state_json()).at("state")!="paused") return 17;
    const auto step=session.request("next",R"({"threadId":1,"singleThread":true})");
    if(!expect_success(step,"next")) return 18;
    if(!wait_for_event(session,"stopped")) return 19;
    const auto threads=session.threads();
    if(!expect_success(threads,"threads")||Json::parse(threads.body_json).at("threads").size()!=1U) return 20;
    const auto stack=session.stack_trace(1U);
    if(!expect_success(stack,"stackTrace")||Json::parse(stack.body_json).at("stackFrames").size()!=1U) return 21;
    const auto timed_out=session.request("hang","{}",std::chrono::milliseconds(100));
    if(!timed_out.sent||!timed_out.timed_out||timed_out.error!="dap.request-timeout") return 22;
    const auto terminated=session.terminate(std::chrono::seconds(2));
    if(!terminated.received||!terminated.success||session.active()||session.state()!=noemancer::ManagedDebugSessionState::terminated) return 23;
    if(!wait_for_event(session,"terminated")) return 24;
    const auto state=Json::parse(session.state_json());
    if(state.at("state")!="terminated"||state.at("active")||state.at("lastError")!="dap.request-timeout"||
       state.at("queuedEvents")!=0U||state.at("requestsSent")!=11U||state.at("nextSequence")!=12U) return 25;

    // Disconnect is a distinct editor lifecycle path from terminate: the
    // adapter acknowledges it, publishes a terminal event, and exits cleanly.
    noemancer::ManagedDebugSession disconnect_session;
    if(!disconnect_session.start(self,{"--fake-long-lived"},std::chrono::seconds(2))) return 26;
    if(!expect_success(disconnect_session.initialize(),"initialize")) return 27;
    if(!wait_for_event(disconnect_session,"initialized")) return 28;
    if(!expect_success(disconnect_session.launch(R"({"program":"fake-game"})"),"launch")) return 29;
    if(!expect_success(disconnect_session.configuration_done(),"configurationDone")) return 30;
    const auto disconnected=disconnect_session.request("disconnect",R"({"restart":false,"terminateDebuggee":false})");
    if(!expect_success(disconnected,"disconnect")) return 31;
    if(!wait_for_event(disconnect_session,"terminated")) return 32;
    // The terminal DAP event and the OS process-exit notification are
    // intentionally observed by different reader/wait threads. Do not treat
    // their legal ordering as a transport failure.
    if(!wait_for_process_exit(disconnect_session)) return 34;
    const auto disconnect_state=Json::parse(disconnect_session.state_json());
    if(disconnect_session.active()||disconnect_session.state()!=noemancer::ManagedDebugSessionState::terminated||
       disconnect_state.at("state")!="terminated"||disconnect_state.at("active")||
       disconnect_state.at("requestsSent")!=4U||disconnect_state.at("queuedEvents")!=0U||
       !disconnect_state.at("lastError").get<std::string>().empty()||disconnect_state.at("exitCode")!=0) return 33;

    std::cout<<"Long-lived DAP session exchanged correlated requests, step, bounded state, timeout, disconnect and termination\n";
    return 0;
}
