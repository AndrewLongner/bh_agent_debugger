#include "named_pipe_transport.h"
#include "bh_log.h"
#include "bh_profile_analysis.h"
#include "../../bh_agent_version.h"
#include "bh_mcp_server.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <process.h>
#include <chrono>
#include <thread>
#include <windows.h>
#include <wtsapi32.h>
#include <string>
#include <vector>

#pragma comment(lib, "Wtsapi32.lib")

namespace
{
    std::string escape_json(const std::string& input)
    {
        std::string out;
        out.reserve(input.size() + 16);
        for (char ch : input)
        {
            switch (ch)
            {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
            }
        }

        return out;
    }

    void print_usage()
    {
        std::cout
            << "Usage:\n"
            << "  bh_agent_debugger_cli send <message> [--target-pipe <name>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli send-target <target-id> <message> [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli send-json <json-line> [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli targets-list [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli dte-list [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli doctor [--timeout-ms <n>]   (reports service/cli/vsix versions; 0.12.4+)\n"
            << "  bh_agent_debugger_cli --version   (local only; CLI version and feature flags)\n"
            << "  bh_agent_debugger_cli mcp [--pipe <name>] [--no-auto-start-service]   (MCP stdio server; tools mirror the OpenCode wrapper)\n"
            << "  bh_agent_debugger_cli profile start --pid <debuggee-pid> [--seconds <1..60>] [--output <absolute-dir-or-.diagsession>] [--print-request]\n"
            << "  bh_agent_debugger_cli profile <stop|status|report> [--print-request]\n"
            << "  bh_agent_debugger_cli profile open --target-id vs_<pid> [--print-request]\n"
            << "  bh_agent_debugger_cli profile analyze --input <etl|diagsession> --pid <pid> [--top <1..50>] [--symbols <local-directory>] [--function <exact-name-or-id>]\n"
            << "    Filters: [--start-ms <ms>] [--end-ms <ms>] [--thread <native-tid>] [--subtree <exact-name-or-id>]\n"
            << "    Sections: [--sections functionsBySelf,functionsByInclusive,callTree,hotPaths] (default all; 0.12.4+)\n"
            << "    VS CPU Usage .diagsession capture. Poll status after start/stop. No implicit broker start/elevation.\n"
            << "  bh_agent_debugger_cli chain-run --file <path> [--timeout-ms <n>] [--continue-on-error]\n"
            << "  bh_agent_debugger_cli status [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli modules [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli open-dump <absolute-path.dmp> --target-id <id> [--timeout-ms <n>] [--print-request]\n"
            << "  bh_agent_debugger_cli list-solutions [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli diag-session [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli set-trap <symbol> [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli read-regs [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli read-stack [bytes] [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli local-vars [count] [--target-id <id>] [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli poll-break --target-id <id> [--timeout-ms <n>] [--interval-ms <n>]\n"
            << "  bh_agent_debugger_cli start-debugging <solution-hint> [--args <command-line>] [--timeout-ms <n>] [--print-request]\n"
            << "  bh_agent_debugger_cli attach-debugging <solution-hint> --pid <n> [--timeout-ms <n>] [--print-request]\n"
            << "  bh_agent_debugger_cli stop-debugging <solution-hint> [--timeout-ms <n>]\n"
            << "  bh_agent_debugger_cli start-debugging-chain <hint1> <hint2> ... [--timeout-ms <n>]\n"
            << "\n"
            << "Global options: --pipe <name>, --no-auto-start-service\n"
            << "Default service pipe: bh_agent_ctl\n";
    }

    std::wstring to_wide_ascii(const std::string& text)
    {
        return std::wstring(text.begin(), text.end());
    }

