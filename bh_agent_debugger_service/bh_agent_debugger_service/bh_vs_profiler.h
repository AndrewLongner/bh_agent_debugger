#pragma once
// VS CPU Usage capture adapter. Commands are serialized by the broker.
// No shell, injection, elevation, ETW-session takeover, or private VS analysis API.
// Only this object's fresh GUID is controlled. A collector lifetime guard ties it
// to the broker. Failed/uncertain cleanup blocks another capture until retried.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "../../bh_vs_installations.h"
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace bh_vs_profiler
{
namespace detail
{
struct handle
{
    HANDLE value = nullptr;
    handle() = default;
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
    ~handle() { reset(); }
    void reset(HANDLE next = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};
inline bool decimal(const std::string& text, DWORD& value)
{
    if (text.empty() || text.size() > 10) return false;
    unsigned long long n = 0;
    for (char ch : text)
    {
        if (ch < '0' || ch > '9') return false;
        n = n * 10 + static_cast<unsigned>(ch - '0');
        if (n > MAXDWORD) return false;
    }
    value = static_cast<DWORD>(n);
    return value != 0;
}
struct request { std::string verb; DWORD pid = 0, seconds = 10; };
inline bool parse(const std::string& text, request& result)
{
    if (text.size() > 128 || text.find_first_of("\r\n\t\v\f") != std::string::npos || text.find('\0') != std::string::npos) return false;
    std::istringstream in(text);
    std::string prefix, pid, seconds, extra;
    request next;
    if (!(in >> prefix >> next.verb) || prefix != "profile") return false;
    if (next.verb == "start")
    {
        if (!(in >> pid) || !decimal(pid, next.pid)) return false;
        if (in >> seconds) if (!decimal(seconds, next.seconds) || next.seconds > 60) return false;
    }
    else if (next.verb != "stop" && next.verb != "status" && next.verb != "report") return false;
    if (in >> extra) return false;
    result = next;
    return true;
}
inline std::wstring quote(const std::wstring& argument)
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument)
    {
        if (ch == L'\\') { ++slashes; continue; }
        out.append(ch == L'\"' ? 2 * slashes + 1 : slashes, L'\\');
        slashes = 0;
        out.push_back(ch);
    }
    out.append(2 * slashes, L'\\');
    out.push_back(L'\"');
    return out;
}
inline std::string utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], count, nullptr, nullptr);
    return result;
}
inline bool wide(const std::string& text, std::wstring& out)
{
    out.clear();
    if (text.empty()) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return false;
    out.resize(count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &out[0], count) == count;
}
inline std::wstring join(const std::wstring& directory, const std::wstring& name)
{
    return directory.empty() || directory.back() == L'\\' ? directory + name : directory + L"\\" + name;
}
// Caller-selected capture destination. Explicit .diagsession file or directory.
struct destination { std::wstring directory, file; bool explicit_file = false; };
constexpr size_t max_output_chars = 200;
// Pure syntax/canonical-form validation; never touches the filesystem except
// drive-type and full-path queries. Empty result means accepted.
inline std::string check_output_syntax(const std::wstring& path, destination& result)
{
    if (path.size() < 3 || path.size() > max_output_chars)
        return "output path must be an absolute local path of 3.." + std::to_string(max_output_chars) + " characters";
    const wchar_t drive = path[0];
    if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) || path[1] != L':' || path[2] != L'\\')
        return "output path must be absolute with a drive letter (X:\\...); relative, UNC and device paths are rejected";
    for (size_t i = 0; i < path.size(); ++i)
    {
        const wchar_t ch = path[i];
        if (ch < 32 || ch == L'/' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || (ch == L':' && i != 1))
            return "output path contains an unsupported character (use backslashes; no wildcards, streams or control characters)";
    }
    std::wstring trimmed = path;
    if (trimmed.size() > 3 && trimmed.back() == L'\\') trimmed.pop_back();
    if (trimmed.find(L"\\\\") != std::wstring::npos) return "output path contains an empty component";
    // Windows silently strips trailing dots/spaces; reject them (and . / ..) explicitly.
    for (size_t start = 3; start < trimmed.size();)
    {
        const size_t end = std::min(trimmed.find(L'\\', start), trimmed.size());
        const wchar_t last = trimmed[end - 1];
        if (last == L'.' || last == L' ') return "output path component ends with a dot or space (., .. and trailing dots/spaces are rejected)";
        start = end + 1;
    }
    wchar_t full[MAX_PATH] = {};
    const DWORD length = GetFullPathNameW(trimmed.c_str(), MAX_PATH, full, nullptr);
    if (!length || length >= MAX_PATH || trimmed != full)
        return "output path is not canonical (., .., trailing dots/spaces and reserved device names are rejected)";
    const UINT type = GetDriveTypeW(trimmed.substr(0, 3).c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK)
        return "output path must be on a local drive (network/mapped drives are rejected)";
    destination next;
    const std::wstring suffix = L".diagsession";
    const bool file_form = path.back() != L'\\' && trimmed.size() > suffix.size() &&
        _wcsicmp(trimmed.c_str() + trimmed.size() - suffix.size(), suffix.c_str()) == 0;
    if (file_form)
    {
        const size_t slash = trimmed.find_last_of(L'\\');
        if (trimmed.size() - slash - 1 <= suffix.size()) return "output file name is empty";
        next.explicit_file = true;
        next.directory = trimmed.substr(0, slash == 2 ? 3 : slash);
        next.file = trimmed.substr(slash + 1);
    }
    else next.directory = trimmed;
    result = next;
    return {};
}
// Walks existing components from the drive root. Rejects reparse points
// (symlinks/junctions/mount points) and non-directories; optionally creates
// missing components one at a time and re-verifies each one.
inline std::string prepare_directory(const std::wstring& directory, bool create, bool& exists)
{
    exists = false;
    if (directory.size() < 3) return "output directory invalid";
    std::wstring current = directory.substr(0, 3);
    size_t pos = 3;
    bool missing = false;
    for (;;)
    {
        if (!missing || create)
        {
            DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                    return "output path inaccessible win32=" + std::to_string(error) + " path=" + utf8(current);
                if (create)
                {
                    if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                        return "output directory creation failed win32=" + std::to_string(GetLastError()) + " path=" + utf8(current);
                    attributes = GetFileAttributesW(current.c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES)
                        return "output path inaccessible win32=" + std::to_string(GetLastError()) + " path=" + utf8(current);
                }
                else missing = true;
            }
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    return "output path contains a reparse point or symlink: " + utf8(current);
                if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
                    return "output path component is not a directory: " + utf8(current);
            }
        }
        if (pos >= directory.size()) break;
        const size_t next = directory.find(L'\\', pos);
        const size_t end = next == std::wstring::npos ? directory.size() : next;
        current = directory.substr(0, end);
        pos = end + 1;
    }
    exists = !missing;
    return {};
}
inline bool absent(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
// First unused capture_N.diagsession inside a caller-selected directory.
inline bool free_name(const std::wstring& directory, std::wstring& name)
{
    for (unsigned n = 1; n <= 9999; ++n)
    {
        const std::wstring candidate = L"capture_" + std::to_wstring(n) + L".diagsession";
        if (absent(join(directory, candidate))) { name = candidate; return true; }
    }
    return false;
}
inline bool file_exists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool nonempty(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (info.nFileSizeHigh || info.nFileSizeLow);
}
// Use the newest installed VS 2022/2026 CPU Usage collector (vswhere), never PATH lookup.
// The choice is kept while its files exist, so one capture never mixes two installs.
inline std::wstring collector_directory()
{
    static std::mutex lock;
    static std::wstring cached;
    std::lock_guard<std::mutex> guard(lock);
    const auto usable = [](const std::wstring& directory)
    {
        return !directory.empty() && file_exists(directory + L"\\VSDiagnostics.exe") &&
            file_exists(directory + L"\\AgentConfigs\\CpuUsageBase.json");
    };
    if (usable(cached)) return cached;
    cached.clear();
    for (const auto& vs : bh_vs_installations::find())
    {
        const std::wstring directory = vs.path + L"\\Team Tools\\DiagnosticsHub\\Collector";
        if (usable(directory)) { cached = directory; break; }
    }
    return cached;
}
inline std::wstring collector()
{
    const std::wstring directory = collector_directory();
    return directory.empty() ? std::wstring() : directory + L"\\VSDiagnostics.exe";
}
inline std::wstring config()
{
    const std::wstring directory = collector_directory();
    return directory.empty() ? std::wstring() : directory + L"\\AgentConfigs\\CpuUsageBase.json";
}
struct execution { DWORD code = ERROR_GEN_FAILURE; bool uncertain = false; };
inline execution run(const std::vector<std::wstring>& arguments, const std::wstring& log, DWORD timeout_ms)
{
    const std::wstring exe = collector();
    std::wstring line = quote(exe);
    for (const auto& arg : arguments) line += L" " + quote(arg);
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    handle output, input;
    output.reset(CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.value == INVALID_HANDLE_VALUE) return {GetLastError(), false};
    input.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) return {GetLastError(), false};
    // Explicit inherited-handle list prevents leaking broker/target handles.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return {GetLastError(), false};
    HANDLE inherited[] = {input.value, output.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
    {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(list);
        return {error, false};
    }
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.value;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(exe.c_str(), &line[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD error = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(list);
    if (!created) return {error, false};
    handle child, thread;
    child.reset(process.hProcess); thread.reset(process.hThread);
    if (WaitForSingleObject(child.value, timeout_ms) != WAIT_OBJECT_0)
    {
        // Terminate only this CLI invocation, never its collector service or target.
        TerminateProcess(child.value, ERROR_TIMEOUT);
        WaitForSingleObject(child.value, 5000);
        return {ERROR_TIMEOUT, true};
    }
    DWORD exit_code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(child.value, &exit_code);
    return {exit_code, false};
}
} // namespace detail

class session
{
    std::mutex mutex_;
    std::thread worker_;
    detail::handle wake_, target_;
    std::atomic<bool> busy_{false};
    std::string state_ = "idle", reason_, error_;
    std::wstring id_, directory_, output_;
    // Caller-selected destination (empty directory = default LocalAppData capture).
    // Captures published there belong to the caller; the broker never deletes them.
    detail::destination target_path_;
    DWORD pid_ = 0, seconds_ = 0;
    unsigned attempt_ = 0;
    bool owned_ = false;
    bool custom() const { return !target_path_.directory.empty(); }
    // Rename the finished temporary sibling without replacing anything.
    bool publish(const std::wstring& temp, std::wstring& final_path, std::string& error)
    {
        for (unsigned tries = 0; tries < 100; ++tries)
        {
            if (tries && !target_path_.explicit_file && !detail::free_name(target_path_.directory, target_path_.file)) break;
            const std::wstring candidate = detail::join(target_path_.directory, target_path_.file);
            if (MoveFileExW(temp.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH)) { final_path = candidate; return true; }
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS)
            {
                error = "output_rename_win32=" + std::to_string(code);
                return false;
            }
            if (target_path_.explicit_file)
            {
                error = "output_exists_at_finalize (file appeared during capture; not overwritten)";
                return false;
            }
        }
        error = "output_unique_name_unavailable";
        return false;
    }
    // Only the worker calls this while busy_; command() joins it before reuse.
    detail::execution invoke(const std::vector<std::wstring>& args)
    {
        return detail::run(args, directory_ + L"\\command_" + std::to_wstring(++attempt_) + L".log", 60000);
    }
    void phase(const std::string& state)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }
    void finish() noexcept
    {
        try
        {
            phase("stopping");
            // Always use a fresh filename when retrying uncertain packaging.
            std::wstring path = directory_ + L"\\capture_" + std::to_wstring(attempt_ + 1) + L".diagsession";
            bool to_caller = false;
            if (custom())
            {
                // Re-verify the destination; a swapped-in reparse point or removed
                // directory falls back to the broker-private folder to keep data.
                bool exists = false;
                const std::string problem = detail::prepare_directory(target_path_.directory, false, exists);
                if (problem.empty() && exists)
                {
                    path = detail::join(target_path_.directory, L".bh_partial_" + id_.substr(1, 8) + L"_" + std::to_wstring(attempt_ + 1) + L".diagsession");
                    to_caller = true;
                }
                else
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ += " output_destination_unsafe_at_finalize: " + (problem.empty() ? std::string("directory missing") : problem);
                }
            }
            const auto result = invoke({L"stop", id_, L"/output:" + path});
            std::lock_guard<std::mutex> lock(mutex_);
            if (!result.code && detail::nonempty(path))
            {
                owned_ = false;
                std::wstring final_path = path;
                std::string move_error;
                if (to_caller && !publish(path, final_path, move_error))
                {
                    state_ = "failed";
                    error_ += " " + move_error + " kept=\"" + detail::utf8(path) + "\"";
                }
                else
                {
                    output_ = final_path;
                    state_ = error_.empty() ? "ready" : "failed";
                    if (!error_.empty()) error_ += " kept=\"" + detail::utf8(final_path) + "\"";
                }
            }
            else
            {
                state_ = "failed";
                const bool partial = detail::nonempty(path);
                // Only temporary artifacts are removed: an empty partial sibling.
                if (to_caller && !partial && detail::file_exists(path)) DeleteFileW(path.c_str());
                error_ += " stop_or_package_exit=" + std::to_string(result.code) + " output_nonempty=" + std::to_string(partial);
                if (to_caller && partial) error_ += " kept=\"" + detail::utf8(path) + "\"";
                // Keep ownership after uncertain stop. Do not stop any other ID or
                // start another session to mask a leaked collection session.
            }
        }
        catch (...) { std::lock_guard<std::mutex> lock(mutex_); state_ = "failed"; error_ = "stop resource failure; cleanup required"; }
        busy_ = false;
    }
    void collect() noexcept
    {
        try
        {
            const auto started = invoke({L"start", id_, L"/attach:" + std::to_wstring(pid_), L"/loadConfig:" + detail::config()});
            if (started.code)
            {
                { std::lock_guard<std::mutex> lock(mutex_); error_ = "start_exit=" + std::to_string(started.code) + " (see command log)"; }
                // A failed start may have created a partial collection. Retain the
                // exact GUID for explicit cleanup retry if STOP cannot confirm it.
                finish();
                return;
            }
            const auto lifetime = invoke({L"update", id_, L"/lifetimeProcess:" + std::to_wstring(GetCurrentProcessId())});
            if (lifetime.code)
            {
                { std::lock_guard<std::mutex> lock(mutex_); error_ = "lifetime_guard_exit=" + std::to_string(lifetime.code); }
                finish(); return;
            }
            phase("running");
            HANDLE handles[] = {wake_.value, target_.value};
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, seconds_ * 1000);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                reason_ = wait == WAIT_TIMEOUT ? "deadline" : wait == WAIT_OBJECT_0 ? "requested" : wait == WAIT_OBJECT_0 + 1 ? "target_exit" : "wait_failure";
                if (wait == WAIT_FAILED) error_ = "wait_win32=" + std::to_string(GetLastError());
            }
            finish();
        }
        catch (...) { { std::lock_guard<std::mutex> lock(mutex_); error_ = "capture resource failure"; } finish(); }
    }
    std::string summary()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return (state_ == "failed" ? "error: " : "ok: ") + std::string("profile backend=vs_cpu_usage state=") + state_ +
            " pid=" + std::to_string(pid_) + " seconds_limit=" + std::to_string(seconds_) + " session=" + detail::utf8(id_) +
            " reason=" + reason_ + " cleanup_required=" + std::to_string(owned_ && !busy_) +
            " output=\"" + detail::utf8(output_) + "\" logs=\"" + detail::utf8(directory_) + "\"" +
            (custom() ? " destination=\"" + detail::utf8(detail::join(target_path_.directory, target_path_.file)) + "\" caller_owned=1" : std::string()) +
            " " + error_;
    }
    std::string start(DWORD pid, DWORD seconds, const std::string& output)
    {
        if (busy_) return "error: profile capture already active";
        if (worker_.joinable()) worker_.join();
        if (owned_) return "error: profile cleanup required; retry profile stop before starting another session";
        // Validate a caller-selected destination before touching the target.
        detail::destination destination;
        if (!output.empty())
        {
            std::wstring path;
            if (!detail::wide(output, path)) return "error: profile output path is not valid UTF-8";
            std::string problem = detail::check_output_syntax(path, destination);
            bool exists = false;
            if (problem.empty()) problem = detail::prepare_directory(destination.directory, false, exists);
            if (problem.empty() && destination.explicit_file && !detail::absent(detail::join(destination.directory, destination.file)))
                problem = "output file already exists (never overwritten): " + output;
            if (!problem.empty()) return "error: profile " + problem;
        }
        if (!detail::file_exists(detail::collector()) || !detail::file_exists(detail::config()))
            return "error: Visual Studio 2022/2026 CPU Usage collector/config not installed";
        target_.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!target_.value) return "error: profile OpenProcess win32=" + std::to_string(GetLastError());
        if (WaitForSingleObject(target_.value, 0) != WAIT_TIMEOUT) return "error: profile target already exited";
        GUID guid;
        if (FAILED(CoCreateGuid(&guid))) return "error: profile GUID creation failed";
        wchar_t text[40] = {};
        StringFromGUID2(guid, text, 40);
        PWSTR appdata = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata))) return "error: profile LocalAppData unavailable";
        std::wstring root(appdata);
        CoTaskMemFree(appdata);
        root += L"\\bh_agent_debugger\\profiles";
        const int made = SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
        if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS)
            return "error: profile output directory win32=" + std::to_string(made);
        const std::wstring directory = root + L"\\" + text;
        if (!CreateDirectoryW(directory.c_str(), nullptr)) return "error: profile unique output directory unavailable";
        if (!destination.directory.empty())
        {
            bool exists = false;
            const std::string problem = detail::prepare_directory(destination.directory, true, exists);
            if (!problem.empty()) return "error: profile " + problem;
            if (!destination.explicit_file && !detail::free_name(destination.directory, destination.file))
                return "error: profile output directory has no unused capture_N.diagsession name";
            if (destination.explicit_file && !detail::absent(detail::join(destination.directory, destination.file)))
                return "error: profile output file already exists (never overwritten)";
            const std::wstring temp = detail::join(destination.directory, L".bh_partial_00000000_00.diagsession");
            if (detail::join(destination.directory, destination.file).size() >= MAX_PATH || temp.size() >= MAX_PATH)
                return "error: profile output path too long";
        }
        wake_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!wake_.value) return "error: profile stop event unavailable";
        target_path_ = destination;
        pid_ = pid; seconds_ = seconds; id_ = text; directory_ = directory;
        output_.clear(); reason_.clear(); error_.clear(); attempt_ = 0;
        state_ = "starting"; owned_ = true; busy_ = true;
        try { worker_ = std::thread([this]() { collect(); }); }
        catch (...) { busy_ = false; owned_ = false; state_ = "failed"; error_ = "worker creation failed"; }
        return summary();
    }
