#pragma once
// Finds installed Visual Studio 2022 (17.x) and Visual Studio 2026 (18.x) instances
// through vswhere.exe, which the Visual Studio Installer keeps at a fixed location.
// Fails closed: if vswhere is missing, fails, or times out, the list is empty and
// callers report that Visual Studio was not found instead of guessing a path.
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>
#include <vector>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace bh_vs_installations
{
struct installation
{
    std::wstring instance_id;  // vswhere instanceId, also part of profile folder names
    std::wstring path;         // installationPath
    std::wstring version;      // installationVersion, for example 18.11.12328.437
    unsigned major = 0;        // 17 = Visual Studio 2022, 18 = Visual Studio 2026
};

namespace detail
{
struct scoped_handle
{
    HANDLE value = nullptr;
    scoped_handle() = default;
    scoped_handle(const scoped_handle&) = delete;
    scoped_handle& operator=(const scoped_handle&) = delete;
    ~scoped_handle() { reset(); }
    void reset(HANDLE next = nullptr)
    {
        if (value != nullptr && value != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value);
        }
        value = next;
    }
};

inline std::wstring known_folder(REFKNOWNFOLDERID id)
{
    PWSTR folder = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, 0, nullptr, &folder);
    std::wstring path;
    if (SUCCEEDED(hr) && folder != nullptr)
    {
        path = folder;
    }
    CoTaskMemFree(folder);
    return path;
}

inline std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
    {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], size);
    return wide;
}

inline unsigned parse_major(const std::wstring& version)
{
    unsigned value = 0;
    size_t index = 0;
    while (index < version.size() && index < 4 && version[index] >= L'0' && version[index] <= L'9')
    {
        value = (value * 10U) + static_cast<unsigned>(version[index] - L'0');
        ++index;
    }
    return (index > 0 && index < version.size() && version[index] == L'.') ? value : 0U;
}

// Instance IDs end up in command lines and folder names, so accept only short alphanumerics.
inline bool is_safe_instance_id(const std::wstring& id)
{
    if (id.empty() || id.size() > 32)
    {
        return false;
    }
    for (const wchar_t ch : id)
    {
        const bool alnum = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
        if (!alnum)
        {
            return false;
        }
    }
    return true;
}

// Runs vswhere with only its output pipe inherited. Returns false on any failure, a nonzero
// exit code, oversized output, or timeout; a timed-out child is terminated.
inline bool run_vswhere(const std::wstring& arguments, std::string& output, DWORD timeout_ms)
{
    output.clear();
    const std::wstring folder = known_folder(FOLDERID_ProgramFilesX86);
    if (folder.empty())
    {
        return false;
    }
    const std::wstring exe = folder + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    const DWORD exe_attributes = GetFileAttributesW(exe.c_str());
    if (exe_attributes == INVALID_FILE_ATTRIBUTES || (exe_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return false;
    }

    SECURITY_ATTRIBUTES security = { sizeof(security), nullptr, TRUE };
    scoped_handle read_end;
    scoped_handle write_end;
    if (!CreatePipe(&read_end.value, &write_end.value, &security, 0))
    {
        return false;
    }
    if (!SetHandleInformation(read_end.value, HANDLE_FLAG_INHERIT, 0))
    {
        return false;
    }

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0)
    {
        return false;
    }
    std::vector<unsigned char> attribute_buffer(size);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &size))
    {
        return false;
    }
    HANDLE inherited[] = { write_end.value };
    if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
    {
        DeleteProcThreadAttributeList(attribute_list);
        return false;
    }

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write_end.value;
    startup.StartupInfo.hStdError = write_end.value;
    startup.lpAttributeList = attribute_list;

    std::wstring command_line = L"\"" + exe + L"\" " + arguments;
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(exe.c_str(), &command_line[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attribute_list);
    if (!created)
    {
        return false;
    }

    scoped_handle child;
    child.reset(process.hProcess);
    CloseHandle(process.hThread);
    write_end.reset();  // only the child holds the write end now, so the pipe breaks when it exits

    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    char chunk[4096];
    bool ok = true;
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(read_end.value, nullptr, 0, nullptr, &available, nullptr))
        {
            break;  // broken pipe: the child exited and all output was read
        }
        if (available > 0)
        {
            DWORD read = 0;
            const DWORD wanted = available < sizeof(chunk) ? available : static_cast<DWORD>(sizeof(chunk));
            if (!ReadFile(read_end.value, chunk, wanted, &read, nullptr))
            {
                break;
            }
            output.append(chunk, read);
            if (output.size() > (1U << 20))
            {
                ok = false;
                break;
            }
            continue;
        }
        if (GetTickCount64() >= deadline)
        {
            ok = false;
            break;
        }
        WaitForSingleObject(child.value, 20);
    }

    const ULONGLONG now = GetTickCount64();
    const DWORD remaining = (ok && now < deadline) ? static_cast<DWORD>(deadline - now) : 0;
    if (!ok || WaitForSingleObject(child.value, remaining) != WAIT_OBJECT_0)
    {
        TerminateProcess(child.value, ERROR_TIMEOUT);
        WaitForSingleObject(child.value, 1000);
        output.clear();
        return false;
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(child.value, &exit_code) || exit_code != 0)
    {
        output.clear();
        return false;
    }
    return true;
}
}  // namespace detail