    std::string narrow_utf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return "";
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return "";
        }

        std::string out;
        out.resize(static_cast<size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
        return out;
    }

    bool read_option_value(int argc, char* argv[], const std::string& key, std::string& value)
    {
        for (int i = 0; i < argc - 1; ++i)
        {
            if (key == argv[i])
            {
                value = argv[i + 1];
                return true;
            }
        }

        return false;
    }

    bool has_option(int argc, char* argv[], const std::string& key)
    {
        for (int i = 0; i < argc; ++i)
        {
            if (key == argv[i])
            {
                return true;
            }
        }

        return false;
    }

    std::vector<std::string> split_simple_tokens(const std::string& text)
    {
        std::vector<std::string> tokens;
        std::string current;
        for (char ch : text)
        {
            if (ch == ' ' || ch == '\t')
            {
                if (!current.empty())
                {
                    tokens.push_back(current);
                    current.clear();
                }

                continue;
            }

            current.push_back(ch);
        }

        if (!current.empty())
        {
            tokens.push_back(current);
        }

        return tokens;
    }

    std::string to_lower_ascii(std::string text)
    {
        for (char& ch : text)
        {
            if (ch >= 'A' && ch <= 'Z')
            {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }

        return text;
    }

    bool starts_with_ci(const std::string& text, const std::string& prefix)
    {
        if (text.size() < prefix.size())
        {
            return false;
        }

        const std::string left = to_lower_ascii(text.substr(0, prefix.size()));
        const std::string right = to_lower_ascii(prefix);
        return left == right;
    }

    bool file_exists_w(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::wstring dirname_w(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return std::wstring();
        }

        return path.substr(0, slash);
    }

    bool try_get_service_exe_path(std::wstring& service_exe)
    {
        service_exe.clear();

        wchar_t env_path[1024] = {};
        const DWORD env_len = GetEnvironmentVariableW(L"BH_AGENT_SERVICE_EXE", env_path, static_cast<DWORD>(sizeof(env_path) / sizeof(env_path[0])));
        if (env_len > 0 && env_len < (sizeof(env_path) / sizeof(env_path[0])) && file_exists_w(env_path))
        {
            service_exe = env_path;
            return true;
        }

        wchar_t module_path[MAX_PATH] = {};
        const DWORD module_len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        if (module_len == 0 || module_len >= MAX_PATH)
        {
            return false;
        }

        const std::wstring cli_path(module_path, module_len);
        const std::wstring cli_dir = dirname_w(cli_path);
        const std::wstring installed_candidate = cli_dir + L"\\bh_agent_debugger_service.exe";
        if (file_exists_w(installed_candidate))
        {
            service_exe = installed_candidate;
            return true;
        }

        const std::wstring temp_dir = dirname_w(cli_dir);
        if (temp_dir.empty())
        {
            return false;
        }

        const std::wstring candidate = temp_dir + L"\\bh_agent_debugger_service_x64_Debug\\bh_agent_debugger_service.exe";
        if (file_exists_w(candidate))
        {
            service_exe = candidate;
            return true;
        }

        return false;
    }

    bool try_run_desktop_service_task(std::string& status_text)
    {
        status_text.clear();

        wchar_t task_name_env[512] = {};
        const DWORD env_len = GetEnvironmentVariableW(
            L"BH_AGENT_SERVICE_TASK",
            task_name_env,
            static_cast<DWORD>(sizeof(task_name_env) / sizeof(task_name_env[0])));

        std::wstring task_name = L"bh_agent_debugger_start_service_desktop_user";
        if (env_len > 0 && env_len < (sizeof(task_name_env) / sizeof(task_name_env[0])))
        {
            task_name = task_name_env;
        }

        bh_log::write("cli", "service autostart path=scheduled_task task='" + narrow_utf8(task_name) + "'");

        std::wstring command_line = L"schtasks.exe /Run /TN \"" + task_name + L"\"";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};

        const BOOL ok = CreateProcessW(
            nullptr,
            &command_line[0],
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);
        if (!ok)
        {
            status_text = std::string("schtasks run failed err=") + std::to_string(GetLastError());
            bh_log::write("cli", "service autostart scheduled_task failed: " + status_text);
            return false;
        }

        DWORD wait_result = WaitForSingleObject(pi.hProcess, 8000);
        DWORD exit_code = 1;
        if (wait_result == WAIT_OBJECT_0)
        {
            (void)GetExitCodeProcess(pi.hProcess, &exit_code);
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (wait_result != WAIT_OBJECT_0)
        {
            status_text = "schtasks run timed out";
            bh_log::write("cli", "service autostart scheduled_task failed: " + status_text);
            return false;
        }

        if (exit_code != 0)
        {
            status_text = std::string("schtasks run exit=") + std::to_string(exit_code);
            bh_log::write("cli", "service autostart scheduled_task failed: " + status_text);
            return false;
        }

        status_text = "started_by_schtasks";
        bh_log::write("cli", "service autostart scheduled_task ok");
        return true;
    }

    bool try_start_service_instance(std::string& status_text)
    {
        status_text.clear();

        std::wstring service_exe;
        if (!try_get_service_exe_path(service_exe))
        {
            status_text = "service_exe_not_found";
            return false;
        }

        DWORD self_session = 0;
        DWORD active_console_session = WTSGetActiveConsoleSessionId();
        if (ProcessIdToSessionId(GetCurrentProcessId(), &self_session) &&
            active_console_session != 0xFFFFFFFF)
        {
            if (self_session != active_console_session)
            {
                bh_log::write("cli", "service autostart mode=non_desktop selfSession=" + std::to_string(self_session) + " activeSession=" + std::to_string(active_console_session));
                std::string task_status;
                if (try_run_desktop_service_task(task_status))
                {
                    status_text = task_status;
                    return true;
                }

                status_text = task_status;
                bh_log::write("cli", "service autostart failed without fallback: " + status_text);
                return false;
            }
            else
            {
                bh_log::write("cli", "service autostart mode=desktop_direct session=" + std::to_string(self_session));
            }
        }

        std::wstring command_line = L"\"" + service_exe + L"\"";

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        const BOOL ok = CreateProcessW(
            service_exe.c_str(),
            &command_line[0],
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);
        if (!ok)
        {
            status_text = std::string("CreateProcessW failed err=") + std::to_string(GetLastError());
            bh_log::write("cli", "service autostart desktop_direct failed: " + status_text);
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        status_text = "started";
        bh_log::write("cli", "service autostart desktop_direct ok");
        return true;
    }

    bool send_request_with_service_autostart(
        const std::wstring& pipe,
        const std::string& request,
        std::string& response,
        unsigned int timeout_ms,
        bool allow_autostart,
        bool& autostart_attempted)
    {
        if (bh_agent_transport::send_request(pipe, request, response, timeout_ms))
        {
            return true;
        }

        if (!allow_autostart || autostart_attempted)
        {
            return false;
        }

        autostart_attempted = true;

        std::string start_status;
        if (!try_start_service_instance(start_status))
        {
            bh_log::write("cli", "service autostart failed: " + start_status);
            return false;
        }

        const ULONGLONG start_tick = GetTickCount64();
        while ((GetTickCount64() - start_tick) <= 5000ULL)
        {
            if (bh_agent_transport::send_request(pipe, request, response, timeout_ms))
            {
                bh_log::write("cli", "service autostart succeeded");
                return true;
            }

            Sleep(200);
        }

        return false;
    }

    int run_chain_file(int argc, char* argv[], const std::string& self_exe)
    {
        std::string exe_path = self_exe;
        while (!exe_path.empty() && (exe_path.front() == '"' || exe_path.front() == '\''))
        {
            exe_path.erase(exe_path.begin());
        }
        while (!exe_path.empty() && (exe_path.back() == '"' || exe_path.back() == '\''))
        {
            exe_path.pop_back();
        }

        std::string file_path;
        if (!read_option_value(argc, argv, "--file", file_path) || file_path.empty())
        {
            std::cerr << "chain-run requires --file <path>" << std::endl;
            return 1;
        }

        std::string pipe_name = "bh_agent_ctl";
        std::string pipe_arg;
        if (read_option_value(argc, argv, "--pipe", pipe_arg) && !pipe_arg.empty())
        {
            pipe_name = pipe_arg;
        }

        std::string timeout_ms = "5000";
        std::string timeout_arg;
        if (read_option_value(argc, argv, "--timeout-ms", timeout_arg) && !timeout_arg.empty())
        {
            timeout_ms = timeout_arg;
        }

        const bool continue_on_error = has_option(argc, argv, "--continue-on-error");

        std::ifstream in(file_path);
        if (!in.is_open())
        {
            std::cerr << "failed to open chain file: " << file_path << std::endl;
            return 1;
        }

        std::string line;
        int line_number = 0;
        while (std::getline(in, line))
        {
            ++line_number;

            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
            {
                line.pop_back();
            }

            size_t start = 0;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            {
                ++start;
            }

            if (start >= line.size())
            {
                continue;
            }

            if (line[start] == '#')
            {
                continue;
            }

            const std::string step = line.substr(start);

            std::vector<std::string> args;
            args.push_back(exe_path);

            if (starts_with_ci(step, "start debugging "))
            {
                std::string hint = step.substr(std::string("start debugging ").size());
                while (!hint.empty() && (hint.front() == ' ' || hint.front() == '\t'))
                {
                    hint.erase(hint.begin());
                }

                while (!hint.empty() && (hint.back() == ' ' || hint.back() == '\t'))
                {
                    hint.pop_back();
                }

                if (hint.empty())
                {
                    std::cerr << "chain-run step failed at line " << line_number << ": start debugging requires solution hint" << std::endl;
                    if (!continue_on_error)
                    {
                        return 1;
                    }

                    continue;
                }

                args.push_back("start-debugging");
                args.push_back(hint);
            }
            else if (starts_with_ci(step, "stop debugging "))
            {
                std::string hint = step.substr(std::string("stop debugging ").size());
                while (!hint.empty() && (hint.front() == ' ' || hint.front() == '\t'))
                {
                    hint.erase(hint.begin());
                }

                while (!hint.empty() && (hint.back() == ' ' || hint.back() == '\t'))
                {
                    hint.pop_back();
                }

                if (hint.empty())
                {
                    std::cerr << "chain-run step failed at line " << line_number << ": stop debugging requires solution hint" << std::endl;
                    if (!continue_on_error)
                    {
                        return 1;
                    }

                    continue;
                }

                args.push_back("stop-debugging");
                args.push_back(hint);
            }
            else
            {
                const std::vector<std::string> tokens = split_simple_tokens(step);
                if (tokens.empty())
                {
                    continue;
                }

                for (const std::string& token : tokens)
                {
                    args.push_back(token);
                }
            }

            args.push_back("--timeout-ms");
            args.push_back(timeout_ms);

            std::vector<char*> argv_list;
            argv_list.reserve(args.size() + 1);
            for (std::string& arg : args)
            {
                argv_list.push_back(const_cast<char*>(arg.c_str()));
            }

            argv_list.push_back(nullptr);

            const intptr_t exit_code = _spawnv(_P_WAIT, exe_path.c_str(), argv_list.data());
            if (exit_code != 0)
            {
                std::cerr << "chain-run step failed at line " << line_number << std::endl;
                if (!continue_on_error)
                {
                    return static_cast<int>(exit_code);
                }
            }
        }

        return 0;
    }

}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        print_usage();
        return 1;
    }

    std::string pipe_name = "bh_agent_ctl";
    std::string pipe_arg;
    if (read_option_value(argc, argv, "--pipe", pipe_arg) && !pipe_arg.empty())
    {
        pipe_name = pipe_arg;
    }
    const bool allow_service_autostart = !has_option(argc, argv, "--no-auto-start-service");

    const std::string command = argv[1];
    bh_log::write("cli", "argv command='" + command + "'");

    if (command == "--version" || command == "version")
    {
        std::cout << "bh_agent_debugger_cli " << bh_agent_version::version << " features=" << bh_agent_version::features << std::endl;
        return 0;
    }

    if (command == "mcp")
        return bh_mcp::run(argc, argv);

    if (command == "profile" && argc >= 3 && std::string(argv[2]) == "analyze")
        return run_profile_analysis();

    if (command == "profile")
    {
        // Fail closed on misspelled, duplicated, or debugger-target options.
        if (argc < 3) { print_usage(); return 1; }
        const std::string verb = argv[2];
        const bool start = verb == "start";
        if (!start && verb != "stop" && verb != "status" && verb != "report" && verb != "open")
        { std::cerr << "invalid profile command" << std::endl; return 1; }
        auto decimal = [](const std::string& text, unsigned int maximum, unsigned int& value) -> bool
        {
            if (text.empty() || text.size() > 10) return false;
            unsigned long long n = 0;
            for (char ch : text)
            {
                if (ch < '0' || ch > '9') return false;
                n = n * 10 + static_cast<unsigned>(ch - '0');
                if (n > maximum) return false;
            }
            value = static_cast<unsigned int>(n);
            return value != 0;
        };
        unsigned int pid = 0, seconds = 10, timeout_ms = 30000;
        std::string target_id, output;
        bool print = false;
        std::vector<std::string> seen;
        for (int i = 3; i < argc; ++i)
        {
            const std::string option = argv[i];
            for (const auto& previous : seen)
                if (previous == option) { std::cerr << "duplicate profile option" << std::endl; return 1; }
            seen.push_back(option);
            if (option == "--print-request") { print = true; continue; }
            if (option == "--no-auto-start-service") continue;
            if (++i >= argc) { std::cerr << "missing profile option value" << std::endl; return 1; }
            const std::string value = argv[i];
            bool valid = false;
            if (option == "--pid" && start) valid = decimal(value, 0xffffffffU, pid);
            else if (option == "--seconds" && start) valid = decimal(value, 60, seconds);
            else if (option == "--target-id" && verb == "open")
            {
                unsigned int vs_pid = 0;
                valid = value.rfind("vs_", 0) == 0 && decimal(value.substr(3), 0xffffffffU, vs_pid);
                if (valid) target_id = value;
            }
            else if (option == "--output" && start)
            {
                // Read the wide argument so non-ANSI paths survive; broker validates fully.
                int wide_count = 0;
                LPWSTR* wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
                if (wide_args && wide_count == argc)
                {
                    const std::wstring path = wide_args[i];
                    bool controls = false;
                    for (wchar_t ch : path) controls = controls || ch < 32;
                    const int bytes = path.empty() || controls || path.size() > 200 ? 0 :
                        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
                    if (bytes > 0)
                    {
                        output.assign(bytes, '\0');
                        valid = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()), &output[0], bytes, nullptr, nullptr) == bytes;
                    }
                }
                if (wide_args) LocalFree(wide_args);
            }
            else if (option == "--timeout-ms") valid = decimal(value, 120000, timeout_ms);
            else if (option == "--pipe") valid = !value.empty() && value[0] != '-';
            if (!valid) { std::cerr << "invalid profile option: " << option << std::endl; return 1; }
        }
        if (start && !pid) { std::cerr << "profile start requires --pid <debuggee-pid>" << std::endl; return 1; }
        if (verb == "open" && target_id.empty()) { std::cerr << "profile open requires --target-id vs_<pid>" << std::endl; return 1; }
        const std::string message = "profile " + verb + (start ? " " + std::to_string(pid) + " " + std::to_string(seconds) : "");
        const std::string request = "{\"protocolVersion\":1,\"id\":\"cli-profile\",\"method\":\"profile.command\",\"message\":\"" +
            escape_json(message) + "\",\"timeoutMs\":" + std::to_string(timeout_ms) +
            (target_id.empty() ? "" : ",\"targetId\":\"" + escape_json(target_id) + "\"") +
            (output.empty() ? "" : ",\"output\":\"" + escape_json(output) + "\"") + "}";
        if (print) { std::cout << request << std::endl; return 0; }
        std::string response;
        // Never replace a missing broker and accidentally lose the captured session.
        if (!bh_agent_transport::send_request(to_wide_ascii(pipe_name), request, response, timeout_ms))
        { std::cerr << "profile transport failed; start the intended broker explicitly (VS collection permissions depend on the target)" << std::endl; return 1; }
        std::cout << response << std::endl;
        return response.find("\"ok\":true") != std::string::npos ? 0 : 1;
    }

    if (command == "poll-break")
    {
        std::string target_id;
        if (!read_option_value(argc, argv, "--target-id", target_id) || target_id.empty())
        {
            print_usage();
            return 1;
        }

        unsigned int timeout_ms = 15000;
        std::string timeout_arg;
        if (read_option_value(argc, argv, "--timeout-ms", timeout_arg) && !timeout_arg.empty())
        {
            timeout_ms = static_cast<unsigned int>(std::stoul(timeout_arg));
        }

        unsigned int interval_ms = 200;
        std::string interval_arg;
        if (read_option_value(argc, argv, "--interval-ms", interval_arg) && !interval_arg.empty())
        {
            interval_ms = static_cast<unsigned int>(std::stoul(interval_arg));
        }

        if (interval_ms < 50)
        {
            interval_ms = 50;
        }

        const auto start_time = std::chrono::steady_clock::now();
        const auto deadline = start_time + std::chrono::milliseconds(timeout_ms);
        unsigned int tries = 0;
        std::string last_response;
        bool autostart_attempted = false;

        while (std::chrono::steady_clock::now() <= deadline)
        {
            ++tries;
            const unsigned int request_timeout_ms = (interval_ms < 1000) ? 1000 : interval_ms;
            const std::string request =
                std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"send\",\"targetId\":\"") +
                escape_json(target_id) +
                "\",\"message\":\"status\",\"timeoutMs\":" + std::to_string(request_timeout_ms) + "}";

            std::string response;
            if (!send_request_with_service_autostart(
                    to_wide_ascii(pipe_name),
                    request,
                    response,
                    request_timeout_ms,
                    allow_service_autostart,
                    autostart_attempted))
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                std::cout << "poll.break: error tries=" << tries << " elapsedMs=" << elapsed
                          << " | transport=send_failed" << std::endl;
                return 1;
            }

            last_response = response;
            if (response.rfind("error:", 0) == 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                std::cout << "poll.break: error tries=" << tries << " elapsedMs=" << elapsed
                          << " | last=" << response << std::endl;
                return 1;
            }

            if (response.find("vs break mode") != std::string::npos)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                std::cout << "poll.break: ok tries=" << tries << " elapsedMs=" << elapsed
                          << " | status=" << response << std::endl;
                return 0;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        std::cout << "poll.break: timeout tries=" << tries << " elapsedMs=" << elapsed
                  << " | last=" << (last_response.empty() ? std::string("(none)") : last_response) << std::endl;
        return 1;
    }

    if (command == "chain-run")
    {
        return run_chain_file(argc, argv, argv[0]);
    }

    if (command == "start-debugging" || command == "attach-debugging" || command == "stop-debugging" || command == "start-debugging-chain")
    {
        unsigned int timeout_ms = 5000;
        std::string timeout_arg;
        if (read_option_value(argc, argv, "--timeout-ms", timeout_arg) && !timeout_arg.empty())
        {
            timeout_ms = static_cast<unsigned int>(std::stoul(timeout_arg));
        }

        std::string request;

        if (command == "start-debugging")
        {
            if (argc < 3)
            {
                print_usage();
                return 1;
            }

            std::string launch_arguments;
            const bool has_launch_arguments = read_option_value(argc, argv, "--args", launch_arguments);
            request =
                std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"debug.startHint\",\"solutionHint\":\"") +
                escape_json(argv[2]) +
                "\",\"timeoutMs\":" + std::to_string(timeout_ms) +
                (has_launch_arguments ? std::string(",\"arguments\":\"") + escape_json(launch_arguments) + "\"" : std::string()) + "}";
        }
        else if (command == "attach-debugging")
        {
            std::string process_id_text;
            if (argc < 3 || !read_option_value(argc, argv, "--pid", process_id_text) || process_id_text.empty())
            {
                print_usage();
                return 1;
            }

            bool process_id_is_decimal = !process_id_text.empty();
            unsigned long long process_id = 0;
            for (char ch : process_id_text)
            {
                if (ch < '0' || ch > '9')
                {
                    process_id_is_decimal = false;
                    break;
                }
                process_id = (process_id * 10ULL) + static_cast<unsigned long long>(ch - '0');
                if (process_id > 0xffffffffULL)
                {
                    process_id_is_decimal = false;
                    break;
                }
            }
            if (!process_id_is_decimal || process_id == 0)
            {
                std::cerr << "invalid --pid" << std::endl;
                return 1;
            }

            request =
                std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"debug.attachPid\",\"solutionHint\":\"") +
                escape_json(argv[2]) +
                "\",\"processId\":" + std::to_string(process_id) +
                ",\"timeoutMs\":" + std::to_string(timeout_ms) + "}";
        }
        else if (command == "stop-debugging")
        {
            if (argc < 3)
            {
                print_usage();
                return 1;
            }

            request =
                std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"debug.stopHint\",\"solutionHint\":\"") +
                escape_json(argv[2]) +
                "\",\"timeoutMs\":" + std::to_string(timeout_ms) + "}";
        }
        else if (command == "start-debugging-chain")
        {
            std::vector<std::string> hints;
            for (int i = 2; i < argc; ++i)
            {
                const std::string arg = argv[i];
                if (arg == "--timeout-ms" || arg == "--pipe")
                {
                    ++i;
                    continue;
                }

                if (!arg.empty() && arg[0] == '-')
                {
                    continue;
                }

                hints.push_back(arg);
            }

            if (hints.empty())
            {
                print_usage();
                return 1;
            }

            for (const std::string& hint : hints)
            {
                request =
                    std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"debug.startHint\",\"solutionHint\":\"") +
                    escape_json(hint) +
                    "\",\"timeoutMs\":" + std::to_string(timeout_ms) + "}";

                std::string response;
                bh_log::write("cli", "send request command='start-debugging-chain' pipe='" + pipe_name + "' payload='" + request + "'");
                bool autostart_attempted = false;
                if (!send_request_with_service_autostart(
                        to_wide_ascii(pipe_name),
                        request,
                        response,
                        timeout_ms,
                        allow_service_autostart,
                        autostart_attempted))
                {
                    bh_log::write("cli", "send failed command='start-debugging-chain' pipe='" + pipe_name + "'");
                    std::cerr << "send failed" << std::endl;
                    return 1;
                }

                bh_log::write("cli", "response command='start-debugging-chain' text='" + response + "'");
                std::cout << response << std::endl;
            }

            return 0;
        }

        if (has_option(argc, argv, "--print-request"))
        {
            std::cout << request << std::endl;
            return 0;
        }

        std::string response;
        bh_log::write("cli", "send request command='" + command + "' pipe='" + pipe_name + "' payload='" + request + "'");
        bool autostart_attempted = false;
        if (!send_request_with_service_autostart(
                to_wide_ascii(pipe_name),
                request,
                response,
                timeout_ms,
                allow_service_autostart,
                autostart_attempted))
        {
            bh_log::write("cli", "send failed command='" + command + "' pipe='" + pipe_name + "'");
            std::cerr << "send failed" << std::endl;
            return 1;
        }

        bh_log::write("cli", "response command='" + command + "' text='" + response + "'");
        std::cout << response << std::endl;
        return 0;
    }

    if (command == "send" || command == "send-target" || command == "send-json" ||
        command == "targets-list" || command == "dte-list" || command == "doctor" ||
        command == "status" || command == "list-solutions" || command == "diag-session" ||
        command == "modules" || command == "set-trap" || command == "read-regs" || command == "read-stack" ||
        command == "local-vars" || command == "open-dump")
    {
        if ((command == "send" || command == "send-json" || command == "send-target") && argc < 3)
        {
            print_usage();
            return 1;
        }

        if (command == "send-target" && argc < 4)
        {
            print_usage();
            return 1;
        }

        if (command == "set-trap" && argc < 3)
        {
            print_usage();
            return 1;
        }

        std::string request = "";
        unsigned int timeout_ms = command == "open-dump" ? 30000 : 5000;
        std::string timeout_arg;
        std::string target_id_arg;
        const bool has_target_id = read_option_value(argc, argv, "--target-id", target_id_arg) && !target_id_arg.empty();
        if (read_option_value(argc, argv, "--timeout-ms", timeout_arg) && !timeout_arg.empty())
        {
            timeout_ms = static_cast<unsigned int>(std::stoul(timeout_arg));
        }

        auto build_send_request = [&](const std::string& message, bool allow_target_pipe_override) -> std::string
        {
            std::string built = std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"send\",\"message\":\"") +
                escape_json(message) +
                "\",\"timeoutMs\":" + std::to_string(timeout_ms);

            if (has_target_id)
            {
                built += std::string(",\"targetId\":\"") + escape_json(target_id_arg) + "\"";
            }

            if (allow_target_pipe_override)
            {
                std::string target_pipe_arg;
                const bool has_target_pipe = read_option_value(argc, argv, "--target-pipe", target_pipe_arg) && !target_pipe_arg.empty();
                if (has_target_pipe)
                {
                    built += std::string(",\"pipeName\":\"") + escape_json(target_pipe_arg) + "\"";
                }
            }

            built += "}";
            return built;
        };

        if (command == "open-dump")
        {
            if (argc < 3 || !has_target_id || target_id_arg.rfind("vs_", 0) != 0 ||
                !argv[2][0] || argv[2][0] == '-' || strpbrk(argv[2], "\"\r\n"))
            {
                std::cerr << "open-dump requires a path and explicit --target-id vs_<pid>" << std::endl;
                return 1;
            }
            request = build_send_request(std::string("open dump \"") + argv[2] + "\"", false);
            if (has_option(argc, argv, "--print-request"))
            {
                std::cout << request << std::endl;
                return 0;
            }
        }
        else if (command == "send")
        {
            request = build_send_request(argv[2], true);
        }
        else if (command == "send-target")
        {
            request = std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"send\",\"targetId\":\"") +
                escape_json(argv[2]) +
                "\",\"message\":\"" + escape_json(argv[3]) +
                "\",\"timeoutMs\":" + std::to_string(timeout_ms) + "}";
        }
        else if (command == "send-json")
        {
            request = argv[2];
        }
        else if (command == "targets-list")
        {
            request = std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"targets.list\",\"timeoutMs\":") +
                std::to_string(timeout_ms) + "}";
        }
        else if (command == "dte-list")
        {
            request = std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"dte.list\",\"timeoutMs\":") +
                std::to_string(timeout_ms) + "}";
        }
        else if (command == "doctor")
        {
            request = std::string("{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"health.summary\",\"clientVersion\":\"") +
                bh_agent_version::version + "\",\"timeoutMs\":" + std::to_string(timeout_ms) + "}";
            if (has_option(argc, argv, "--print-request"))
            {
                std::cout << request << std::endl;
                return 0;
            }
        }
        else if (command == "status")
        {
            request = build_send_request("status", false);
        }
        else if (command == "modules")
        {
            request = build_send_request("modules", false);
        }
        else if (command == "list-solutions")
        {
            request = "{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"dte.list\",\"timeoutMs\":" + std::to_string(timeout_ms) + "}";
        }
        else if (command == "diag-session")
        {
            request = "{\"protocolVersion\":1,\"id\":\"cli-1\",\"method\":\"health.summary\",\"timeoutMs\":" + std::to_string(timeout_ms) + "}";
        }
        else if (command == "set-trap")
        {
            request = build_send_request(std::string("set trap symbol ") + argv[2], false);
        }
        else if (command == "read-regs")
        {
            request = build_send_request("read regs", false);
        }
        else if (command == "read-stack")
        {
            std::string stack_message = "read stack";
            if (argc >= 3)
            {
                const std::string possible_bytes = argv[2];
                if (!possible_bytes.empty() && possible_bytes[0] != '-')
                {
                    stack_message += " " + possible_bytes;
                }
            }

            request = build_send_request(stack_message, false);
        }
        else if (command == "local-vars")
        {
            std::string locals_message = "local variables";
            if (argc >= 3)
            {
                const std::string possible_count = argv[2];
                if (!possible_count.empty() && possible_count[0] != '-')
                {
                    locals_message += " " + possible_count;
                }
            }

            request = build_send_request(locals_message, false);
        }

        std::string response;
        bh_log::write("cli", "send request command='" + command + "' pipe='" + pipe_name + "' payload='" + request + "'");
        bool autostart_attempted = false;
        if (!send_request_with_service_autostart(
                to_wide_ascii(pipe_name),
                request,
                response,
                timeout_ms,
                allow_service_autostart,
                autostart_attempted))
        {
            bh_log::write("cli", "send failed command='" + command + "' pipe='" + pipe_name + "'");
            std::cerr << "send failed" << std::endl;
            return 1;
        }

        bh_log::write("cli", "response command='" + command + "' text='" + response + "'");
        std::cout << response << std::endl;
        return 0;
    }

    print_usage();
    return 1;
}
