#include "engine/process_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#else
#include <unistd.h>
#endif

namespace noemancer {
namespace {

std::string process_role{"unknown"};
std::string report_path{};
std::string minidump_path{};
#if defined(_WIN32)
std::wstring minidump_path_wide{};
#endif

unsigned long current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::string sanitized_component(const std::string_view value) {
    std::string output;
    output.reserve(std::min<std::size_t>(value.size(), 64));
    for (const unsigned char character : value) {
        if (output.size() >= 64) {
            break;
        }
        if (std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.') {
            output.push_back(static_cast<char>(character));
        } else {
            output.push_back('_');
        }
    }
    return output.empty() ? std::string{"unknown"} : output;
}

void write_json_string(std::FILE* stream, const std::string_view value) noexcept {
    if (stream == nullptr) {
        return;
    }
    std::fputc('"', stream);
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': std::fputs("\\\\", stream); break;
        case '"': std::fputs("\\\"", stream); break;
        case '\n': std::fputs("\\n", stream); break;
        case '\r': std::fputs("\\r", stream); break;
        case '\t': std::fputs("\\t", stream); break;
        default:
            if (character < 0x20U) {
                std::fputs("\\u00", stream);
                std::fputc(hex[(character >> 4U) & 0x0FU], stream);
                std::fputc(hex[character & 0x0FU], stream);
            } else {
                std::fputc(character, stream);
            }
            break;
        }
    }
    std::fputc('"', stream);
}

void write_record(
    std::FILE* stream,
    const std::string_view event,
    const std::string_view detail,
    const int exit_code,
    const unsigned long exception_code = 0UL,
    const void* exception_address = nullptr) noexcept {
    if (stream == nullptr) {
        return;
    }
    std::fputs("{\"schema\":\"noemancer.process-diagnostics.v1\",\"level\":\"fatal\",\"event\":", stream);
    write_json_string(stream, event);
    std::fputs(",\"role\":", stream);
    write_json_string(stream, process_role);
    std::fprintf(stream, ",\"processId\":%lu,\"exitCode\":%d,\"detail\":", current_process_id(), exit_code);
    write_json_string(stream, detail);
    if(!minidump_path.empty()) {
        std::fputs(",\"minidumpPath\":",stream);
        write_json_string(stream,minidump_path);
    }
    if (exception_code != 0UL || exception_address != nullptr) {
        std::fprintf(stream, ",\"exceptionCode\":\"0x%08lX\",\"address\":\"%p\"", exception_code, exception_address);
    }
    std::fputs("}\n", stream);
    std::fflush(stream);
}

#if defined(_WIN32)
void write_minidump(void* exception_context) noexcept {
    if(minidump_path_wide.empty())return;
    const HANDLE file=CreateFileW(minidump_path_wide.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    MINIDUMP_EXCEPTION_INFORMATION information{
        GetCurrentThreadId(),static_cast<EXCEPTION_POINTERS*>(exception_context),FALSE};
    const auto type=static_cast<MINIDUMP_TYPE>(MiniDumpNormal|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules);
    const BOOL written=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file,type,
        exception_context==nullptr?nullptr:&information,nullptr,nullptr);
    CloseHandle(file);
    if(written==FALSE)DeleteFileW(minidump_path_wide.c_str());
}
#endif

std::FILE* open_report_file(const char* path) noexcept {
#if defined(_MSC_VER)
    std::FILE* report = nullptr;
    if (path == nullptr || ::fopen_s(&report, path, "wb") != 0) {
        return nullptr;
    }
    return report;
#else
    return std::fopen(path, "wb");
#endif
}

void write_fatal_evidence(
    const std::string_view event,
    const std::string_view detail,
    const int exit_code,
    const unsigned long exception_code = 0UL,
    const void* exception_address = nullptr,
    void* exception_context = nullptr) noexcept {
#if defined(_WIN32)
    write_minidump(exception_context);
#else
    static_cast<void>(exception_context);
#endif
    write_record(stderr, event, detail, exit_code, exception_code, exception_address);
    if (report_path.empty()) {
        return;
    }
    if (auto* report = open_report_file(report_path.c_str()); report != nullptr) {
        write_record(report, event, detail, exit_code, exception_code, exception_address);
        std::fclose(report);
    }
}

