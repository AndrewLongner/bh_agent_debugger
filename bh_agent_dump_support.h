#pragma once

#include <windows.h>
#include <dbghelp.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <limits>

#include "bh_agent_eval_grammar.h"

// Pure parsing/formatting and read-only minidump validation shared with the tests.
namespace bh_dump
{
    inline std::string trim(const std::string& text)
    {
        const size_t first = text.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() :
            text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    inline bool command_is(const std::string& command, const char* name)
    {
        const size_t n = strlen(name);
        return command.size() >= n && _strnicmp(command.c_str(), name, n) == 0 &&
            (command.size() == n || command[n] == ' ' || command[n] == '\t');
    }

    inline bool dte_read_only_command(const std::string& command)
    {
        // Match exactly the downstream package grammar, not the more permissive
        // dump parser. A false-positive here could otherwise reach live forwarding.
        // eval --side-effects uses the automation evaluator and is never read-only.
        bool allow_side_effects = false;
        const char* expression = nullptr;
        const bh_eval_grammar::parse_result eval = bh_eval_grammar::parse(command.c_str(), allow_side_effects, expression);
        return (eval != bh_eval_grammar::parse_result::not_eval && !allow_side_effects) ||
            _stricmp(command.c_str(), "locals") == 0 || _strnicmp(command.c_str(), "locals ", 7) == 0 ||
            _stricmp(command.c_str(), "local variables") == 0 || _strnicmp(command.c_str(), "local variables ", 16) == 0 ||
            _stricmp(command.c_str(), "list bp") == 0;
    }

    inline bool parse_uint(const std::string& text, UINT64 maximum, UINT64& value)
    {
        value = 0;
        size_t i = 0;
        unsigned base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            i = 2;
            base = 16;
        }
        if (i == text.size()) return false;
        for (; i < text.size(); ++i)
        {
            const char c = text[i];
            const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
            if (digit >= base || digit > maximum || value > (maximum - digit) / base) return false;
            value = value * base + digit;
        }
        return true;
    }