inline bool is_file(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool is_directory(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Complete, launchable VS 2022 and VS 2026 instances (Preview/Insiders included), newest first.
inline std::vector<installation> find(DWORD timeout_ms = 15000)
{
    std::vector<installation> result;
    std::string output;
    if (!detail::run_vswhere(L"-nologo -utf8 -prerelease -products * -version [17.0,19.0) -sort -format text", output, timeout_ms))
    {
        return result;
    }

    const std::wstring text = detail::widen(output);
    installation current;
    const auto flush = [&result, &current]()
    {
        if ((current.major == 17 || current.major == 18) && detail::is_safe_instance_id(current.instance_id) && !current.path.empty())
        {
            result.push_back(current);
        }
        current = installation{};
    };

    // Text format: one "key: value" line per property; each instance starts with instanceId.
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos)
        {
            end = text.size();
        }
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }
        const size_t colon = line.find(L": ");
        if (colon == std::wstring::npos)
        {
            continue;
        }
        const std::wstring key = line.substr(0, colon);
        const std::wstring value = line.substr(colon + 2);
        if (key == L"instanceId")
        {
            flush();
            current.instance_id = value;
        }
        else if (key == L"installationPath")
        {
            current.path = value;
        }
        else if (key == L"installationVersion")
        {
            current.version = value;
            current.major = detail::parse_major(value);
        }
    }
    flush();
    return result;
}

inline std::wstring devenv_path(const installation& vs)
{
    return vs.path + L"\\Common7\\IDE\\devenv.exe";
}

// "<major>.0_<instanceId><suffix>": the per-user profile (hive) folder name.
inline std::wstring hive_name(const installation& vs, const std::wstring& root_suffix)
{
    return std::to_wstring(vs.major) + L".0_" + vs.instance_id + root_suffix;
}

// %LOCALAPPDATA%\Microsoft\VisualStudio, where the per-user hives live.
inline std::wstring local_hive_root()
{
    const std::wstring local = detail::known_folder(FOLDERID_LocalAppData);
    return local.empty() ? std::wstring() : local + L"\\Microsoft\\VisualStudio";
}

// True for VS 2022 ("17.0_...") and VS 2026 ("18.0_...") hive folder names.
inline bool is_supported_hive_name(const std::wstring& name)
{
    return name.rfind(L"17.0_", 0) == 0 || name.rfind(L"18.0_", 0) == 0;
}
}  // namespace bh_vs_installations