[[noreturn]] void terminate_with_evidence() noexcept {
    char detail_buffer[1024]{};
    const char* detail = "std::terminate called without an active standard exception";
    if (const auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            const char* what = error.what();
            if (what != nullptr) {
                std::size_t length = 0;
                while (length + 1 < sizeof(detail_buffer) && what[length] != '\0') {
                    detail_buffer[length] = what[length];
                    ++length;
                }
                detail_buffer[length] = '\0';
                detail = detail_buffer;
            }
        } catch (...) {
            detail = "std::terminate called for a non-standard exception";
        }
    }
    write_fatal_evidence("process.terminate", detail, 134);
    std::_Exit(134);
}

[[noreturn]] void abort_with_evidence(int) noexcept {
    write_fatal_evidence("process.abort", "SIGABRT/abort invoked", 134);
    std::_Exit(134);
}

#if defined(_WIN32)
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception) {
    const auto code = exception != nullptr && exception->ExceptionRecord != nullptr
        ? exception->ExceptionRecord->ExceptionCode
        : 0UL;
    const auto address = exception != nullptr && exception->ExceptionRecord != nullptr
        ? exception->ExceptionRecord->ExceptionAddress
        : nullptr;
    write_fatal_evidence("process.seh", "unhandled structured exception", 134, code, address,exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

#if defined(_MSC_VER) && defined(_DEBUG)
int __cdecl crt_report_without_dialog(
    const int report_type, char* message, int* return_value) noexcept {
    if (report_type == _CRT_WARN) {
        return FALSE;
    }

    const auto event = report_type == _CRT_ASSERT
        ? std::string_view{"process.crt-assert"}
        : std::string_view{"process.crt-error"};
    write_record(
        stderr,
        event,
        message != nullptr ? std::string_view{message} : std::string_view{"CRT report without detail"},
        134);
    if (return_value != nullptr) {
        // Returning zero tells the debug CRT not to break into a debugger.  The
        // abort/signal/invalid-parameter handlers below still terminate with
        // structured evidence when the originating failure is fatal.
        *return_value = 0;
    }
    return TRUE;
}

[[noreturn]] void invalid_parameter_with_evidence(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) noexcept {
    write_fatal_evidence("process.invalid-parameter", "invalid CRT parameter", 134);
    std::_Exit(134);
}

[[noreturn]] void __cdecl purecall_with_evidence() noexcept {
    write_fatal_evidence("process.purecall", "pure virtual call", 134);
    std::_Exit(134);
}
#endif
#endif

} // namespace

void configure_process_diagnostics(const std::string_view role) {
    configure_process_diagnostics(process_diagnostics_options{role, {}});
}

void configure_process_diagnostics(const process_diagnostics_options& options) {
    process_role = options.process_role.empty() ? "unknown" : std::string{options.process_role};
    report_path.clear();
    minidump_path.clear();
#if defined(_WIN32)
    minidump_path_wide.clear();
#endif
    if (!options.report_directory.empty()) {
        try {
            const auto directory = std::filesystem::path{std::string{options.report_directory}};
            std::filesystem::create_directories(directory);
            const auto stem="noemancer-"+sanitized_component(process_role)+"-"+
                std::to_string(current_process_id());
            report_path=(directory/(stem+".fatal.json")).string();
#if defined(_WIN32)
            const auto minidump_file=directory/(stem+".dmp");
            minidump_path=minidump_file.string();
            minidump_path_wide=minidump_file.wstring();
#endif
        } catch (...) {
            // Diagnostics configuration must never turn a process startup
            // failure into a second exception.  stderr evidence remains live.
            report_path.clear();
            minidump_path.clear();
#if defined(_WIN32)
            minidump_path_wide.clear();
#endif
        }
    }
    std::set_terminate(terminate_with_evidence);
    std::signal(SIGABRT, abort_with_evidence);
#if defined(_WIN32)
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(unhandled_exception_filter);
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, crt_report_without_dialog);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(invalid_parameter_with_evidence);
    _set_purecall_handler(purecall_with_evidence);
#endif
#endif
}

} // namespace noemancer
