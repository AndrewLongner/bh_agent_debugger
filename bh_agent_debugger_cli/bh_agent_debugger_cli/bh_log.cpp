#include "bh_log.h"

#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <mutex>

namespace
{
    std::mutex g_log_mutex;

    void ensure_directory(const std::string& path)
    {
        if (path.empty())
        {
            return;
        }

        CreateDirectoryA(path.c_str(), nullptr);
    }

    std::string resolve_log_path()
    {
        char* local_app_data = nullptr;
        size_t local_app_data_len = 0;
        if (_dupenv_s(&local_app_data, &local_app_data_len, "LOCALAPPDATA") == 0 &&
            local_app_data != nullptr && local_app_data[0] != '\0')
        {
            std::string base(local_app_data);
            free(local_app_data);
            for (char& ch : base)
            {
                if (ch == '/')
                {
                    ch = '\\';
                }
            }

            ensure_directory(base);

            std::string app_dir = base;
            app_dir += "\\bh_agent_debugger";
            ensure_directory(app_dir);

            return app_dir + "\\bh_agent_debugger.log";
        }

        if (local_app_data != nullptr)
        {
            free(local_app_data);
        }

        return "bh_agent_debugger.log";
    }
}

namespace bh_log
{
    void write(const char* component, const std::string& message)
    {
        const char* safe_component = (component == nullptr) ? "unknown" : component;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        const DWORD pid = GetCurrentProcessId();

        std::string line;
        line.reserve(message.size() + 96);
        line += std::to_string(st.wYear);
        line += "-";
        if (st.wMonth < 10)
        {
            line += "0";
        }

        line += std::to_string(st.wMonth);
        line += "-";
        if (st.wDay < 10)
        {
            line += "0";
        }

        line += std::to_string(st.wDay);
        line += " ";
        if (st.wHour < 10)
        {
            line += "0";
        }

        line += std::to_string(st.wHour);
        line += ":";
        if (st.wMinute < 10)
        {
            line += "0";
        }

        line += std::to_string(st.wMinute);
        line += ":";
        if (st.wSecond < 10)
        {
            line += "0";
        }

        line += std::to_string(st.wSecond);
        line += ".";
        line += std::to_string(st.wMilliseconds);
        line += " pid=";
        line += std::to_string(pid);
        line += " [";
        line += safe_component;
        line += "] ";
        line += message;

        {
            std::lock_guard<std::mutex> guard(g_log_mutex);
            std::ofstream out(resolve_log_path(), std::ios::app);
            if (out.is_open())
            {
                out << line << "\n";
            }
        }

        line += "\n";
        OutputDebugStringA(line.c_str());
    }
}
