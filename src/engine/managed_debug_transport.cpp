#include "engine/managed_debug_transport.hpp"

#include "engine/managed_debug_protocol.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace noemancer {
namespace {
constexpr std::size_t maximum_stream_bytes=2U*1024U*1024U;

#ifdef _WIN32
std::wstring quote_argument(const std::wstring& value) {
    if(value.find_first_of(L" \t\"")==std::wstring::npos)return value;
    std::wstring result{L'\"'};
    std::size_t slashes{};
    for(const auto character:value) {
        if(character==L'\\'){++slashes;continue;}
        if(character==L'\"'){result.append(slashes*2+1,L'\\');result.push_back(character);slashes=0;continue;}
        result.append(slashes,L'\\');slashes=0;result.push_back(character);
    }
    result.append(slashes*2,L'\\');result.push_back(L'\"');return result;
}

void close_handle(HANDLE& handle) {if(handle!=nullptr&&handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=nullptr;}}

void read_bounded(HANDLE handle,std::string& output) {
    char buffer[4096];
    DWORD read{};
    while(ReadFile(handle,buffer,sizeof(buffer),&read,nullptr)&&read>0) {
        const auto available=maximum_stream_bytes-output.size();
        output.append(buffer,std::min<std::size_t>(read,available));
    }
    close_handle(handle);
}
#endif
}

