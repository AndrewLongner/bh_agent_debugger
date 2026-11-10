#include "../bh_agent_debugger_service/bh_agent_debugger_service/bh_vs_profiler.h"
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char* text)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
__declspec(noinline) static void workload(DWORD milliseconds)
{
    const auto until = GetTickCount64() + milliseconds;
    volatile unsigned long long value = 1;
    while (GetTickCount64() < until)
        for (unsigned i = 0; i < 100000; ++i) value = value * 1664525 + 1013904223;
}
int main(int argc, char** argv)
{
    using namespace bh_vs_profiler;
    if (argc == 2 && std::string(argv[1]) == "--workload") { workload(120000); return 0; }
    detail::request request;
    check(detail::parse("profile start 123", request) && request.pid == 123 && request.seconds == 10, "default duration");
    check(detail::parse("profile start 123 60", request) && request.seconds == 60, "maximum duration");
    for (const auto* text : {"profile start 0", "profile start -1", "profile start 4294967296", "profile start 1 0", "profile start 1 61",
        "profile start 1 2 junk", "profile status junk", "profile report 1", "profile start +1", "profile start 1\n", "profile functions"})
        check(!detail::parse(text, request), "malformed/unsupported command");
    check(!detail::parse(std::string("profile status\0x", 16), request), "NUL rejection");
    check(detail::quote(L"a b") == L"\"a b\"", "space quoting");
    check(detail::quote(L"a\\") == L"\"a\\\\\"", "trailing backslash quoting");
    check(detail::quote(L"a\"b") == L"\"a\\\"b\"", "embedded quote quoting");
    // Caller-selected output validation (no capture started).
    {
        using namespace bh_vs_profiler::detail;
        destination d;
        for (const auto* bad : {L"relative\\x", L"\\\\server\\share\\x", L"\\\\?\\C:\\x", L"\\\\.\\C:\\x", L"C:x", L"C:/x",
            L"C:\\a\\..\\b", L"C:\\a\\.\\b", L"C:\\a.\\b", L"C:\\a \\b", L"C:\\a\\NUL", L"C:\\a\\b:stream", L"C:\\a\\\\b", L"C:\\a*", L"C:\\.diagsession"})
            check(!check_output_syntax(bad, d).empty(), "invalid output path rejected");
        check(check_output_syntax(L"C:\\a b\\run.DiagSession", d).empty() && d.explicit_file && d.directory == L"C:\\a b" && d.file == L"run.DiagSession", "file form");
        check(check_output_syntax(L"C:\\a b\\caps\\", d).empty() && !d.explicit_file && d.directory == L"C:\\a b\\caps", "directory form");
        check(check_output_syntax(L"C:\\x.diagsession\\", d).empty() && !d.explicit_file, "trailing slash forces directory form");
        check(check_output_syntax(L"C:\\r.diagsession", d).empty() && d.directory == L"C:\\", "drive-root file form");
        wchar_t cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        const std::wstring base = std::wstring(cwd) + L"\\Temp\\tests\\profile_output";
        system("rmdir /s /q Temp\\tests\\profile_output >NUL 2>&1");
        bool exists = true;
        check(prepare_directory(base + L"\\new\\deep", false, exists).empty() && !exists, "missing directories are not created during validation");
        check(prepare_directory(base + L"\\new\\deep", true, exists).empty() && exists && !absent(base + L"\\new\\deep"), "missing directories created");
        std::wstring name;
        check(free_name(base + L"\\new", name) && name == L"capture_1.diagsession", "first unique name");
        CloseHandle(CreateFileW((base + L"\\new\\capture_1.diagsession").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
        check(free_name(base + L"\\new", name) && name == L"capture_2.diagsession", "unique name skips existing capture");
        check(!prepare_directory(base + L"\\new\\capture_1.diagsession\\x", false, exists).empty(), "file component rejected");
        check(system("mklink /J Temp\\tests\\profile_output\\junction Temp\\tests\\profile_output\\new >NUL") == 0, "junction fixture");
        check(prepare_directory(base + L"\\junction", false, exists).find("reparse") != std::string::npos, "junction rejected");
        check(prepare_directory(base + L"\\junction\\sub", true, exists).find("reparse") != std::string::npos && absent(base + L"\\new\\sub"), "junction parent rejected before creation");
        session guarded;
        const std::string self = "profile start " + std::to_string(GetCurrentProcessId()) + " 1";
        check(guarded.command(self, utf8(base + L"\\new\\capture_1.diagsession")).find("already exists") != std::string::npos, "existing file rejected at start");
        check(guarded.command(self, "relative\\x").rfind("error:", 0) == 0, "relative rejected at start");
        check(guarded.command(self, utf8(base + L"\\junction")).find("reparse") != std::string::npos, "reparse rejected at start");
        check(guarded.command("profile status", "C:\\x").find("only for start") != std::string::npos, "output is start-only");
        check(guarded.command("profile status").find("state=idle") != std::string::npos, "rejected output starts nothing");
        system("rmdir Temp\\tests\\profile_output\\junction");
    }
    session capture;
    check(capture.command("profile status").find("state=idle") != std::string::npos, "idle status");
    check(capture.command("profile stop").find("state=idle") != std::string::npos, "idle stop");
    check(capture.command("profile report").rfind("error:", 0) == 0, "no fake report");
    if (argc != 2 || std::string(argv[1]) != "--live")
    {
        std::puts("PASS: VS profiler deterministic tests; live collection NOT RUN (use --live)");
        return 0;
    }
    workload(50);
    const auto start = capture.command("profile start " + std::to_string(GetCurrentProcessId()) + " 2");
    std::puts(start.c_str());
    check(start.rfind("ok:", 0) == 0, "capture accepted");
    check(capture.command("profile start 1 1").rfind("error:", 0) == 0, "overlapping capture rejected");
    const auto deadline = GetTickCount64() + 150000;
    bool ready = false;
    std::string status;
    do
    {
        workload(100);
        status = capture.command("profile status");
        if (status.find("state=ready") != std::string::npos) { ready = true; break; }
        if (status.find("state=failed") != std::string::npos) break;
    } while (GetTickCount64() < deadline);
    std::puts(status.c_str());
    check(ready, "capture completed and produced a nonempty diagsession");
    std::wstring path;
    check(capture.report_path(path) && detail::nonempty(path), "saved report file exists");
    check(capture.command("profile report").find("state=ready") != std::string::npos, "report route");
    std::puts("PASS: live VS CPU Usage capture; GUI analysis not exercised");
}
