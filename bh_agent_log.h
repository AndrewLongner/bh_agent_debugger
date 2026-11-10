#pragma once

#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

inline void bh_agent_log_append_file(const char* text)
{
    if (!text)
    {
        return;
    }

    wchar_t* local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)) || !local_app_data)
    {
        return;
    }

    wchar_t dir_path[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(dir_path, MAX_PATH, L"%s\\bh_agent_debugger", local_app_data)))
    {
        CoTaskMemFree(local_app_data);
        return;
    }

    CreateDirectoryW(dir_path, nullptr);

    wchar_t file_path[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(file_path, MAX_PATH, L"%s\\bh_agent_debugger.log", dir_path)))
    {
        CoTaskMemFree(local_app_data);
        return;
    }

    CoTaskMemFree(local_app_data);

    HANDLE file = CreateFileW(
        file_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD written = 0;
    WriteFile(file, text, (DWORD)strlen(text), &written, nullptr);
    CloseHandle(file);
}

inline void bh_agent_logf(const char* component, const char* format, ...)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char payload[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(payload, sizeof(payload), _TRUNCATE, format, args);
    va_end(args);

    char line[1400] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\n",
        (unsigned)st.wYear,
        (unsigned)st.wMonth,
        (unsigned)st.wDay,
        (unsigned)st.wHour,
        (unsigned)st.wMinute,
        (unsigned)st.wSecond,
        (unsigned)st.wMilliseconds,
        component ? component : "agent",
        payload);

    OutputDebugStringA(line);
    bh_agent_log_append_file(line);
}

inline void bh_agent_format_message_a(DWORD code, char* buffer, size_t buffer_count)
{
    if (!buffer || buffer_count == 0)
    {
        return;
    }

    buffer[0] = 0;
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD chars = FormatMessageA(
        flags,
        nullptr,
        code,
        0,
        buffer,
        (DWORD)buffer_count,
        nullptr);

    if (chars == 0)
    {
        return;
    }

    while (chars > 0)
    {
        char ch = buffer[chars - 1];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t')
        {
            buffer[chars - 1] = 0;
            --chars;
            continue;
        }

        break;
    }
}

#define bh_agent_check_hr(component, expr)                                                   \
    do                                                                                        \
    {                                                                                         \
        const HRESULT _bh_hr = (expr);                                                       \
        if (FAILED(_bh_hr))                                                                   \
        {                                                                                     \
            const DWORD _bh_last_error = GetLastError();                                     \
            char _bh_message[256] = {};                                                       \
            bh_agent_format_message_a((DWORD)_bh_hr, _bh_message, sizeof(_bh_message));      \
            if (_bh_message[0] == 0 && _bh_last_error != 0)                                  \
            {                                                                                 \
                bh_agent_format_message_a(_bh_last_error, _bh_message, sizeof(_bh_message)); \
            }                                                                                 \
            bh_agent_logf(component,                                                         \
                          "HRESULT failed: %s hr=0x%08X last_error=0x%08X %s (%s:%d)",     \
                          #expr,                                                              \
                          (unsigned)(UINT32)_bh_hr,                                           \
                          (unsigned)(UINT32)_bh_last_error,                                   \
                          _bh_message[0] ? _bh_message : "<no system message>",             \
                          __FILE__,                                                           \
                          __LINE__);                                                          \
            return _bh_hr;                                                                    \
        }                                                                                     \
    } while (0)