DapProcessExchange run_dap_process_exchange(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,const std::string& stdin_bytes,const std::chrono::milliseconds timeout) {
    DapProcessExchange result;
#ifdef _WIN32
    if(timeout<=std::chrono::milliseconds::zero()||stdin_bytes.size()>maximum_stream_bytes||!std::filesystem::is_regular_file(executable)) {
        result.error="dap.invalid-process-request";return result;
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE stdin_read{},stdin_write{},stdout_read{},stdout_write{},stderr_read{},stderr_write{};
    const auto pipes_ready=CreatePipe(&stdin_read,&stdin_write,&security,0)&&CreatePipe(&stdout_read,&stdout_write,&security,0)&&
        CreatePipe(&stderr_read,&stderr_write,&security,0)&&SetHandleInformation(stdin_write,HANDLE_FLAG_INHERIT,0)&&
        SetHandleInformation(stdout_read,HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(stderr_read,HANDLE_FLAG_INHERIT,0);
    if(!pipes_ready) {
        close_handle(stdin_read);close_handle(stdin_write);close_handle(stdout_read);close_handle(stdout_write);
        close_handle(stderr_read);close_handle(stderr_write);result.error="dap.pipe-create-failed";return result;
    }
    std::wstring command_line=quote_argument(executable.wstring());
    for(const auto& argument:arguments)command_line+=L" "+quote_argument(std::wstring(argument.begin(),argument.end()));
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=stdin_read;startup.hStdOutput=stdout_write;startup.hStdError=stderr_write;
    PROCESS_INFORMATION process{};
    const auto working_directory=executable.parent_path().wstring();
    const auto created=CreateProcessW(nullptr,command_line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,
        working_directory.empty()?nullptr:working_directory.c_str(),&startup,&process);
    close_handle(stdin_read);close_handle(stdout_write);close_handle(stderr_write);
    if(!created) {
        close_handle(stdin_write);close_handle(stdout_read);close_handle(stderr_read);result.error="dap.process-start-failed";return result;
    }
    result.started=true;
    std::thread stdout_thread(read_bounded,stdout_read,std::ref(result.stdout_bytes));
    std::thread stderr_thread(read_bounded,stderr_read,std::ref(result.stderr_text));
    std::size_t written_total{};
    while(written_total<stdin_bytes.size()) {
        DWORD written{};
        const auto chunk=static_cast<DWORD>(std::min<std::size_t>(stdin_bytes.size()-written_total,64U*1024U));
        if(!WriteFile(stdin_write,stdin_bytes.data()+written_total,chunk,&written,nullptr)||written==0)break;
        written_total+=written;
    }
    close_handle(stdin_write);
    const auto wait=WaitForSingleObject(process.hProcess,static_cast<DWORD>(std::min<long long>(timeout.count(),INFINITE-1LL)));
    if(wait==WAIT_TIMEOUT){result.timed_out=true;TerminateProcess(process.hProcess,124);WaitForSingleObject(process.hProcess,5000);}
    DWORD exit_code{};if(GetExitCodeProcess(process.hProcess,&exit_code))result.exit_code=static_cast<int>(exit_code);
    close_handle(process.hThread);close_handle(process.hProcess);
    stdout_thread.join();stderr_thread.join();
    if(written_total!=stdin_bytes.size()&&!result.timed_out)result.error="dap.stdin-write-failed";
#else
    static_cast<void>(executable);static_cast<void>(arguments);static_cast<void>(stdin_bytes);static_cast<void>(timeout);
    result.error="dap.process-platform-pending";
#endif
    return result;
}

const char* managed_debug_session_state_name(const ManagedDebugSessionState state) noexcept {
    switch(state) {
    case ManagedDebugSessionState::created: return "created";
    case ManagedDebugSessionState::starting: return "starting";
    case ManagedDebugSessionState::ready: return "ready";
    case ManagedDebugSessionState::running: return "running";
    case ManagedDebugSessionState::paused: return "paused";
    case ManagedDebugSessionState::terminating: return "terminating";
    case ManagedDebugSessionState::terminated: return "terminated";
    case ManagedDebugSessionState::failed: return "failed";
    }
    return "failed";
}

struct ManagedDebugSession::Impl final {
    using Json=nlohmann::json;

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::mutex request_mutex;
    ManagedDebugSessionState session_state{ManagedDebugSessionState::created};
    bool active_process{};
    bool stdout_eof{};
    bool process_exited{};
    bool expected_shutdown{};
    int process_exit_code{-1};
    std::uint32_t process_id{};
    std::uint64_t next_sequence{1U};
    std::size_t requests_sent{};
    std::chrono::milliseconds request_timeout{std::chrono::seconds(5)};
    std::string error;
    std::string stderr_text;
    std::deque<DapSessionMessage> events;
    std::unordered_map<std::uint64_t,DapSessionReply> responses;
    DapStreamDecoder decoder;

#ifdef _WIN32
    HANDLE process_handle{};
    HANDLE process_thread_handle{};
    HANDLE stdin_write{};
    HANDLE stdout_read{};
    HANDLE stderr_read{};
    std::thread stdout_thread;
    std::thread stderr_thread;
    std::thread process_thread;
#endif

    void set_error_locked(const std::string_view value) {
        if(error.empty()) error=value;
        if(session_state!=ManagedDebugSessionState::terminating&&session_state!=ManagedDebugSessionState::terminated)
            session_state=ManagedDebugSessionState::failed;
        condition.notify_all();
    }

    void observe_message_locked(const DapSessionMessage& message) {
        if(message.kind==DapSessionMessageKind::event) {
            events.push_back(message);
            if(message.event=="initialized") session_state=ManagedDebugSessionState::ready;
            else if(message.event=="stopped") session_state=ManagedDebugSessionState::paused;
            else if(message.event=="continued") session_state=ManagedDebugSessionState::running;
            else if(message.event=="terminated"||message.event=="exited") session_state=ManagedDebugSessionState::terminated;
            condition.notify_all();
        } else if(message.kind==DapSessionMessageKind::request) {
            events.push_back(message);
            condition.notify_all();
        }
    }

    static std::optional<DapSessionMessage> decode_message(const std::string& raw) {
        const auto message=Json::parse(raw,nullptr,false);
        if(message.is_discarded()||!message.is_object()) return std::nullopt;
        const auto sequence_value=[&](const std::string_view key) {
            const auto found=message.find(std::string(key));if(found==message.end())return std::uint64_t{};
            if(found->is_number_unsigned())return found->get<std::uint64_t>();
            if(found->is_number_integer()){const auto value=found->get<std::int64_t>();return value>0?static_cast<std::uint64_t>(value):0U;}
            if(found->is_string()){const auto& text=found->get_ref<const std::string&>();std::uint64_t value{};
                const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
                if(error==std::errc{}&&end==text.data()+text.size())return value;}
            return std::uint64_t{};
        };
        const auto type=message.value("type",std::string{});
        DapSessionMessage result;
        result.message_json=raw;
        result.sequence=sequence_value("seq");
        if(type=="response") {
            result.kind=DapSessionMessageKind::response;
            result.request_sequence=sequence_value("request_seq");
            result.command=message.value("command",std::string{});
            if(message.contains("body")) result.body_json=message.at("body").dump();
        } else if(type=="event") {
            result.kind=DapSessionMessageKind::event;
            result.event=message.value("event",std::string{});
            if(message.contains("body")) result.body_json=message.at("body").dump();
        } else if(type=="request") {
            result.kind=DapSessionMessageKind::request;
            result.command=message.value("command",std::string{});
            result.request_sequence=message.value("seq",0ULL);
            if(message.contains("arguments")) result.body_json=message.at("arguments").dump();
        } else {
            return std::nullopt;
        }
        if(result.sequence==0U||(result.kind==DapSessionMessageKind::response&&result.request_sequence==0U))
            return std::nullopt;
        return result;
    }

    void accept_bytes(const std::string_view bytes) {
        const auto decoded=decoder.feed(bytes);
        std::lock_guard lock(mutex);
        if(decoded.error) {
            set_error_locked("dap.protocol-error");
            return;
        }
        for(const auto& raw:decoded.messages) {
            const auto message=decode_message(raw);
            if(!message) {
                set_error_locked("dap.protocol-error");
                return;
            }
            if(message->kind==DapSessionMessageKind::response) {
                const auto parsed=Json::parse(message->message_json,nullptr,false);
                DapSessionReply reply;
                reply.sent=true;
                reply.received=true;
                reply.success=parsed.value("success",false);
                reply.sequence=message->request_sequence;
                reply.exit_code=process_exit_code;
                reply.command=message->command;
                reply.body_json=message->body_json;
                reply.message=parsed.value("message",std::string{});
                reply.response_json=message->message_json;
                if(!reply.success) reply.error="dap.request-rejected";
                responses.insert_or_assign(message->request_sequence,std::move(reply));
                condition.notify_all();
            } else {
                observe_message_locked(*message);
            }
        }
    }

#ifdef _WIN32
    void stdout_loop(HANDLE handle) {
        char buffer[16U*1024U];
        DWORD read{};
        while(ReadFile(handle,buffer,sizeof(buffer),&read,nullptr)&&read>0U) {
            try {accept_bytes(std::string_view(buffer,read));}
            catch(const std::exception& exception) {
                std::lock_guard lock(mutex);
                const auto detail=std::string{"transport exception: "}+exception.what()+"\n";
                stderr_text.append(detail,0,std::min(detail.size(),maximum_stream_bytes-stderr_text.size()));
                set_error_locked("dap.protocol-exception");break;
            } catch(...) {std::lock_guard lock(mutex);set_error_locked("dap.protocol-exception");break;}
            std::lock_guard lock(mutex);
            if(error=="dap.protocol-error"||error=="dap.protocol-exception") break;
        }
        close_handle(handle);
        std::lock_guard lock(mutex);
        if(stdout_read==handle) stdout_read=nullptr;
        stdout_eof=true;
        condition.notify_all();
    }

    void stderr_loop(HANDLE handle) {
        char buffer[4096];
        DWORD read{};
        while(ReadFile(handle,buffer,sizeof(buffer),&read,nullptr)&&read>0U) {
            std::lock_guard lock(mutex);
            const auto available=maximum_stream_bytes-stderr_text.size();
            stderr_text.append(buffer,std::min<std::size_t>(read,available));
            if(stderr_text.size()>=maximum_stream_bytes) break;
        }
        close_handle(handle);
        std::lock_guard lock(mutex);
        if(stderr_read==handle) stderr_read=nullptr;
    }

    void process_wait_loop(const HANDLE handle) {
        WaitForSingleObject(handle,INFINITE);
        DWORD code{};
        GetExitCodeProcess(handle,&code);
        std::lock_guard lock(mutex);
        process_exited=true;
        process_exit_code=static_cast<int>(code);
        active_process=false;
        if(code!=0U&&!expected_shutdown) set_error_locked("dap.process-nonzero");
        else if(session_state!=ManagedDebugSessionState::terminating&&session_state!=ManagedDebugSessionState::terminated)
            set_error_locked(stdout_eof?"dap.process-eof":"dap.process-exit");
        else if(session_state==ManagedDebugSessionState::terminating) session_state=ManagedDebugSessionState::terminated;
        condition.notify_all();
    }
#endif

    void clear_process_handles() {
#ifdef _WIN32
        close_handle(stdin_write);
        close_handle(stdout_read);
        close_handle(stderr_read);
        close_handle(process_thread_handle);
        close_handle(process_handle);
#endif
    }
};

ManagedDebugSession::ManagedDebugSession():impl_(std::make_unique<Impl>()) {}

ManagedDebugSession::~ManagedDebugSession() { shutdown(); }

bool ManagedDebugSession::start(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,const std::chrono::milliseconds startup_timeout) {
    if(!impl_) impl_=std::make_unique<Impl>();
    if(startup_timeout<=std::chrono::milliseconds::zero()||!std::filesystem::is_regular_file(executable)) {
        std::lock_guard lock(impl_->mutex);
        impl_->error="dap.invalid-process-request";
        impl_->session_state=ManagedDebugSessionState::failed;
        return false;
    }
    bool needs_cleanup{};
    {
        std::lock_guard lock(impl_->mutex);
        if(impl_->active_process) {impl_->error="dap.session-already-active";return false;}
#ifdef _WIN32
        needs_cleanup=impl_->stdout_thread.joinable()||impl_->stderr_thread.joinable()||impl_->process_thread.joinable();
#endif
    }
    if(needs_cleanup) shutdown();
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE stdin_read{},stdin_write{},stdout_read{},stdout_write{},stderr_read{},stderr_write{};
    const auto pipes_ready=CreatePipe(&stdin_read,&stdin_write,&security,0)&&CreatePipe(&stdout_read,&stdout_write,&security,0)&&
        CreatePipe(&stderr_read,&stderr_write,&security,0)&&SetHandleInformation(stdin_write,HANDLE_FLAG_INHERIT,0)&&
        SetHandleInformation(stdout_read,HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(stderr_read,HANDLE_FLAG_INHERIT,0);
    if(!pipes_ready) {
        close_handle(stdin_read);close_handle(stdin_write);close_handle(stdout_read);close_handle(stdout_write);
        close_handle(stderr_read);close_handle(stderr_write);
        std::lock_guard lock(impl_->mutex);impl_->error="dap.pipe-create-failed";impl_->session_state=ManagedDebugSessionState::failed;return false;
    }
    std::wstring command_line=quote_argument(executable.wstring());
    for(const auto& argument:arguments) command_line+=L" "+quote_argument(std::wstring(argument.begin(),argument.end()));
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=stdin_read;startup.hStdOutput=stdout_write;startup.hStdError=stderr_write;
    PROCESS_INFORMATION process{};
    const auto working_directory=executable.parent_path().wstring();
    const auto created=CreateProcessW(nullptr,command_line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,
        working_directory.empty()?nullptr:working_directory.c_str(),&startup,&process);
    close_handle(stdin_read);close_handle(stdout_write);close_handle(stderr_write);
    if(!created) {
        close_handle(stdin_write);close_handle(stdout_read);close_handle(stderr_read);
        std::lock_guard lock(impl_->mutex);impl_->error="dap.process-start-failed";impl_->session_state=ManagedDebugSessionState::failed;return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->session_state=ManagedDebugSessionState::ready;
        impl_->active_process=true;
        impl_->stdout_eof=false;
        impl_->process_exited=false;
        impl_->expected_shutdown=false;
        impl_->process_exit_code=-1;
        impl_->process_id=process.dwProcessId;
        impl_->next_sequence=1U;
        impl_->requests_sent=0U;
        impl_->error.clear();
        impl_->stderr_text.clear();
        impl_->events.clear();
        impl_->responses.clear();
        impl_->decoder.reset();
        impl_->request_timeout=startup_timeout;
        impl_->process_handle=process.hProcess;
        impl_->process_thread_handle=process.hThread;
        impl_->stdin_write=stdin_write;
        impl_->stdout_read=stdout_read;
        impl_->stderr_read=stderr_read;
    }
    impl_->stdout_thread=std::thread([impl=impl_.get(),handle=stdout_read]{impl->stdout_loop(handle);});
    impl_->stderr_thread=std::thread([impl=impl_.get(),handle=stderr_read]{impl->stderr_loop(handle);});
    impl_->process_thread=std::thread([impl=impl_.get(),handle=process.hProcess]{impl->process_wait_loop(handle);});
    return true;
#else
    static_cast<void>(executable);static_cast<void>(arguments);
    std::lock_guard lock(impl_->mutex);
    impl_->error="dap.process-platform-pending";
    impl_->session_state=ManagedDebugSessionState::failed;
    return false;
#endif
}

void ManagedDebugSession::shutdown(const std::chrono::milliseconds timeout) {
    if(!impl_) return;
    std::lock_guard request_lock(impl_->request_mutex);
#ifdef _WIN32
    HANDLE process_handle{};
    {
        std::lock_guard lock(impl_->mutex);
        if(!impl_->active_process&& !impl_->stdout_thread.joinable()&& !impl_->stderr_thread.joinable()&& !impl_->process_thread.joinable()) {
            if(impl_->session_state==ManagedDebugSessionState::created) impl_->session_state=ManagedDebugSessionState::terminated;
            return;
        }
        impl_->expected_shutdown=true;
        impl_->session_state=ManagedDebugSessionState::terminating;
        process_handle=impl_->process_handle;
        close_handle(impl_->stdin_write);
    }
    if(process_handle!=nullptr) {
        const auto bounded=static_cast<DWORD>(std::clamp<long long>(timeout.count(),0LL,INFINITE-1LL));
        if(WaitForSingleObject(process_handle,bounded)==WAIT_TIMEOUT) {
            TerminateProcess(process_handle,124U);
            WaitForSingleObject(process_handle,5000U);
        }
    }
    if(impl_->process_thread.joinable()) impl_->process_thread.join();
    if(impl_->stdout_thread.joinable()) impl_->stdout_thread.join();
    if(impl_->stderr_thread.joinable()) impl_->stderr_thread.join();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->active_process=false;
        impl_->session_state=ManagedDebugSessionState::terminated;
        impl_->clear_process_handles();
        impl_->condition.notify_all();
    }
#else
    static_cast<void>(timeout);
    std::lock_guard lock(impl_->mutex);
    impl_->session_state=ManagedDebugSessionState::terminated;
#endif
}

bool ManagedDebugSession::active() const noexcept {
    if(!impl_) return false;
    std::lock_guard lock(impl_->mutex);return impl_->active_process;
}

ManagedDebugSessionState ManagedDebugSession::state() const noexcept {
    if(!impl_) return ManagedDebugSessionState::created;
    std::lock_guard lock(impl_->mutex);return impl_->session_state;
}

std::string ManagedDebugSession::last_error() const {
    if(!impl_) return "dap.session-unavailable";
    std::lock_guard lock(impl_->mutex);return impl_->error;
}

int ManagedDebugSession::exit_code() const noexcept {
    if(!impl_) return -1;
    std::lock_guard lock(impl_->mutex);return impl_->process_exit_code;
}

std::string ManagedDebugSession::state_json() const {
    using Json=nlohmann::json;
    if(!impl_) return Json{{"schemaVersion","noemancer.managed-debug-session/0.1"},{"state","created"}}.dump();
    std::lock_guard lock(impl_->mutex);
    return Json{{"schemaVersion","noemancer.managed-debug-session/0.1"},
        {"state",managed_debug_session_state_name(impl_->session_state)},
        {"active",impl_->active_process},{"processId",impl_->process_id},
        {"nextSequence",impl_->next_sequence},{"requestsSent",impl_->requests_sent},
        {"queuedEvents",impl_->events.size()},{"exitCode",impl_->process_exit_code},
        {"lastError",impl_->error},{"stderr",impl_->stderr_text}}.dump();
}

DapSessionReply ManagedDebugSession::request(const std::string_view command,const std::string_view arguments_json,
    const std::chrono::milliseconds timeout) {
    DapSessionReply result;result.command=std::string(command);
    if(!impl_) {result.error="dap.session-unavailable";return result;}
    std::lock_guard request_lock(impl_->request_mutex);
    const auto effective_timeout=timeout>std::chrono::milliseconds::zero()?timeout:impl_->request_timeout;
    if(effective_timeout<=std::chrono::milliseconds::zero()) {result.error="dap.invalid-timeout";return result;}
    {
        std::lock_guard lock(impl_->mutex);
        if(!impl_->active_process) {result.error=impl_->error.empty()?"dap.session-not-active":impl_->error;return result;}
        result.sequence=impl_->next_sequence++;
        ++impl_->requests_sent;
    }
    std::string frame;
    try {frame=dap_request_frame(result.sequence,command,arguments_json);}
    catch(const std::exception&) {
        std::lock_guard lock(impl_->mutex);result.error="dap.invalid-request";impl_->error=result.error;return result;
    }
#ifdef _WIN32
    std::size_t written_total{};
    while(written_total<frame.size()) {
        DWORD written{};
        const auto chunk=static_cast<DWORD>(std::min<std::size_t>(frame.size()-written_total,64U*1024U));
        HANDLE stdin_handle{};
        {std::lock_guard lock(impl_->mutex);stdin_handle=impl_->stdin_write;}
        if(stdin_handle==nullptr||!WriteFile(stdin_handle,frame.data()+written_total,chunk,&written,nullptr)||written==0U) {
            std::lock_guard lock(impl_->mutex);result.error="dap.stdin-write-failed";impl_->set_error_locked(result.error);return result;
        }
        written_total+=written;
    }
    result.sent=written_total==frame.size();
    std::unique_lock lock(impl_->mutex);
    const auto deadline=std::chrono::steady_clock::now()+effective_timeout;
    while(true) {
        if(const auto found=impl_->responses.find(result.sequence);found!=impl_->responses.end()) {
            result=std::move(found->second);impl_->responses.erase(found);break;
        }
        if(impl_->error=="dap.protocol-error"||impl_->error=="dap.protocol-exception"||impl_->error=="dap.process-eof"||impl_->error=="dap.process-exit"||
           impl_->error=="dap.process-nonzero") {result.error=impl_->error;result.process_exited=impl_->process_exited;
            result.exit_code=impl_->process_exit_code;result.message=impl_->stderr_text.substr(0,4096U);break;}
        if(impl_->process_exited&&!impl_->active_process) {result.error=impl_->error.empty()?"dap.process-exit":impl_->error;result.process_exited=true;result.exit_code=impl_->process_exit_code;break;}
        if(impl_->condition.wait_until(lock,deadline)==std::cv_status::timeout) {
            result.timed_out=true;result.error="dap.request-timeout";impl_->error=result.error;break;
        }
    }
    if(result.received) {
        if(!result.success&&result.error.empty()) result.error="dap.request-rejected";
        if(result.command=="continue"||result.command=="next"||result.command=="stepIn"||result.command=="stepOut")
            impl_->session_state=ManagedDebugSessionState::running;
        else if(result.command=="pause") impl_->session_state=ManagedDebugSessionState::paused;
        else if(result.command=="terminate"||result.command=="disconnect") impl_->session_state=ManagedDebugSessionState::terminating;
        else if(result.command=="initialize"||result.command=="launch"||result.command=="attach"||result.command=="configurationDone")
            impl_->session_state=ManagedDebugSessionState::ready;
    }
    return result;
#else
    static_cast<void>(frame);result.error="dap.process-platform-pending";return result;
#endif
}

DapSessionReply ManagedDebugSession::initialize(const std::string_view arguments_json,const std::chrono::milliseconds timeout) {
    return request("initialize",arguments_json,timeout);
}

DapSessionReply ManagedDebugSession::launch(const std::string_view arguments_json,const std::chrono::milliseconds timeout) {
    return request("launch",arguments_json,timeout);
}

DapSessionReply ManagedDebugSession::configuration_done(const std::chrono::milliseconds timeout) {
    return request("configurationDone","{}",timeout);
}

DapSessionReply ManagedDebugSession::set_breakpoints(const std::string_view arguments_json,const std::chrono::milliseconds timeout) {
    return request("setBreakpoints",arguments_json,timeout);
}

DapSessionReply ManagedDebugSession::continue_execution(const std::uint64_t thread_id,const std::chrono::milliseconds timeout) {
    return request("continue",nlohmann::json{{"threadId",thread_id}}.dump(),timeout);
}

DapSessionReply ManagedDebugSession::pause(const std::uint64_t thread_id,const std::chrono::milliseconds timeout) {
    return request("pause",nlohmann::json{{"threadId",thread_id}}.dump(),timeout);
}

DapSessionReply ManagedDebugSession::threads(const std::chrono::milliseconds timeout) {
    return request("threads","{}",timeout);
}

DapSessionReply ManagedDebugSession::stack_trace(const std::uint64_t thread_id,const std::chrono::milliseconds timeout) {
    return request("stackTrace",nlohmann::json{{"threadId",thread_id}}.dump(),timeout);
}

DapSessionReply ManagedDebugSession::terminate(const std::chrono::milliseconds timeout) {
    const auto result=request("terminate",R"({"restart":false,"terminateDebuggee":true})",timeout);
    if(result.received||result.timed_out||!result.error.empty()) shutdown(timeout>std::chrono::milliseconds::zero()?timeout:std::chrono::seconds(2));
    return result;
}

std::vector<DapSessionMessage> ManagedDebugSession::drain_events() {
    std::vector<DapSessionMessage> result;
    if(!impl_) return result;
    std::lock_guard lock(impl_->mutex);
    result.reserve(impl_->events.size());
    while(!impl_->events.empty()) {result.push_back(std::move(impl_->events.front()));impl_->events.pop_front();}
    return result;
}

} // namespace noemancer