    inline std::string hex(UINT64 value, unsigned width = 0)
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
        return out.str();
    }

    inline std::string stack_function_name(const std::string& name)
    {
        if (name.empty()) return "<symbols unavailable>";
        const bool prefixed = name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X');
        const size_t digits = name.size() - (prefixed ? 2 : 0);
        UINT64 address = 0;
        if ((digits == 8 || digits == 16) && parse_uint(prefixed ? name : "0x" + name, UINT64_MAX, address))
            return name + " [symbols unavailable]";
        return name;
    }

    inline bool parse_path(const std::string& argument, std::wstring& path)
    {
        std::string text = trim(argument);
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);
        if (text.size() < 7 || text.size() > 32760 || text.find_first_of("\"\r\n\t") != std::string::npos ||
            text.find('\0') != std::string::npos ||
            !((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')) ||
            text[1] != ':' || (text[2] != '\\' && text[2] != '/') ||
            _stricmp(text.c_str() + text.size() - 4, ".dmp") != 0) return false;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
        if (count <= 0) return false;
        path.resize(count);
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), &path[0], count) == count;
    }

    struct dump_info
    {
        UINT16 architecture = 0xffff;
        DWORD exception_code = 0;
        DWORD exception_thread = 0;
        UINT64 exception_address = 0;
    };

    inline bool range_valid(UINT64 offset, UINT64 count, UINT64 size)
    {
        return offset <= size && count <= size - offset;
    }

    inline bool read_at(HANDLE file, UINT64 size, UINT64 offset, void* data, DWORD count)
    {
        if (!range_valid(offset, count, size)) return false;
        LARGE_INTEGER position;
        position.QuadPart = offset;
        DWORD read = 0;
        return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
            ReadFile(file, data, count, &read, nullptr) && read == count;
    }

    inline bool validate_file(const std::wstring& path, dump_info& info, std::string& error)
    {
        info = dump_info();
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = "error: dump file unavailable win32=" + std::to_string(GetLastError());
            return false;
        }
        struct file_closer
        {
            HANDLE value;
            ~file_closer() { CloseHandle(value); }
        } closer = {file};
        LARGE_INTEGER length = {};
        MINIDUMP_HEADER header = {};
        if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 ||
            !read_at(file, length.QuadPart, 0, &header, sizeof(header)) ||
            header.Signature != MINIDUMP_SIGNATURE || (header.Version & 0xffff) != MINIDUMP_VERSION ||
            header.NumberOfStreams == 0 || header.NumberOfStreams > 4096 ||
            !range_valid(header.StreamDirectoryRva, UINT64(header.NumberOfStreams) * sizeof(MINIDUMP_DIRECTORY), length.QuadPart))
        {
            error = "error: invalid or truncated Windows minidump header";
            return false;
        }
        for (DWORD i = 0; i < header.NumberOfStreams; ++i)
        {
            MINIDUMP_DIRECTORY entry = {};
            if (!read_at(file, length.QuadPart, UINT64(header.StreamDirectoryRva) + UINT64(i) * sizeof(entry), &entry, sizeof(entry)) ||
                !range_valid(entry.Location.Rva, entry.Location.DataSize, length.QuadPart))
            {
                error = "error: invalid minidump stream bounds";
                return false;
            }
            if (entry.StreamType == SystemInfoStream)
            {
                MINIDUMP_SYSTEM_INFO system = {};
                if (entry.Location.DataSize < sizeof(system) ||
                    !read_at(file, length.QuadPart, entry.Location.Rva, &system, sizeof(system)))
                {
                    error = "error: truncated minidump system information";
                    return false;
                }
                info.architecture = system.ProcessorArchitecture;
            }
            if (entry.StreamType == ExceptionStream)
            {
                MINIDUMP_EXCEPTION_STREAM exception = {};
                if (entry.Location.DataSize < sizeof(exception) ||
                    !read_at(file, length.QuadPart, entry.Location.Rva, &exception, sizeof(exception)))
                {
                    error = "error: truncated minidump exception information";
                    return false;
                }
                info.exception_code = exception.ExceptionRecord.ExceptionCode;
                info.exception_thread = exception.ThreadId;
                info.exception_address = exception.ExceptionRecord.ExceptionAddress;
            }
        }
        if (info.architecture != PROCESSOR_ARCHITECTURE_INTEL && info.architecture != PROCESSOR_ARCHITECTURE_AMD64)
        {
            error = "error: unsupported dump architecture (native x86/x64 required)";
            return false;
        }
        return true;
    }

    struct memory_request
    {
        UINT64 address = 0;
        UINT32 size = 0;
        unsigned width = 1;
        std::string format = "hex";
    };

    inline bool parse_memory(const std::string& arguments, unsigned pointer_width, memory_request& request)
    {
        std::istringstream input(arguments);
        std::string address, size, extra;
        request = memory_request();
        if (!(input >> address >> size)) return false;
        if (input >> request.format) { if (input >> extra) return false; }
        UINT64 count = 0;
        if (!parse_uint(address, UINT64_MAX, request.address) || !parse_uint(size, 4096, count) || count == 0 ||
            request.address > UINT64_MAX - (count - 1) || (pointer_width == 4 && request.address + count - 1 > UINT32_MAX)) return false;
        if (request.format == "u32") request.width = 4;
        else if (request.format == "u64") request.width = 8;
        else if (request.format == "ptr") request.width = pointer_width;
        else if (request.format != "hex" && request.format != "ascii") return false;
        if ((pointer_width != 4 && pointer_width != 8) || count % request.width != 0) return false;
        request.size = static_cast<UINT32>(count);
        return true;
    }

    inline std::string format_memory(const BYTE* data, UINT32 size, const memory_request& request)
    {
        std::ostringstream out;
        if (request.format == "ascii")
        {
            for (UINT32 i = 0; i < size; ++i) out << (data[i] >= 32 && data[i] <= 126 ? char(data[i]) : '.');
        }
        else
        {
            for (UINT32 i = 0; i + request.width <= size; i += request.width)
            {
                UINT64 value = 0;
                for (unsigned j = 0; j < request.width; ++j) value |= UINT64(data[i + j]) << (j * 8);
                if (i) out << ' ';
                out << hex(value, request.width * 2);
            }
        }
        return out.str();
    }
}
