#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace {

// CTest owns the process output stream.  Configure every test executable before
// main() so a failing Debug CRT assertion is written to that stream instead of
// opening a modal dialog on the developer's desktop.  Tests which opt into the
// richer Noemancer diagnostics layer replace these defaults from main().
struct TestProcessGuard final {
    TestProcessGuard() noexcept {
        SetErrorMode(GetErrorMode() |
            SEM_FAILCRITICALERRORS |
            SEM_NOGPFAULTERRORBOX |
            SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER) && defined(_DEBUG)
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    }
};

const TestProcessGuard test_process_guard{};

} // namespace
#endif