public:
    session() = default;
    session(const session&) = delete;
    session& operator=(const session&) = delete;
    ~session()
    {
        if (wake_.value) SetEvent(wake_.value);
        if (worker_.joinable()) worker_.join();
        // Broker lifetime guard also stops collection on abrupt process exit.
    }
    std::string command(const std::string& message, const std::string& output = std::string())
    {
        try
        {
            detail::request request;
            if (!detail::parse(message, request)) return "error: profile syntax: start <pid> [seconds 1..60] | stop | status | report; analysis is in Visual Studio, not a text export";
            if (request.verb == "start") return start(request.pid, request.seconds, output);
            if (!output.empty()) return "error: profile output is valid only for start";
            if (request.verb == "stop")
            {
                if (busy_) { SetEvent(wake_.value); return "ok: profile stop requested; poll profile status until ready or failed"; }
                if (worker_.joinable()) worker_.join();
                if (owned_)
                {
                    busy_ = true;
                    try { worker_ = std::thread([this]() { finish(); }); }
                    catch (...) { busy_ = false; return "error: profile cleanup worker unavailable"; }
                    return "ok: profile cleanup requested; poll profile status";
                }
            }
            if (request.verb == "report")
            {
                std::wstring path;
                if (!report_path(path)) return "error: profile has no completed nonempty report; use profile status";
            }
            return summary();
        }
        catch (...) { return "error: profile resource failure"; }
    }
    bool report_path(std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || state_ != "ready" || !detail::nonempty(output_)) return false;
        path = output_;
        return true;
    }
};
} // namespace bh_vs_profiler
