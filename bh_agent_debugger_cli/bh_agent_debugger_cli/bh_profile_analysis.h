#pragma once
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <string>
#pragma comment(lib, "shell32.lib")

// Separate, bounded worker. Never load a CLR in the CLI/broker/VS, invoke a shell,
// resolve a helper through PATH, or attach to a live debugging session.
inline int run_profile_analysis()
{
    auto fail = [](const char* reason, DWORD code) -> int
    {
        std::cout << "{\"schemaVersion\":1,\"ok\":false,\"error\":\"" << reason << "\",\"win32\":" << code << "}" << std::endl;
        return 1;
    };
    wchar_t module[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
    if (!length || length >= 32768) return fail("CLI path unavailable", GetLastError());
    std::wstring path(module, length);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"bh_profile_analyzer.exe";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail("analysis helper missing; build.bat must build the matching development CLI and helper", ERROR_FILE_NOT_FOUND);
    auto quote = [](const std::wstring& text) -> std::wstring
    {
        std::wstring result = L"\"";
        size_t slashes = 0;
        for (wchar_t ch : text)
        {
            if (ch == L'\\') { ++slashes; continue; }
            result.append(ch == L'\"' ? 2 * slashes + 1 : slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
        result.append(2 * slashes, L'\\');
        result.push_back(L'\"');
        return result;
    };
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return fail("arguments unavailable", GetLastError());
    // CLI prefixes: <exe> profile analyze. Helper validates all remaining args.
    std::wstring command = quote(path);
    for (int i = 3; i < count; ++i) command += L" " + quote(args[i]);
    LocalFree(args);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return fail("analysis job unavailable", GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = 1024ULL * 1024 * 1024;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        const DWORD error = GetLastError(); CloseHandle(job); return fail("analysis limits unavailable", error);
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child = {};
    if (!CreateProcessW(path.c_str(), &command[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &child))
    {
        const DWORD error = GetLastError(); CloseHandle(job); return fail("analysis launch failed", error);
    }
    DWORD error = ERROR_SUCCESS;
    if (!AssignProcessToJobObject(job, child.hProcess)) error = GetLastError();
    else if (ResumeThread(child.hThread) == static_cast<DWORD>(-1)) error = GetLastError();
    CloseHandle(child.hThread);
    if (error)
    {
        TerminateProcess(child.hProcess, error); WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hProcess); CloseHandle(job); return fail("analysis isolation failed", error);
    }
    const DWORD wait = WaitForSingleObject(child.hProcess, 120000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &code);
    else
    {
        TerminateJobObject(job, ERROR_TIMEOUT); WaitForSingleObject(child.hProcess, 5000);
        fail("analysis timed out; its temporary bh-profile-analysis directory may need cleanup", ERROR_TIMEOUT);
    }
    CloseHandle(child.hProcess); CloseHandle(job);
    return static_cast<int>(code);
}
