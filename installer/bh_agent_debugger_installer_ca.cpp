#include <windows.h>
#include <tlhelp32.h>
#include <msiquery.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <userenv.h>

#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

#include "..\bh_vs_installations.h"

#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Advapi32.lib")

namespace
{
    std::wstring get_msi_property(MSIHANDLE hInstall, const wchar_t* name);
    std::wstring get_msi_target_path(MSIHANDLE hInstall, const wchar_t* directory_id);

    void log_msi_info(MSIHANDLE hInstall, const std::wstring& message)
    {
        if (hInstall == 0 || message.empty())
        {
            return;
        }

        MSIHANDLE record = MsiCreateRecord(1);
        if (record == 0)
        {
            return;
        }

        MsiRecordSetStringW(record, 0, message.c_str());
        MsiProcessMessage(hInstall, INSTALLMESSAGE_INFO, record);
        MsiCloseHandle(record);
    }

    // Returns the exit code, -1 when the process cannot start, or -2 when it is still running after
    // timeout_ms (it is left running).
    int run_hidden_process(std::wstring command_line, DWORD timeout_ms = INFINITE)
    {
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
        cmd.push_back(L'\0');

        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            return -1;
        }

        int result = -2;
        if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0)
        {
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            result = static_cast<int>(exit_code);
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        return result;
    }

    std::wstring get_bin_folder(MSIHANDLE hInstall)
    {
        std::wstring bin_folder = get_msi_target_path(hInstall, L"BINFOLDER");
        if (bin_folder.empty())
        {
            bin_folder = get_msi_property(hInstall, L"BINFOLDER");
        }

        return bin_folder;
    }

    std::wstring get_msi_target_path(MSIHANDLE hInstall, const wchar_t* directory_id)
    {
        if (hInstall == 0 || directory_id == nullptr)
        {
            return L"";
        }

        DWORD length = 32768;
        std::wstring value(length, L'\0');
        UINT result = MsiGetTargetPathW(hInstall, directory_id, value.data(), &length);
        if (result == ERROR_MORE_DATA)
        {
            value.assign(length + 1, L'\0');
            result = MsiGetTargetPathW(hInstall, directory_id, value.data(), &length);
        }

        if (result != ERROR_SUCCESS)
        {
            return L"";
        }

        value.resize(length);
        return value;
    }

    std::wstring trim(const std::wstring& value)
    {
        size_t first = 0;
        while (first < value.size() && iswspace(value[first]))
        {
            ++first;
        }

        size_t last = value.size();
        while (last > first && iswspace(value[last - 1]))
        {
            --last;
        }

        return value.substr(first, last - first);
    }

    std::wstring quote_for_cmd(const std::wstring& value)
    {
        std::wstring escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back(L'"');
        for (const wchar_t ch : value)
        {
            if (ch == L'"')
            {
                escaped.push_back(L'\\');
            }

            escaped.push_back(ch);
        }
        escaped.push_back(L'"');
        return escaped;
    }

    std::wstring get_system32_executable(const wchar_t* exe_name)
    {
        if (exe_name == nullptr || *exe_name == L'\0')
        {
            return L"";
        }

        wchar_t system_dir[MAX_PATH] = {};
        const UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
        {
            return L"";
        }

        std::filesystem::path path(system_dir);
        path /= exe_name;
        std::error_code ignored;
        if (!std::filesystem::exists(path, ignored))
        {
            return L"";
        }

        return path.wstring();
    }

    std::filesystem::path get_user_profile_dir(MSIHANDLE hInstall)
    {
        if (hInstall != 0)
        {
            std::filesystem::path local_app_data(get_msi_property(hInstall, L"LocalAppDataFolder"));
            if (!local_app_data.empty())
            {
                std::error_code ignored;
                local_app_data = std::filesystem::weakly_canonical(local_app_data, ignored);
                if (!local_app_data.empty() && local_app_data.has_parent_path())
                {
                    std::filesystem::path app_data = local_app_data.parent_path();
                    if (app_data.has_parent_path())
                    {
                        return app_data.parent_path();
                    }
                }
            }
        }

        wchar_t* user_profile = nullptr;
        size_t len = 0;
        if (_wdupenv_s(&user_profile, &len, L"USERPROFILE") != 0 || user_profile == nullptr)
        {
            user_profile = nullptr;
        }

        if (user_profile != nullptr)
        {
            std::filesystem::path path(user_profile);
            free(user_profile);

            const std::wstring text = path.wstring();
            if (!text.empty() && text.find(L"systemprofile") == std::wstring::npos)
            {
                return path;
            }
        }

        std::wstring logon_user = get_msi_property(hInstall, L"LogonUser");
        logon_user = trim(logon_user);
        if (logon_user.empty())
        {
            return {};
        }

        const size_t slash = logon_user.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash + 1 < logon_user.size())
        {
            logon_user = logon_user.substr(slash + 1);
        }

        if (logon_user.empty())
        {
            return {};
        }

        return std::filesystem::path(L"C:\\Users") / logon_user;
    }

    std::wstring windows_to_wsl_path(const std::filesystem::path& path)
    {
        std::wstring value = path.wstring();
        if (value.size() < 3 || value[1] != L':')
        {
            return L"";
        }

        const wchar_t drive = static_cast<wchar_t>(towlower(value[0]));
        std::wstring rest = value.substr(2);
        for (wchar_t& ch : rest)
        {
            if (ch == L'\\')
            {
                ch = L'/';
            }
        }

        if (!rest.empty() && rest[0] != L'/')
        {
            rest.insert(rest.begin(), L'/');
        }

        return std::wstring(L"/mnt/") + drive + rest;
    }

    bool has_wsl_distros()
    {
        const std::wstring wsl_exe = get_system32_executable(L"wsl.exe");
        if (wsl_exe.empty())
        {
            return false;
        }

        const std::wstring command = quote_for_cmd(wsl_exe) + L" -e sh -lc true";
        return run_hidden_process(command) == 0;
    }

    void set_msi_property(MSIHANDLE hInstall, const wchar_t* name, const wchar_t* value)
    {
        if (hInstall == 0 || name == nullptr || value == nullptr)
        {
            return;
        }

        MsiSetPropertyW(hInstall, name, value);
    }

    void set_verifyready_block(MSIHANDLE hInstall, bool block)
    {
        if (block)
        {
            set_msi_property(hInstall, L"OutOfDiskSpace", L"1");
            set_msi_property(hInstall, L"OutOfNoRbDiskSpace", L"0");
            set_msi_property(hInstall, L"PROMPTROLLBACKCOST", L"X");
            return;
        }

        set_msi_property(hInstall, L"OutOfDiskSpace", L"0");
        set_msi_property(hInstall, L"OutOfNoRbDiskSpace", L"0");
        set_msi_property(hInstall, L"PROMPTROLLBACKCOST", L"");
    }

    HWND get_msi_parent_window(MSIHANDLE hInstall)
    {
        const std::wstring hwnd_text = get_msi_property(hInstall, L"HWNDPARENT");
        if (hwnd_text.empty())
        {
            return nullptr;
        }

        const unsigned long long value = _wcstoui64(hwnd_text.c_str(), nullptr, 0);
        if (value == 0)
        {
            return nullptr;
        }

        return reinterpret_cast<HWND>(static_cast<ULONG_PTR>(value));
    }

    bool is_devenv_running()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);

        if (!Process32FirstW(snapshot, &entry))
        {
            CloseHandle(snapshot);
            return false;
        }

        do
        {
            if (_wcsicmp(entry.szExeFile, L"devenv.exe") == 0)
            {
                CloseHandle(snapshot);
                return true;
            }
        }
        while (Process32NextW(snapshot, &entry));

        CloseHandle(snapshot);
        return false;
    }

    bool is_devenv_running_via_tasklist()
    {
        const std::wstring command =
            L"cmd.exe /d /c tasklist /FI \"IMAGENAME eq devenv.exe\" | find /I \"devenv.exe\" >nul";
        return run_hidden_process(command) == 0;
    }

    bool is_devenv_running_via_powershell()
    {
        const std::wstring command =
            L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"if (Get-Process -Name devenv -ErrorAction SilentlyContinue) { exit 10 } else { exit 0 }\"";
        return run_hidden_process(command) == 10;
    }

    bool is_devenv_running_via_wts()
    {
        PWTS_PROCESS_INFOW processes = nullptr;
        DWORD count = 0;
        if (!WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &processes, &count))
        {
            return false;
        }

        bool found = false;
        for (DWORD i = 0; i < count; ++i)
        {
            const LPWSTR name = processes[i].pProcessName;
            if (name != nullptr && _wcsicmp(name, L"devenv.exe") == 0)
            {
                found = true;
                break;
            }
        }

        WTSFreeMemory(processes);
        return found;
    }

    bool is_devenv_running_any()
    {
        if (is_devenv_running())
        {
            return true;
        }

        if (is_devenv_running_via_wts())
        {
            return true;
        }

        if (is_devenv_running_via_powershell())
        {
            return true;
        }

        return is_devenv_running_via_tasklist();
    }

    std::wstring get_msi_property(MSIHANDLE hInstall, const wchar_t* name)
    {
        DWORD length = 0;
        wchar_t empty = L'\0';
        UINT result = MsiGetPropertyW(hInstall, name, &empty, &length);
        if (result != ERROR_MORE_DATA)
        {
            return L"";
        }

        std::wstring value(length + 1, L'\0');
        result = MsiGetPropertyW(hInstall, name, value.data(), &length);
        if (result != ERROR_SUCCESS)
        {
            return L"";
        }

        value.resize(length);
        return value;
    }

    std::filesystem::path get_localappdata_vs_root(MSIHANDLE hInstall)
    {
        if (hInstall != 0)
        {
            std::wstring local_app_data = get_msi_property(hInstall, L"LocalAppDataFolder");
            local_app_data = trim(local_app_data);
            if (!local_app_data.empty())
            {
                std::filesystem::path out(local_app_data);
                out /= L"Microsoft";
                out /= L"VisualStudio";
                return out;
            }
        }

        PWSTR local_app_data = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data);
        if (FAILED(hr) || local_app_data == nullptr)
        {
            return {};
        }

        std::filesystem::path out(local_app_data);
        CoTaskMemFree(local_app_data);
        out /= L"Microsoft";
        out /= L"VisualStudio";
        return out;
    }

    struct vsix_installer
    {
        std::wstring instance_id;
        std::filesystem::path path;
        std::filesystem::path devenv;  // empty when the instance has no devenv.exe
    };

    // VSIXInstaller.exe and devenv.exe of every VS 2022 (17.x) and VS 2026 (18.x) instance, newest first.
    std::vector<vsix_installer> find_vsix_installers()
    {
        std::vector<vsix_installer> installers;
        for (const auto& vs : bh_vs_installations::find())
        {
            const std::filesystem::path ide = std::filesystem::path(vs.path) / L"Common7" / L"IDE";
            std::filesystem::path candidate = ide / L"VSIXInstaller.exe";
            std::error_code ignored;
            if (std::filesystem::is_regular_file(candidate, ignored))
            {
                std::filesystem::path devenv = ide / L"devenv.exe";
                if (!std::filesystem::is_regular_file(devenv, ignored))
                {
                    devenv.clear();
                }

                installers.push_back({ vs.instance_id, candidate, devenv });
            }
        }

        return installers;
    }

    constexpr DWORD update_configuration_timeout_ms = 5 * 60 * 1000;

    // devenv /updateconfiguration rebuilds an instance's configuration from its pkgdef files. VSIXInstaller
    // does not always refresh it (a VS 2026 install kept the old configuration), and Visual Studio does not
    // load the package until it is refreshed. Call it after every VSIXInstaller run, so no devenv.exe is
    // running while another instance's VSIXInstaller checks for open Visual Studio processes.
    void update_vs_configurations(MSIHANDLE hInstall, const wchar_t* action, const std::vector<vsix_installer>& installers)
    {
        for (const auto& installer : installers)
        {
            const std::wstring prefix = std::wstring(L"[installer.ca] ") + action + L" updateconfiguration";
            if (installer.devenv.empty())
            {
                log_msi_info(hInstall, prefix + L" skip=devenv_missing instance=" + installer.instance_id);
                continue;
            }

            const int exit_code = run_hidden_process(
                quote_for_cmd(installer.devenv.wstring()) + L" /updateconfiguration",
                update_configuration_timeout_ms);
            log_msi_info(hInstall, prefix + L" exit_code=" + std::to_wstring(exit_code) + L" instance=" + installer.instance_id);
        }
    }

    // Agent host integrations (Claude Code, Codex). Each sync action records what it added under HKCU, and
    // its cleanup removes only that, never a skill or an MCP server the user set up by hand.
    const wchar_t* const claude_state_key = L"Software\\Boiling Lava Hot\\BH Agent Debugger\\ClaudeCode";
    const wchar_t* const claude_skill_dir_value = L"SkillDir";
    const wchar_t* const claude_mcp_value = L"McpServerAdded";
    const wchar_t* const codex_state_key = L"Software\\Boiling Lava Hot\\BH Agent Debugger\\Codex";
    const wchar_t* const codex_wsl_state_key = L"Software\\Boiling Lava Hot\\BH Agent Debugger\\CodexWsl";
    const wchar_t* const codex_skill_dir_value = L"SkillDir";
    const wchar_t* const codex_skill_installed_value = L"SkillInstalled";
    // The CLI path that codex mcp add registered: a Windows path, or its /mnt/... form under CodexWsl.
    const wchar_t* const codex_mcp_command_value = L"McpCommand";
    constexpr DWORD agent_tool_timeout_ms = 60 * 1000;
    constexpr DWORD codex_wsl_timeout_ms = 120 * 1000;

    // The environment Windows builds for a new process of this user, so PATH and CLAUDE_CONFIG_DIR are
    // current even when msiexec started before Claude Code was installed. Empty on failure.
    std::vector<wchar_t> get_user_environment_block()
    {
        HANDLE token = nullptr;
        const DWORD access = TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE;
        if (!OpenThreadToken(GetCurrentThread(), access, TRUE, &token)
            && !OpenProcessToken(GetCurrentProcess(), access, &token))
        {
            return {};
        }

        void* block = nullptr;
        const BOOL created = CreateEnvironmentBlock(&block, token, FALSE);
        CloseHandle(token);
        if (!created || block == nullptr)
        {
            return {};
        }

        const wchar_t* begin = static_cast<const wchar_t*>(block);
        const wchar_t* end = begin;
        while (*end != L'\0')
        {
            end += wcslen(end) + 1;
        }

        std::vector<wchar_t> environment(begin, end + 1);
        environment.push_back(L'\0');
        DestroyEnvironmentBlock(block);
        return environment;
    }

    // The value of name in environment, or in this process's environment when environment is empty.
    std::wstring get_environment_value(const std::vector<wchar_t>& environment, const wchar_t* name)
    {
        if (environment.empty())
        {
            wchar_t* value = nullptr;
            size_t length = 0;
            if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr)
            {
                return L"";
            }

            std::wstring result(value);
            free(value);
            return result;
        }

        const size_t name_length = wcslen(name);
        const wchar_t* entry = environment.data();
        while (*entry != L'\0')
        {
            const size_t length = wcslen(entry);
            if (length > name_length && entry[name_length] == L'=' && _wcsnicmp(entry, name, name_length) == 0)
            {
                return std::wstring(entry + name_length + 1);
            }

            entry += length + 1;
        }

        return L"";
    }

    // %CLAUDE_CONFIG_DIR% when set, otherwise %USERPROFILE%\.claude. Empty when unknown or not absolute.
    std::filesystem::path get_claude_config_dir(MSIHANDLE hInstall, const std::vector<wchar_t>& environment)
    {
        const std::wstring configured = trim(get_environment_value(environment, L"CLAUDE_CONFIG_DIR"));
        if (!configured.empty())
        {
            const std::filesystem::path path(configured);
            return path.is_absolute() ? path : std::filesystem::path();
        }

        const std::filesystem::path profile = get_user_profile_dir(hInstall);
        return profile.empty() ? profile : profile / L".claude";
    }

    // The first file named like one of names in a folder of the user's PATH. Empty when not found.
    std::filesystem::path find_on_user_path(const std::vector<wchar_t>& environment, std::initializer_list<const wchar_t*> names)
    {
        const std::wstring path_value = get_environment_value(environment, L"PATH");
        std::error_code ignored;
        size_t start = 0;
        while (start < path_value.size())
        {
            size_t end = path_value.find(L';', start);
            if (end == std::wstring::npos)
            {
                end = path_value.size();
            }

            std::wstring entry = trim(path_value.substr(start, end - start));
            start = end + 1;
            if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"')
            {
                entry = entry.substr(1, entry.size() - 2);
            }

            const std::filesystem::path dir(entry);
            if (entry.empty() || !dir.is_absolute())
            {
                continue;
            }

            for (const wchar_t* name : names)
            {
                const std::filesystem::path candidate = dir / name;
                if (std::filesystem::is_regular_file(candidate, ignored))
                {
                    return candidate;
                }
            }
        }

        return {};
    }

    // claude.exe (native installer, WinGet) or claude.cmd (npm) from the user's PATH, else the native
    // installer's default %USERPROFILE%\.local\bin\claude.exe. Empty when not found.
    std::filesystem::path find_claude_command(MSIHANDLE hInstall, const std::vector<wchar_t>& environment)
    {
        const std::filesystem::path found = find_on_user_path(environment, { L"claude.exe", L"claude.cmd" });
        if (!found.empty())
        {
            return found;
        }

        std::error_code ignored;
        const std::filesystem::path profile = get_user_profile_dir(hInstall);
        if (!profile.empty())
        {
            const std::filesystem::path candidate = profile / L".local" / L"bin" / L"claude.exe";
            if (std::filesystem::is_regular_file(candidate, ignored))
            {
                return candidate;
            }
        }

        return {};
    }

    // Runs command_line hidden with the user's environment. After timeout_ms it ends the process and every
    // process it started. Returns the exit code, -1 when the process cannot start, or -2 on timeout.
    int run_user_process(const std::wstring& command_line, std::vector<wchar_t> environment, const std::filesystem::path& working_dir, DWORD timeout_ms)
    {
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
        cmd.push_back(L'\0');

        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
        void* environment_block = nullptr;
        if (!environment.empty())
        {
            flags |= CREATE_UNICODE_ENVIRONMENT;
            environment_block = environment.data();
        }

        const wchar_t* directory = working_dir.empty() ? nullptr : working_dir.c_str();
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, environment_block, directory, &si, &pi))
        {
            return -1;
        }

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        const bool in_job = job != nullptr && AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);

        int result = -2;
        if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0)
        {
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            result = static_cast<int>(exit_code);
        }
        else if (in_job)
        {
            TerminateJobObject(job, 1);
        }
        else
        {
            TerminateProcess(pi.hProcess, 1);
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job != nullptr)
        {
            CloseHandle(job);
        }

        return result;
    }

    // Runs an agent host's command line tool (an .exe, or a .cmd through cmd.exe) with arguments and the
    // user's environment for at most agent_tool_timeout_ms; see run_user_process for the result.
    int run_user_tool(const std::filesystem::path& tool, const std::wstring& arguments, const std::vector<wchar_t>& environment, const std::filesystem::path& working_dir)
    {
        std::wstring command_line = quote_for_cmd(tool.wstring()) + L" " + arguments;
        if (_wcsicmp(tool.extension().c_str(), L".cmd") == 0)
        {
            const std::wstring cmd_exe = get_system32_executable(L"cmd.exe");
            if (cmd_exe.empty())
            {
                return -1;
            }

            // /s removes only the outer quotes, so the quoted paths inside stay intact.
            command_line = quote_for_cmd(cmd_exe) + L" /d /s /c \"" + command_line + L"\"";
        }

        return run_user_process(command_line, environment, working_dir, agent_tool_timeout_ms);
    }

    std::wstring read_state_text(const wchar_t* state_key, const wchar_t* name)
    {
        DWORD size = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, state_key, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS
            || size < sizeof(wchar_t))
        {
            return L"";
        }

        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, state_key, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
        {
            return L"";
        }

        value.resize(wcsnlen(value.c_str(), value.size()));
        return value;
    }

    bool read_state_flag(const wchar_t* state_key, const wchar_t* name)
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, state_key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
            && value != 0;
    }

    void write_state(const wchar_t* state_key, const wchar_t* name, DWORD type, const void* data, DWORD size)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, state_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        (void)RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
        RegCloseKey(key);
    }

    void write_state_text(const wchar_t* state_key, const wchar_t* name, const std::wstring& value)
    {
        write_state(state_key, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    void delete_state(const wchar_t* state_key, const wchar_t* name)
    {
        (void)RegDeleteKeyValueW(HKEY_CURRENT_USER, state_key, name);
    }

    // True for an empty profile path or the LocalSystem profile: an agent host's command must never write
    // another account's user-scope configuration.
    bool is_system_profile(std::wstring path)
    {
        for (wchar_t& ch : path)
        {
            ch = static_cast<wchar_t>(towlower(ch));
        }

        return path.empty() || path.find(L"systemprofile") != std::wstring::npos;
    }

    // Only <folder>\skills\bh-agent-debugger, the form SyncClaudeCode and SyncCodex record.
    bool is_agent_skill_dir(const std::filesystem::path& dir)
    {
        return dir.is_absolute()
            && _wcsicmp(dir.filename().c_str(), L"bh-agent-debugger") == 0
            && _wcsicmp(dir.parent_path().filename().c_str(), L"skills") == 0;
    }

    // %CODEX_HOME% when set, otherwise %USERPROFILE%\.codex. Empty when unknown or not absolute.
    std::filesystem::path get_codex_home(MSIHANDLE hInstall, const std::vector<wchar_t>& environment)
    {
        const std::wstring configured = trim(get_environment_value(environment, L"CODEX_HOME"));
        if (!configured.empty())
        {
            const std::filesystem::path path(configured);
            return path.is_absolute() ? path : std::filesystem::path();
        }

        const std::filesystem::path profile = get_user_profile_dir(hInstall);
        return profile.empty() ? profile : profile / L".codex";
    }

    // What the exit codes of the scripts that SyncCodexWsl and CleanupCodexWsl run in WSL mean, and the
    // results of run_user_process.
    const wchar_t* describe_codex_wsl_exit(int exit_code)
    {
        switch (exit_code)
        {
        case 0: return L"done";
        case 20: return L"mcp_already_registered";
        case 21: return L"codex_not_found";
        case 22: return L"codex_does_not_run";
        case 23: return L"skill_copy_failed";
        case 24: return L"cli_not_reachable";
        case 25: return L"server_added_by_user_kept";
        case 26: return L"mcp_add_failed";
        case 27: return L"mcp_remove_failed";
        case -1: return L"wsl_not_started";
        case -2: return L"timeout";
        default: return L"unexpected";
        }
    }
}

extern "C" __declspec(dllexport) UINT __stdcall CreateDesktopTaskMachine(MSIHANDLE hInstall)
{
    try
    {
        const std::wstring bin_folder = get_bin_folder(hInstall);
        if (bin_folder.empty())
        {
            return ERROR_SUCCESS;
        }

        std::wstring exe = bin_folder;
        if (!exe.empty() && exe.back() != L'\\')
        {
            exe.push_back(L'\\');
        }
        exe += L"bh_agent_debugger_service.exe";

        std::wstring ps =
            L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
            L"\"$task='bh_agent_debugger_start_service_desktop';"
            L"$exe='" + exe + L"';"
            L"$user=(Get-CimInstance Win32_ComputerSystem).UserName;"
            L"if([string]::IsNullOrWhiteSpace($user)){exit 0};"
            L"$a=New-ScheduledTaskAction -Execute $exe;"
            L"$p=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited;"
            L"$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -StartWhenAvailable;"
            L"Register-ScheduledTask -TaskName $task -Action $a -Principal $p -Settings $s -Force | Out-Null\"";

        (void)run_hidden_process(ps);
    }
    catch (...)
    {
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall CreateDesktopTaskUser(MSIHANDLE hInstall)
{
    try
    {
        const std::wstring bin_folder = get_bin_folder(hInstall);
        if (bin_folder.empty())
        {
            return ERROR_SUCCESS;
        }

        std::wstring exe = bin_folder;
        if (!exe.empty() && exe.back() != L'\\')
        {
            exe.push_back(L'\\');
        }
        exe += L"bh_agent_debugger_service.exe";

        std::wstring ps =
            L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
            L"\"$task='bh_agent_debugger_start_service_desktop_user';"
            L"$exe='" + exe + L"';"
            L"$user=[System.Security.Principal.WindowsIdentity]::GetCurrent().Name;"
            L"if([string]::IsNullOrWhiteSpace($user)){exit 0};"
            L"$a=New-ScheduledTaskAction -Execute $exe;"
            L"$p=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited;"
            L"$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -StartWhenAvailable;"
            L"Register-ScheduledTask -TaskName $task -Action $a -Principal $p -Settings $s -Force | Out-Null\"";

        (void)run_hidden_process(ps);
    }
    catch (...)
    {
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall StopAgentProcesses(MSIHANDLE hInstall)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        log_msi_info(hInstall, L"[installer.ca] StopAgentProcesses snapshot_failed");
        return ERROR_INSTALL_FAILURE;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool failed = false;
    unsigned int stopped_service_count = 0;
    unsigned int stopped_cli_count = 0;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            const bool is_service = _wcsicmp(entry.szExeFile, L"bh_agent_debugger_service.exe") == 0;
            const bool is_cli = _wcsicmp(entry.szExeFile, L"bh_agent_debugger_cli.exe") == 0;
            if (!is_service && !is_cli) continue;

            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (process == nullptr)
            {
                failed = true;
                continue;
            }

            if (!TerminateProcess(process, 0) || WaitForSingleObject(process, 5000) == WAIT_TIMEOUT)
            {
                failed = true;
            }
            else if (is_service)
            {
                ++stopped_service_count;
            }
            else
            {
                ++stopped_cli_count;
            }
            CloseHandle(process);
        }
        while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    log_msi_info(hInstall, L"[installer.ca] StopAgentProcesses services=" + std::to_wstring(stopped_service_count) +
        L" cli=" + std::to_wstring(stopped_cli_count) +
        L" failed=" + std::wstring(failed ? L"true" : L"false"));
    return failed ? ERROR_INSTALL_FAILURE : ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall DeleteDesktopTaskMachine(MSIHANDLE)
{
    try
    {
        (void)run_hidden_process(L"schtasks.exe /Delete /TN \"bh_agent_debugger_start_service_desktop\" /F");
    }
    catch (...)
    {
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall DeleteDesktopTaskUser(MSIHANDLE)
{
    try
    {
        (void)run_hidden_process(L"schtasks.exe /Delete /TN \"bh_agent_debugger_start_service_desktop_user\" /F");
    }
    catch (...)
    {
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall SyncOpenCodeWindows(MSIHANDLE hInstall)
{
    try
    {
        std::filesystem::path source_root(get_msi_target_path(hInstall, L"OPENCODEFOLDER"));
        if (source_root.empty())
        {
            source_root = std::filesystem::path(get_msi_property(hInstall, L"OPENCODEFOLDER"));
        }
        if (source_root.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWindows skip=source_root_empty");
            return ERROR_SUCCESS;
        }

        const std::filesystem::path source_tool = source_root / L"tools" / L"bh_agent_debugger.ts";
        const std::filesystem::path source_skill = source_root / L"skills" / L"bh-agent-debugger" / L"SKILL.md";
        if (!std::filesystem::exists(source_tool) || !std::filesystem::exists(source_skill))
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWindows skip=payload_missing");
            return ERROR_SUCCESS;
        }

        std::filesystem::path dest_root = get_user_profile_dir(hInstall);
        if (dest_root.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWindows skip=user_profile_empty");
            return ERROR_SUCCESS;
        }

        dest_root /= L".config";
        dest_root /= L"opencode";

        const std::filesystem::path dest_tool = dest_root / L"tools" / L"bh_agent_debugger.ts";
        const std::filesystem::path dest_skill = dest_root / L"skills" / L"bh-agent-debugger" / L"SKILL.md";
        std::filesystem::create_directories(dest_tool.parent_path());
        std::filesystem::create_directories(dest_skill.parent_path());
        std::filesystem::copy_file(source_tool, dest_tool, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(source_skill, dest_skill, std::filesystem::copy_options::overwrite_existing);
        log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWindows synced");
    }
    catch (...)
    {
        log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWindows exception");
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall CleanupOpenCodeWindows(MSIHANDLE hInstall)
{
    try
    {
        std::filesystem::path dest_root = get_user_profile_dir(hInstall);
        if (dest_root.empty())
        {
            return ERROR_SUCCESS;
        }

        dest_root /= L".config";
        dest_root /= L"opencode";

        std::error_code ignored;
        std::filesystem::remove(dest_root / L"tools" / L"bh_agent_debugger.ts", ignored);
        std::filesystem::remove(dest_root / L"skills" / L"bh-agent-debugger" / L"SKILL.md", ignored);
        std::filesystem::remove_all(dest_root / L"skills" / L"bh-agent-debugger", ignored);
        log_msi_info(hInstall, L"[installer.ca] CleanupOpenCodeWindows cleaned");
    }
    catch (...)
    {
        log_msi_info(hInstall, L"[installer.ca] CleanupOpenCodeWindows exception");
    }

    return ERROR_SUCCESS;
}

// Copies the agent skill into Claude Code and registers the bh_agent_debugger MCP server for this user.
// Does nothing when there is neither a Claude Code configuration folder nor a claude command.
extern "C" __declspec(dllexport) UINT __stdcall SyncClaudeCode(MSIHANDLE hInstall)
{
    const std::wstring prefix = L"[installer.ca] SyncClaudeCode";
    try
    {
        std::filesystem::path source_root(get_msi_target_path(hInstall, L"OPENCODEFOLDER"));
        if (source_root.empty())
        {
            source_root = std::filesystem::path(get_msi_property(hInstall, L"OPENCODEFOLDER"));
        }

        std::error_code ignored;
        const std::filesystem::path source_skill = source_root / L"skills" / L"bh-agent-debugger" / L"SKILL.md";
        if (source_root.empty() || !std::filesystem::is_regular_file(source_skill, ignored))
        {
            log_msi_info(hInstall, prefix + L" skip=payload_missing");
            return ERROR_SUCCESS;
        }

        const std::vector<wchar_t> environment = get_user_environment_block();
        const std::filesystem::path config_dir = get_claude_config_dir(hInstall, environment);
        const std::filesystem::path claude = find_claude_command(hInstall, environment);
        const bool config_found = !config_dir.empty() && std::filesystem::is_directory(config_dir, ignored);
        if (!config_found && claude.empty())
        {
            log_msi_info(hInstall, prefix + L" skip=claude_code_not_found");
            return ERROR_SUCCESS;
        }

        if (config_dir.empty())
        {
            log_msi_info(hInstall, prefix + L" skill skip=config_dir_unknown");
        }
        else
        {
            const std::filesystem::path skill_dir = config_dir / L"skills" / L"bh-agent-debugger";
            std::error_code error;
            std::filesystem::create_directories(skill_dir, error);
            if (!error)
            {
                std::filesystem::copy_file(source_skill, skill_dir / L"SKILL.md", std::filesystem::copy_options::overwrite_existing, error);
            }

            if (error)
            {
                log_msi_info(hInstall, prefix + L" skill error=" + std::to_wstring(error.value()) + L" path=" + skill_dir.wstring());
            }
            else
            {
                const std::wstring recorded = skill_dir.wstring();
                write_state_text(claude_state_key, claude_skill_dir_value, recorded);
                log_msi_info(hInstall, prefix + L" skill synced path=" + recorded);
            }
        }

        if (claude.empty())
        {
            log_msi_info(hInstall, prefix + L" mcp skip=claude_command_not_found");
            return ERROR_SUCCESS;
        }

        const std::filesystem::path cli = std::filesystem::path(get_bin_folder(hInstall)) / L"bh_agent_debugger_cli.exe";
        if (!std::filesystem::is_regular_file(cli, ignored))
        {
            log_msi_info(hInstall, prefix + L" mcp skip=cli_missing");
            return ERROR_SUCCESS;
        }

        if (is_system_profile(get_environment_value(environment, L"USERPROFILE")))
        {
            log_msi_info(hInstall, prefix + L" mcp skip=not_user_context");
            return ERROR_SUCCESS;
        }

        // Replace the registration an earlier install added, so it runs this install's CLI. A server of
        // the same name that the user added is kept: claude mcp add then fails without changing it.
        if (read_state_flag(claude_state_key, claude_mcp_value))
        {
            const int remove_exit = run_user_tool(claude, L"mcp remove --scope user bh_agent_debugger", environment, cli.parent_path());
            delete_state(claude_state_key, claude_mcp_value);
            log_msi_info(hInstall, prefix + L" mcp_remove exit_code=" + std::to_wstring(remove_exit));
        }

        const int add_exit = run_user_tool(
            claude,
            L"mcp add --scope user bh_agent_debugger -- " + quote_for_cmd(cli.wstring()) + L" mcp",
            environment,
            cli.parent_path());
        if (add_exit == 0)
        {
            const DWORD added = 1;
            write_state(claude_state_key, claude_mcp_value, REG_DWORD, &added, sizeof(added));
        }

        log_msi_info(hInstall, prefix + L" mcp_add exit_code=" + std::to_wstring(add_exit) + L" claude=" + claude.wstring());
    }
    catch (...)
    {
        log_msi_info(hInstall, prefix + L" exception");
    }

    return ERROR_SUCCESS;
}

// Removes what SyncClaudeCode recorded: the skill (its folder only when nothing else is in it) and the
// MCP server registration.
extern "C" __declspec(dllexport) UINT __stdcall CleanupClaudeCode(MSIHANDLE hInstall)
{
    const std::wstring prefix = L"[installer.ca] CleanupClaudeCode";
    try
    {
        const std::filesystem::path skill_dir(read_state_text(claude_state_key, claude_skill_dir_value));
        if (!skill_dir.empty())
        {
            if (is_agent_skill_dir(skill_dir))
            {
                std::error_code ignored;
                std::filesystem::remove(skill_dir / L"SKILL.md", ignored);
                std::filesystem::remove(skill_dir, ignored);
                log_msi_info(hInstall, prefix + L" skill removed path=" + skill_dir.wstring());
            }
            else
            {
                log_msi_info(hInstall, prefix + L" skill skip=unexpected_path");
            }

            delete_state(claude_state_key, claude_skill_dir_value);
        }

        if (read_state_flag(claude_state_key, claude_mcp_value))
        {
            const std::vector<wchar_t> environment = get_user_environment_block();
            const std::filesystem::path claude = find_claude_command(hInstall, environment);
            if (claude.empty())
            {
                log_msi_info(hInstall, prefix + L" mcp_remove skip=claude_command_not_found");
            }
            else if (is_system_profile(get_environment_value(environment, L"USERPROFILE")))
            {
                log_msi_info(hInstall, prefix + L" mcp_remove skip=not_user_context");
            }
            else
            {
                std::filesystem::path working_dir(get_bin_folder(hInstall));
                std::error_code ignored;
                if (!std::filesystem::is_directory(working_dir, ignored))
                {
                    working_dir.clear();
                }

                const int remove_exit = run_user_tool(claude, L"mcp remove --scope user bh_agent_debugger", environment, working_dir);
                log_msi_info(hInstall, prefix + L" mcp_remove exit_code=" + std::to_wstring(remove_exit));
            }

            delete_state(claude_state_key, claude_mcp_value);
        }

        (void)RegDeleteKeyW(HKEY_CURRENT_USER, claude_state_key);
    }
    catch (...)
    {
        log_msi_info(hInstall, prefix + L" exception");
    }

    return ERROR_SUCCESS;
}

// Copies the agent skill to the user skill folder that Codex reads and registers the bh_agent_debugger MCP
// server with codex mcp add. Does nothing when there is neither a Codex home folder nor a codex command.
extern "C" __declspec(dllexport) UINT __stdcall SyncCodex(MSIHANDLE hInstall)
{
    const std::wstring prefix = L"[installer.ca] SyncCodex";
    try
    {
        std::filesystem::path source_root(get_msi_target_path(hInstall, L"OPENCODEFOLDER"));
        if (source_root.empty())
        {
            source_root = std::filesystem::path(get_msi_property(hInstall, L"OPENCODEFOLDER"));
        }

        std::error_code ignored;
        const std::filesystem::path source_skill = source_root / L"skills" / L"bh-agent-debugger" / L"SKILL.md";
        if (source_root.empty() || !std::filesystem::is_regular_file(source_skill, ignored))
        {
            log_msi_info(hInstall, prefix + L" skip=payload_missing");
            return ERROR_SUCCESS;
        }

        const std::vector<wchar_t> environment = get_user_environment_block();
        const std::filesystem::path codex_home = get_codex_home(hInstall, environment);
        const std::filesystem::path codex = find_on_user_path(environment, { L"codex.exe", L"codex.cmd" });
        const bool home_found = !codex_home.empty() && std::filesystem::is_directory(codex_home, ignored);
        if (!home_found && codex.empty())
        {
            log_msi_info(hInstall, prefix + L" skip=codex_not_found");
            return ERROR_SUCCESS;
        }

        // Codex reads user skills from %USERPROFILE%\.agents\skills, wherever CODEX_HOME points.
        const std::filesystem::path profile = get_user_profile_dir(hInstall);
        if (profile.empty())
        {
            log_msi_info(hInstall, prefix + L" skill skip=user_profile_unknown");
        }
        else
        {
            const std::filesystem::path skill_dir = profile / L".agents" / L"skills" / L"bh-agent-debugger";
            std::error_code error;
            std::filesystem::create_directories(skill_dir, error);
            if (!error)
            {
                std::filesystem::copy_file(source_skill, skill_dir / L"SKILL.md", std::filesystem::copy_options::overwrite_existing, error);
            }

            if (error)
            {
                log_msi_info(hInstall, prefix + L" skill error=" + std::to_wstring(error.value()) + L" path=" + skill_dir.wstring());
            }
            else
            {
                const std::wstring recorded = skill_dir.wstring();
                write_state_text(codex_state_key, codex_skill_dir_value, recorded);
                log_msi_info(hInstall, prefix + L" skill synced path=" + recorded);
            }
        }

        if (codex.empty())
        {
            log_msi_info(hInstall, prefix + L" mcp skip=codex_command_not_found");
            return ERROR_SUCCESS;
        }

        const std::filesystem::path cli = std::filesystem::path(get_bin_folder(hInstall)) / L"bh_agent_debugger_cli.exe";
        if (!std::filesystem::is_regular_file(cli, ignored))
        {
            log_msi_info(hInstall, prefix + L" mcp skip=cli_missing");
            return ERROR_SUCCESS;
        }

        if (is_system_profile(get_environment_value(environment, L"USERPROFILE")))
        {
            log_msi_info(hInstall, prefix + L" mcp skip=not_user_context");
            return ERROR_SUCCESS;
        }

        // A codex command that does not run (for example the npm launcher on a Node.js older than Codex
        // needs) would make codex mcp get fail as if the name were free, so check that it starts first.
        const int version_exit = run_user_tool(codex, L"--version", environment, cli.parent_path());
        if (version_exit != 0)
        {
            log_msi_info(hInstall, prefix + L" mcp skip=codex_does_not_run exit_code=" + std::to_wstring(version_exit) + L" codex=" + codex.wstring());
            return ERROR_SUCCESS;
        }

        // codex mcp add replaces a server of the same name together with the settings the user added to
        // it. So it runs only when the name is free, or when this installer registered another CLI path.
        const std::wstring recorded_command = read_state_text(codex_state_key, codex_mcp_command_value);
        const int get_exit = run_user_tool(codex, L"mcp get bh_agent_debugger", environment, cli.parent_path());
        if (get_exit < 0)
        {
            log_msi_info(hInstall, prefix + L" mcp skip=codex_does_not_run exit_code=" + std::to_wstring(get_exit) + L" codex=" + codex.wstring());
            return ERROR_SUCCESS;
        }

        if (get_exit == 0)
        {
            if (recorded_command.empty())
            {
                log_msi_info(hInstall, prefix + L" mcp skip=server_added_by_user_kept");
                return ERROR_SUCCESS;
            }

            if (_wcsicmp(recorded_command.c_str(), cli.c_str()) == 0)
            {
                log_msi_info(hInstall, prefix + L" mcp kept=already_registered");
                return ERROR_SUCCESS;
            }
        }

        const int add_exit = run_user_tool(
            codex,
            L"mcp add bh_agent_debugger -- " + quote_for_cmd(cli.wstring()) + L" mcp",
            environment,
            cli.parent_path());
        if (add_exit == 0)
        {
            write_state_text(codex_state_key, codex_mcp_command_value, cli.wstring());
        }

        log_msi_info(hInstall, prefix + L" mcp_add exit_code=" + std::to_wstring(add_exit) + L" codex=" + codex.wstring());
    }
    catch (...)
    {
        log_msi_info(hInstall, prefix + L" exception");
    }

    return ERROR_SUCCESS;
}

// Removes what SyncCodex recorded: the skill (its folder only when nothing else is in it) and the MCP
// server registration.
extern "C" __declspec(dllexport) UINT __stdcall CleanupCodex(MSIHANDLE hInstall)
{
    const std::wstring prefix = L"[installer.ca] CleanupCodex";
    try
    {
        const std::filesystem::path skill_dir(read_state_text(codex_state_key, codex_skill_dir_value));
        if (!skill_dir.empty())
        {
            if (is_agent_skill_dir(skill_dir))
            {
                std::error_code ignored;
                std::filesystem::remove(skill_dir / L"SKILL.md", ignored);
                std::filesystem::remove(skill_dir, ignored);
                log_msi_info(hInstall, prefix + L" skill removed path=" + skill_dir.wstring());
            }
            else
            {
                log_msi_info(hInstall, prefix + L" skill skip=unexpected_path");
            }

            delete_state(codex_state_key, codex_skill_dir_value);
        }

        if (!read_state_text(codex_state_key, codex_mcp_command_value).empty())
        {
            const std::vector<wchar_t> environment = get_user_environment_block();
            const std::filesystem::path codex = find_on_user_path(environment, { L"codex.exe", L"codex.cmd" });
            if (codex.empty())
            {
                log_msi_info(hInstall, prefix + L" mcp_remove skip=codex_command_not_found");
            }
            else if (is_system_profile(get_environment_value(environment, L"USERPROFILE")))
            {
                log_msi_info(hInstall, prefix + L" mcp_remove skip=not_user_context");
            }
            else
            {
                std::filesystem::path working_dir(get_bin_folder(hInstall));
                std::error_code ignored;
                if (!std::filesystem::is_directory(working_dir, ignored))
                {
                    working_dir.clear();
                }

                const int remove_exit = run_user_tool(codex, L"mcp remove bh_agent_debugger", environment, working_dir);
                log_msi_info(hInstall, prefix + L" mcp_remove exit_code=" + std::to_wstring(remove_exit));
            }

            delete_state(codex_state_key, codex_mcp_command_value);
        }

        (void)RegDeleteKeyW(HKEY_CURRENT_USER, codex_state_key);
    }
    catch (...)
    {
        log_msi_info(hInstall, prefix + L" exception");
    }

    return ERROR_SUCCESS;
}

// Codex in the default WSL distribution: copies the skill to ~/.agents/skills and registers the Windows CLI
// as the bh_agent_debugger MCP server. bash runs as an interactive login shell (-lic) so that codex is the
// one the user's terminal finds: a Node version manager is usually set up in ~/.bashrc, which a
// non-interactive shell skips. The script uses the exit codes of describe_codex_wsl_exit.
extern "C" __declspec(dllexport) UINT __stdcall SyncCodexWsl(MSIHANDLE hInstall)
{
    const std::wstring prefix = L"[installer.ca] SyncCodexWsl";
    try
    {
        if (!has_wsl_distros())
        {
            log_msi_info(hInstall, prefix + L" skip=no_distros");
            return ERROR_SUCCESS;
        }

        std::filesystem::path source_root(get_msi_target_path(hInstall, L"OPENCODEFOLDER"));
        if (source_root.empty())
        {
            source_root = std::filesystem::path(get_msi_property(hInstall, L"OPENCODEFOLDER"));
        }

        std::error_code ignored;
        const std::filesystem::path source_skill = source_root / L"skills" / L"bh-agent-debugger" / L"SKILL.md";
        if (source_root.empty() || !std::filesystem::is_regular_file(source_skill, ignored))
        {
            log_msi_info(hInstall, prefix + L" skip=payload_missing");
            return ERROR_SUCCESS;
        }

        const std::filesystem::path cli = std::filesystem::path(get_bin_folder(hInstall)) / L"bh_agent_debugger_cli.exe";
        if (!std::filesystem::is_regular_file(cli, ignored))
        {
            log_msi_info(hInstall, prefix + L" skip=cli_missing");
            return ERROR_SUCCESS;
        }

        // The script puts both paths in single quotes, so a path with a single quote is not supported.
        const std::wstring wsl_skill = windows_to_wsl_path(source_skill);
        const std::wstring wsl_cli = windows_to_wsl_path(cli);
        if (wsl_skill.empty() || wsl_cli.empty()
            || wsl_skill.find(L'\'') != std::wstring::npos || wsl_cli.find(L'\'') != std::wstring::npos)
        {
            log_msi_info(hInstall, prefix + L" skip=unsupported_path");
            return ERROR_SUCCESS;
        }

        const std::wstring wsl_exe = get_system32_executable(L"wsl.exe");
        if (wsl_exe.empty())
        {
            log_msi_info(hInstall, prefix + L" skip=wsl_exe_missing");
            return ERROR_SUCCESS;
        }

        const std::vector<wchar_t> environment = get_user_environment_block();
        if (is_system_profile(get_environment_value(environment, L"USERPROFILE")))
        {
            log_msi_info(hInstall, prefix + L" skip=not_user_context");
            return ERROR_SUCCESS;
        }

        std::wstring script = L"command -v codex >/dev/null 2>&1 || exit 21; ";
        script += L"codex --version >/dev/null 2>&1 || exit 22; ";
        script += L"mkdir -p ~/.agents/skills/bh-agent-debugger && cp -f '" + wsl_skill + L"' ~/.agents/skills/bh-agent-debugger/SKILL.md || exit 23; ";
        script += L"[ -f '" + wsl_cli + L"' ] || exit 24; ";

        // codex mcp add replaces a server of the same name together with the settings the user added to
        // it. So it runs only when the name is free, or when this installer registered another CLI path.
        const std::wstring recorded_command = read_state_text(codex_wsl_state_key, codex_mcp_command_value);
        if (recorded_command.empty())
        {
            script += L"if codex mcp get bh_agent_debugger >/dev/null 2>&1; then exit 25; fi; ";
        }
        else if (recorded_command == wsl_cli)
        {
            script += L"if codex mcp get bh_agent_debugger >/dev/null 2>&1; then exit 20; fi; ";
        }

        script += L"codex mcp add bh_agent_debugger -- '" + wsl_cli + L"' mcp >/dev/null 2>&1 || exit 26; exit 0";

        // -e starts bash directly, so no other shell expands the script first.
        const std::wstring command = quote_for_cmd(wsl_exe) + L" -e bash -lic " + quote_for_cmd(script);
        const int exit_code = run_user_process(command, environment, std::filesystem::path(), codex_wsl_timeout_ms);
        if (exit_code == 0 || exit_code == 20 || exit_code == 24 || exit_code == 25 || exit_code == 26)
        {
            const DWORD installed = 1;
            write_state(codex_wsl_state_key, codex_skill_installed_value, REG_DWORD, &installed, sizeof(installed));
        }

        if (exit_code == 0)
        {
            write_state_text(codex_wsl_state_key, codex_mcp_command_value, wsl_cli);
        }

        log_msi_info(hInstall, prefix + L" exit_code=" + std::to_wstring(exit_code) + L" result=" + describe_codex_wsl_exit(exit_code));
    }
    catch (...)
    {
        log_msi_info(hInstall, prefix + L" exception");
    }

    return ERROR_SUCCESS;
}

// Removes what SyncCodexWsl recorded, and does not start WSL when it recorded nothing.
extern "C" __declspec(dllexport) UINT __stdcall CleanupCodexWsl(MSIHANDLE hInstall)
{
    const std::wstring prefix = L"[installer.ca] CleanupCodexWsl";
    try
    {
        const bool skill_installed = read_state_flag(codex_wsl_state_key, codex_skill_installed_value);
        const bool mcp_added = !read_state_text(codex_wsl_state_key, codex_mcp_command_value).empty();
        if (!skill_installed && !mcp_added)
        {
            (void)RegDeleteKeyW(HKEY_CURRENT_USER, codex_wsl_state_key);
            log_msi_info(hInstall, prefix + L" skip=nothing_recorded");
            return ERROR_SUCCESS;
        }

        const std::wstring wsl_exe = get_system32_executable(L"wsl.exe");
        const std::vector<wchar_t> environment = get_user_environment_block();
        if (wsl_exe.empty() || !has_wsl_distros())
        {
            log_msi_info(hInstall, prefix + L" skip=no_distros");
        }
        else if (is_system_profile(get_environment_value(environment, L"USERPROFILE")))
        {
            log_msi_info(hInstall, prefix + L" skip=not_user_context");
        }
        else
        {
            std::wstring script;
            if (skill_installed)
            {
                script += L"rm -f ~/.agents/skills/bh-agent-debugger/SKILL.md; rmdir ~/.agents/skills/bh-agent-debugger 2>/dev/null; ";
            }

            if (mcp_added)
            {
                script += L"command -v codex >/dev/null 2>&1 || exit 21; ";
                script += L"codex mcp remove bh_agent_debugger >/dev/null 2>&1 || exit 27; ";
            }

            script += L"exit 0";

            const std::wstring command = quote_for_cmd(wsl_exe) + L" -e bash -lic " + quote_for_cmd(script);
            const int exit_code = run_user_process(command, environment, std::filesystem::path(), codex_wsl_timeout_ms);
            log_msi_info(hInstall, prefix + L" exit_code=" + std::to_wstring(exit_code) + L" result=" + describe_codex_wsl_exit(exit_code));
        }

        delete_state(codex_wsl_state_key, codex_skill_installed_value);
        delete_state(codex_wsl_state_key, codex_mcp_command_value);
        (void)RegDeleteKeyW(HKEY_CURRENT_USER, codex_wsl_state_key);
    }
    catch (...)
    {
        log_msi_info(hInstall, prefix + L" exception");
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall SyncOpenCodeWsl(MSIHANDLE hInstall)
{
    try
    {
        if (!has_wsl_distros())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl skip=no_distros");
            return ERROR_SUCCESS;
        }

        std::filesystem::path source_root(get_msi_target_path(hInstall, L"OPENCODEFOLDER"));
        if (source_root.empty())
        {
            source_root = std::filesystem::path(get_msi_property(hInstall, L"OPENCODEFOLDER"));
        }
        if (source_root.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl skip=source_root_empty");
            return ERROR_SUCCESS;
        }

        const std::filesystem::path source_tool = source_root / L"tools" / L"bh_agent_debugger.ts";
        const std::filesystem::path source_skill = source_root / L"skills" / L"bh-agent-debugger" / L"SKILL.md";
        if (!std::filesystem::exists(source_tool) || !std::filesystem::exists(source_skill))
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl skip=payload_missing");
            return ERROR_SUCCESS;
        }

        const std::wstring wsl_root = windows_to_wsl_path(source_root);
        if (wsl_root.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl skip=wsl_root_empty");
            return ERROR_SUCCESS;
        }

        std::wstring script = L"mkdir -p ~/.config/opencode/tools ~/.config/opencode/skills/bh-agent-debugger; ";
        script += L"cp -f '" + wsl_root + L"/tools/bh_agent_debugger.ts' ~/.config/opencode/tools/bh_agent_debugger.ts; ";
        script += L"cp -f '" + wsl_root + L"/skills/bh-agent-debugger/SKILL.md' ~/.config/opencode/skills/bh-agent-debugger/SKILL.md";

        const std::wstring wsl_exe = get_system32_executable(L"wsl.exe");
        if (wsl_exe.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl skip=wsl_exe_missing");
            return ERROR_SUCCESS;
        }

        const std::wstring command = quote_for_cmd(wsl_exe) + L" bash -lc " + quote_for_cmd(script);
        const int exit_code = run_hidden_process(command);
        if (exit_code == 0)
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl synced");
        }
        else
        {
            log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl exit_code=" + std::to_wstring(exit_code));
        }
    }
    catch (...)
    {
        log_msi_info(hInstall, L"[installer.ca] SyncOpenCodeWsl exception");
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall CleanupOpenCodeWsl(MSIHANDLE hInstall)
{
    try
    {
        if (!has_wsl_distros())
        {
            log_msi_info(hInstall, L"[installer.ca] CleanupOpenCodeWsl skip=no_distros");
            return ERROR_SUCCESS;
        }

        const std::wstring wsl_exe = get_system32_executable(L"wsl.exe");
        if (wsl_exe.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] CleanupOpenCodeWsl skip=wsl_exe_missing");
            return ERROR_SUCCESS;
        }

        const std::wstring command =
            quote_for_cmd(wsl_exe) +
            L" bash -lc \"rm -f ~/.config/opencode/tools/bh_agent_debugger.ts; rm -rf ~/.config/opencode/skills/bh-agent-debugger\"";
        const int exit_code = run_hidden_process(command);
        if (exit_code == 0)
        {
            log_msi_info(hInstall, L"[installer.ca] CleanupOpenCodeWsl cleaned");
        }
        else
        {
            log_msi_info(hInstall, L"[installer.ca] CleanupOpenCodeWsl exit_code=" + std::to_wstring(exit_code));
        }
    }
    catch (...)
    {
        log_msi_info(hInstall, L"[installer.ca] CleanupOpenCodeWsl exception");
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall SyncVsExtensions(MSIHANDLE hInstall)
{
    try
    {
        if (is_devenv_running())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncVsExtensions skip=devenv_running");
            return ERROR_SUCCESS;
        }

        std::filesystem::path source_root(get_msi_target_path(hInstall, L"APPLICATIONFOLDER"));
        if (source_root.empty())
        {
            source_root = std::filesystem::path(get_msi_property(hInstall, L"APPLICATIONFOLDER"));
        }
        if (source_root.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncVsExtensions skip=source_root_empty");
            return ERROR_SUCCESS;
        }

        std::filesystem::path vsix_path = source_root / L"extension" / L"bh_agent_debugger.vsix";
        if (!std::filesystem::exists(vsix_path))
        {
            log_msi_info(hInstall, L"[installer.ca] SyncVsExtensions skip=vsix_missing");
            return ERROR_SUCCESS;
        }

        const std::vector<vsix_installer> installers = find_vsix_installers();
        if (installers.empty())
        {
            log_msi_info(hInstall, L"[installer.ca] SyncVsExtensions skip=vsixinstaller_missing");
            return ERROR_SUCCESS;
        }

        // One run per instance, each with that version's own VSIXInstaller.
        for (const auto& installer : installers)
        {
            const std::wstring command =
                quote_for_cmd(installer.path.wstring()) +
                L" /quiet /instanceIds:" + installer.instance_id + L" " +
                quote_for_cmd(vsix_path.wstring());
            const int exit_code = run_hidden_process(command);
            if (exit_code == 0)
            {
                log_msi_info(hInstall, L"[installer.ca] SyncVsExtensions installed_via_vsixinstaller instance=" + installer.instance_id);
            }
            else
            {
                log_msi_info(hInstall, L"[installer.ca] SyncVsExtensions vsixinstaller_exit_code=" + std::to_wstring(exit_code) + L" instance=" + installer.instance_id);
            }
        }

        update_vs_configurations(hInstall, L"SyncVsExtensions", installers);
    }
    catch (...)
    {
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall CleanupVsExtensions(MSIHANDLE hInstall)
{
    try
    {
        if (is_devenv_running())
        {
            return ERROR_SUCCESS;
        }

        const std::vector<vsix_installer> installers = find_vsix_installers();
        for (const auto& installer : installers)
        {
            const std::wstring uninstall_command =
                quote_for_cmd(installer.path.wstring()) +
                L" /quiet /instanceIds:" + installer.instance_id +
                L" /u:boiling-lava-hot-agent-debugger.6A7FD49D-C71A-45AB-A816-8F279F2583E2";
            (void)run_hidden_process(uninstall_command);
        }

        std::filesystem::path vs_root = get_localappdata_vs_root(hInstall);
        std::error_code root_error;
        if (!vs_root.empty() && std::filesystem::exists(vs_root, root_error))
        {
            for (const auto& entry : std::filesystem::directory_iterator(vs_root))
            {
                if (!entry.is_directory())
                {
                    continue;
                }

                const std::wstring name = entry.path().filename().wstring();
                if (!bh_vs_installations::is_supported_hive_name(name))
                {
                    continue;
                }

                std::filesystem::path destination_root = entry.path() / L"Extensions" / L"boiling-lava-hot-agent-debugger.6A7FD49D-C71A-45AB-A816-8F279F2583E2";
                std::error_code ignored;
                std::filesystem::remove_all(destination_root, ignored);
            }
        }

        update_vs_configurations(hInstall, L"CleanupVsExtensions", installers);
    }
    catch (...)
    {
    }

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RequireVsClosed(MSIHANDLE hInstall)
{
    const bool is_scheduled = MsiGetMode(hInstall, MSIRUNMODE_SCHEDULED) ? true : false;
    const HWND parent = get_msi_parent_window(hInstall);
    int ui_level = 5;
    const std::wstring ui_level_text = get_msi_property(hInstall, L"UILevel");
    if (!ui_level_text.empty())
    {
        ui_level = _wtoi(ui_level_text.c_str());
    }

    if (is_scheduled || ui_level < 3)
    {
        if (is_devenv_running_any())
        {
            log_msi_info(hInstall, L"[installer.ca] RequireVsClosed mode=quiet result=fail_vs_open");
            return ERROR_INSTALL_FAILURE;
        }

        log_msi_info(hInstall, L"[installer.ca] RequireVsClosed mode=quiet result=pass_vs_closed");
        return ERROR_SUCCESS;
    }

    log_msi_info(hInstall, L"[installer.ca] RequireVsClosed mode=interactive result=prompt_if_needed");

    set_verifyready_block(hInstall, false);

    while (is_devenv_running_any())
    {
        if (parent != nullptr)
        {
            ShowWindow(parent, SW_SHOWNORMAL);
            SetForegroundWindow(parent);
        }

        const int result = MessageBoxW(
            parent,
            L"Visual Studio is still running. Close all Visual Studio windows, then click Retry. Click Cancel to return to setup.",
            L"BH Agent Debugger Setup",
            MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST | MB_TASKMODAL);

        if (result == IDRETRY)
        {
            log_msi_info(hInstall, L"[installer.ca] RequireVsClosed interactive_choice=retry");
            continue;
        }

        log_msi_info(hInstall, L"[installer.ca] RequireVsClosed interactive_choice=cancel_return_setup");
        set_verifyready_block(hInstall, true);
        return ERROR_SUCCESS;
    }

    set_verifyready_block(hInstall, false);

    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RequireVsClosedExecute(MSIHANDLE hInstall)
{
    if (is_devenv_running_any())
    {
        log_msi_info(hInstall, L"[installer.ca] RequireVsClosedExecute result=fail_vs_open");
        return ERROR_INSTALL_FAILURE;
    }

    log_msi_info(hInstall, L"[installer.ca] RequireVsClosedExecute result=pass_vs_closed");
    return ERROR_SUCCESS;
}
