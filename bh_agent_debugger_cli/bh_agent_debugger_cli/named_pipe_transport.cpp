#include "named_pipe_transport.h"
#include "bh_log.h"
#include "../../bh_agent_pipe_security.h"

#include <windows.h>

#include <string>

namespace
{
    std::string narrow_ascii(const std::wstring& text)
    {
        std::string out;
        out.reserve(text.size());
        for (wchar_t ch : text)
        {
            out.push_back(static_cast<char>(ch));
        }

        return out;
    }

    std::wstring make_pipe_path(const std::wstring& pipe_name)
    {
        return L"\\\\.\\pipe\\" + pipe_name;
    }

    bool read_line(HANDLE handle, std::string& text)
    {
        text.clear();
        char ch = 0;
        DWORD bytes_read = 0;

        while (true)
        {
            if (!ReadFile(handle, &ch, 1, &bytes_read, nullptr))
            {
                return false;
            }

            if (bytes_read == 0)
            {
                return false;
            }

            if (ch == '\n')
            {
                return true;
            }

            if (ch != '\r')
            {
                text.push_back(ch);
            }
        }
    }

    bool write_line(HANDLE handle, const std::string& text)
    {
        std::string payload = text;
        payload.push_back('\n');

        DWORD bytes_written = 0;
        return WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &bytes_written, nullptr) != FALSE;
    }
}

namespace bh_agent_transport
{
    bool send_request(
        const std::wstring& pipe_name,
        const std::string& request,
        std::string& response,
        unsigned int connect_timeout_ms)
    {
        const std::wstring pipe_path = make_pipe_path(pipe_name);
        const unsigned int attempts = 3;
        const unsigned int wait_each = (connect_timeout_ms < attempts) ? 1U : (connect_timeout_ms / attempts);

        for (unsigned int attempt = 0; attempt < attempts; ++attempt)
        {
            if (!WaitNamedPipeW(pipe_path.c_str(), wait_each))
            {
                bh_log::write("cli", "WaitNamedPipe failed attempt=" + std::to_string(attempt + 1U) + " err=" + std::to_string(GetLastError()));
                Sleep(50);
                continue;
            }

            HANDLE handle = CreateFileW(
                pipe_path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);

            if (handle == INVALID_HANDLE_VALUE)
            {
                bh_log::write("cli", "CreateFile pipe open failed attempt=" + std::to_string(attempt + 1U) + " err=" + std::to_string(GetLastError()));
                Sleep(50);
                continue;
            }

            // Fail closed if another account created this pipe name.
            if (!bh_agent_pipe_security::server_owned_by_current_user(handle))
            {
                bh_log::write("cli", "pipe rejected: server not owned by current user pipe='" + narrow_ascii(pipe_name) + "'");
                CloseHandle(handle);
                return false;
            }

            bool ok = write_line(handle, request);
            if (ok)
            {
                ok = read_line(handle, response);
            }

            CloseHandle(handle);
            if (ok)
            {
                bh_log::write("cli", "send_request ok pipe='" + narrow_ascii(pipe_name) + "'");
                return true;
            }

            bh_log::write("cli", "send_request attempt failed pipe='" + narrow_ascii(pipe_name) + "' attempt=" + std::to_string(attempt + 1U));

            Sleep(50);
        }

        bh_log::write("cli", "send_request failed pipe='" + narrow_ascii(pipe_name) + "'");
        return false;
    }
}
