#pragma once
// MCP (Model Context Protocol) stdio server:
//   bh_agent_debugger_cli mcp [--pipe <name>] [--no-auto-start-service]
// Reads newline-delimited JSON-RPC 2.0 messages from stdin and writes only protocol messages to
// stdout. Each tools/call runs this same CLI as a child process with the arguments the OpenCode
// wrapper (opencode/tools/bh_agent_debugger.ts) uses, so both front ends share one validated
// command path and broker transport. Requests are handled one at a time, in arrival order.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "bh_log.h"
#include "../../bh_agent_version.h"

namespace bh_mcp
{
    const size_t max_message_bytes = 4 * 1024 * 1024;
    const size_t max_capture_bytes = 4 * 1024 * 1024;
    const int max_json_depth = 64;
    const size_t max_object_members = 4096;
    const unsigned long long max_timeout_ms = 600000;
    const DWORD child_grace_ms = 30000;

    // ---- JSON -----------------------------------------------------------------------------

    struct json_value
    {
        enum class kind { null_value, boolean, number, string, array, object };
        kind type = kind::null_value;
        bool flag = false;
        double number = 0.0;
        std::string text; // string contents, or the exact source text of a number
        std::vector<json_value> items;
        std::vector<std::string> keys;
        std::vector<json_value> values;

        const json_value* find(const std::string& key) const
        {
            if (type != kind::object) return nullptr;
            for (size_t i = 0; i < keys.size(); ++i)
                if (keys[i] == key) return &values[i];
            return nullptr;
        }
    };

    // Length of the valid UTF-8 sequence at text[at], or 0 when it is not valid UTF-8.
    inline size_t utf8_sequence_length(const std::string& text, size_t at)
    {
        const unsigned char lead = static_cast<unsigned char>(text[at]);
        if (lead < 0x80) return 1;
        size_t length = 0;
        unsigned long code = 0;
        if (lead >= 0xC2 && lead <= 0xDF) { length = 2; code = lead & 0x1Fu; }
        else if (lead >= 0xE0 && lead <= 0xEF) { length = 3; code = lead & 0x0Fu; }
        else if (lead >= 0xF0 && lead <= 0xF4) { length = 4; code = lead & 0x07u; }
        else return 0;
        if (text.size() - at < length) return 0;
        for (size_t i = 1; i < length; ++i)
        {
            const unsigned char next = static_cast<unsigned char>(text[at + i]);
            if ((next & 0xC0u) != 0x80u) return 0;
            code = (code << 6) | (next & 0x3Fu);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF)) ||
            (code >= 0xD800 && code <= 0xDFFF))
            return 0;
        return length;
    }

    inline void append_utf8(std::string& out, unsigned long code)
    {
        if (code < 0x80)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else if (code < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    // Strict RFC 8259 parser: rejects invalid UTF-8, lone surrogates, duplicate member names,
    // trailing data and nesting deeper than max_json_depth.
    class json_parser
    {
    public:
        explicit json_parser(const std::string& text) : text_(text), pos_(0) {}

        bool parse(json_value& out)
        {
            skip_space();
            if (!parse_value(out, 0)) return false;
            skip_space();
            return pos_ == text_.size();
        }

    private:
        const std::string& text_;
        size_t pos_;

        bool at_end() const { return pos_ >= text_.size(); }
        char peek() const { return text_[pos_]; }
        static bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

        void skip_space()
        {
            while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
        }

        bool literal(const char* word)
        {
            const size_t length = std::strlen(word);
            if (text_.compare(pos_, length, word) != 0) return false;
            pos_ += length;
            return true;
        }

        bool hex4(unsigned long& value)
        {
            if (text_.size() - pos_ < 4) return false;
            value = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char ch = text_[pos_++];
                value <<= 4;
                if (ch >= '0' && ch <= '9') value |= static_cast<unsigned long>(ch - '0');
                else if (ch >= 'a' && ch <= 'f') value |= static_cast<unsigned long>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') value |= static_cast<unsigned long>(ch - 'A' + 10);
                else return false;
            }
            return true;
        }

        bool parse_string(std::string& out)
        {
            if (at_end() || peek() != '"') return false;
            ++pos_;
            out.clear();
            while (!at_end())
            {
                const unsigned char ch = static_cast<unsigned char>(peek());
                if (ch == '"')
                {
                    ++pos_;
                    return true;
                }
                if (ch < 0x20) return false;
                if (ch != '\\')
                {
                    const size_t length = utf8_sequence_length(text_, pos_);
                    if (!length) return false;
                    out.append(text_, pos_, length);
                    pos_ += length;
                    continue;
                }
                if (++pos_ >= text_.size()) return false;
                const char escape = text_[pos_++];
                switch (escape)
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    unsigned long code = 0;
                    if (!hex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF)
                    {
                        unsigned long low = 0;
                        if (text_.size() - pos_ < 2 || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') return false;
                        pos_ += 2;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else if (code >= 0xDC00 && code <= 0xDFFF)
                    {
                        return false;
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    return false;
                }
            }
            return false;
        }

        bool parse_number(json_value& out)
        {
            const size_t start = pos_;
            if (!at_end() && peek() == '-') ++pos_;
            if (at_end()) return false;
            if (peek() == '0') ++pos_;
            else if (peek() >= '1' && peek() <= '9') { while (!at_end() && is_digit(peek())) ++pos_; }
            else return false;
            if (!at_end() && peek() == '.')
            {
                ++pos_;
                if (at_end() || !is_digit(peek())) return false;
                while (!at_end() && is_digit(peek())) ++pos_;
            }
            if (!at_end() && (peek() == 'e' || peek() == 'E'))
            {
                ++pos_;
                if (!at_end() && (peek() == '+' || peek() == '-')) ++pos_;
                if (at_end() || !is_digit(peek())) return false;
                while (!at_end() && is_digit(peek())) ++pos_;
            }
            out.type = json_value::kind::number;
            out.text = text_.substr(start, pos_ - start);
            out.number = std::strtod(out.text.c_str(), nullptr);
            return std::isfinite(out.number);
        }

        bool parse_array(json_value& out, int depth)
        {
            out.type = json_value::kind::array;
            ++pos_;
            skip_space();
            if (!at_end() && peek() == ']')
            {
                ++pos_;
                return true;
            }
            for (;;)
            {
                out.items.emplace_back();
                skip_space();
                if (!parse_value(out.items.back(), depth + 1)) return false;
                skip_space();
                if (at_end()) return false;
                if (peek() == ',') { ++pos_; continue; }
                if (peek() == ']') { ++pos_; return true; }
                return false;
            }
        }

        bool parse_object(json_value& out, int depth)
        {
            out.type = json_value::kind::object;
            ++pos_;
            skip_space();
            if (!at_end() && peek() == '}')
            {
                ++pos_;
                return true;
            }
            for (;;)
            {
                skip_space();
                std::string key;
                if (!parse_string(key)) return false;
                // Duplicate names are ambiguous across JSON implementations: reject them.
                if (out.find(key) || out.keys.size() >= max_object_members) return false;
                skip_space();
                if (at_end() || peek() != ':') return false;
                ++pos_;
                skip_space();
                out.keys.push_back(key);
                out.values.emplace_back();
                if (!parse_value(out.values.back(), depth + 1)) return false;
                skip_space();
                if (at_end()) return false;
                if (peek() == ',') { ++pos_; continue; }
                if (peek() == '}') { ++pos_; return true; }
                return false;
            }
        }

        bool parse_value(json_value& out, int depth)
        {
            if (depth > max_json_depth || at_end()) return false;
            const char ch = peek();
            if (ch == '{') return parse_object(out, depth);
            if (ch == '[') return parse_array(out, depth);
            if (ch == '"')
            {
                out.type = json_value::kind::string;
                return parse_string(out.text);
            }
            if (ch == 't') { out.type = json_value::kind::boolean; out.flag = true; return literal("true"); }
            if (ch == 'f') { out.type = json_value::kind::boolean; out.flag = false; return literal("false"); }
            if (ch == 'n') { out.type = json_value::kind::null_value; return literal("null"); }
            return parse_number(out);
        }
    };

    // Replaces invalid UTF-8 (for example a code-page message from a child process) with U+FFFD.
    inline std::string sanitize_utf8(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            const size_t length = utf8_sequence_length(text, i);
            if (length)
            {
                out.append(text, i, length);
                i += length;
            }
            else
            {
                out += "\xEF\xBF\xBD";
                ++i;
            }
        }
        return out;
    }

    // JSON string literal for valid UTF-8 text. Control characters are always escaped, so a
    // serialized message never contains a raw newline.
    inline std::string quote(const std::string& text)
    {
        std::string out = "\"";
        for (char raw : text)
        {
            const unsigned char ch = static_cast<unsigned char>(raw);
            switch (ch)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20 || ch == 0x7F)
                {
                    char escaped[8] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(ch));
                    out += escaped;
                }
                else
                {
                    out.push_back(raw);
                }
                break;
            }
        }
        out.push_back('"');
        return out;
    }

    // ---- Text conversion ------------------------------------------------------------------

    inline bool utf8_to_wide(const std::string& text, std::wstring& out)
    {
        out.clear();
        if (text.empty()) return true;
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0) return false;
        out.assign(static_cast<size_t>(length), L'\0');
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &out[0], length) == length;
    }

    // Most CLI commands read the narrow (ANSI code page) argv. Allow only text that the code page
    // represents exactly: never rely on best-fit mapping, which can turn a character into a quote.
    inline bool fits_ansi_argv(const std::wstring& text)
    {
        if (text.empty() || GetACP() == CP_UTF8) return true;
        BOOL used_default = FALSE;
        const int length = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, &used_default);
        if (length <= 0 || used_default) return false;
        std::string narrow(static_cast<size_t>(length), '\0');
        if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(), static_cast<int>(text.size()), &narrow[0], length, nullptr, &used_default) != length || used_default)
            return false;
        const int wide_length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow.data(), length, nullptr, 0);
        if (wide_length <= 0) return false;
        std::wstring round_trip(static_cast<size_t>(wide_length), L'\0');
        return MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow.data(), length, &round_trip[0], wide_length) == wide_length &&
            round_trip == text;
    }

    // Quotes one argument so CommandLineToArgvW and the CRT both read it back unchanged.
    inline std::wstring quote_argument(const std::wstring& text)
    {
        std::wstring result = L"\"";
        size_t slashes = 0;
        for (wchar_t ch : text)
        {
            if (ch == L'\\')
            {
                ++slashes;
                continue;
            }
            result.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
        result.append(2 * slashes, L'\\');
        result.push_back(L'"');
        return result;
    }

    inline std::string trim(const std::string& text)
    {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) ++begin;
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
        return text.substr(begin, end - begin);
    }

    // ---- Tool arguments -------------------------------------------------------------------

    enum : unsigned
    {
        text_positional = 1u,  // a positional CLI argument: must not look like an option
        text_ansi_argv = 2u,   // the CLI reads it from the narrow argv
        text_target_id = 4u,   // letters, digits, '.', '_' and '-' only
        text_allow_empty = 8u,
    };

    struct tool_call
    {
        std::vector<std::wstring> args; // CLI arguments after the executable path
        DWORD wait_ms = 0;
    };

    inline bool only(const json_value& arguments, std::initializer_list<const char*> allowed, std::string& error)
    {
        for (const std::string& key : arguments.keys)
        {
            bool known = false;
            for (const char* name : allowed) known = known || key == name;
            if (!known)
            {
                error = "unknown argument \"" + key + "\"";
                return false;
            }
        }
        return true;
    }

    inline bool check_text(const std::string& name, const std::string& text, size_t max_length, unsigned flags,
        std::wstring& value, std::string& error)
    {
        if (text.empty() && !(flags & text_allow_empty))
        {
            error = name + " must not be empty";
            return false;
        }
        for (char raw : text)
        {
            const unsigned char ch = static_cast<unsigned char>(raw);
            if (ch < 0x20 || ch == 0x7F)
            {
                error = name + " must not contain control characters";
                return false;
            }
            const bool id_char = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                ch == '.' || ch == '_' || ch == '-';
            if ((flags & text_target_id) && !id_char)
            {
                error = name + " must use only letters, digits, '.', '_' and '-'";
                return false;
            }
        }
        if ((flags & (text_positional | text_target_id)) && !text.empty() && text[0] == '-')
        {
            error = name + " must not start with '-'";
            return false;
        }
        if (!utf8_to_wide(text, value))
        {
            error = name + " is not valid UTF-8";
            return false;
        }
        if (value.size() > max_length)
        {
            error = name + " must be at most " + std::to_string(max_length) + " characters";
            return false;
        }
        if ((flags & text_ansi_argv) && !fits_ansi_argv(value))
        {
            error = name + " contains characters that the Windows ANSI code page cannot pass to the CLI exactly";
            return false;
        }
        return true;
    }

    inline bool read_text(const json_value& arguments, const char* name, bool required, size_t max_length,
        unsigned flags, std::wstring& value, bool& present, std::string& error)
    {
        const json_value* item = arguments.find(name);
        present = item != nullptr;
        if (!item)
        {
            if (required) error = std::string(name) + " is required";
            return !required;
        }
        if (item->type != json_value::kind::string)
        {
            error = std::string(name) + " must be a string";
            return false;
        }
        return check_text(name, item->text, max_length, flags, value, error);
    }

    inline bool read_integer(const json_value& arguments, const char* name, bool required,
        unsigned long long minimum, unsigned long long maximum, unsigned long long& value, bool& present, std::string& error)
    {
        const json_value* item = arguments.find(name);
        present = item != nullptr;
        if (!item)
        {
            if (required) error = std::string(name) + " is required";
            return !required;
        }
        if (item->type != json_value::kind::number || std::floor(item->number) != item->number ||
            item->number < static_cast<double>(minimum) || item->number > static_cast<double>(maximum))
        {
            error = std::string(name) + " must be an integer from " + std::to_string(minimum) + " to " + std::to_string(maximum);
            return false;
        }
        value = static_cast<unsigned long long>(item->number);
        return true;
    }

    // Trace-relative milliseconds for profile analysis, passed on as the client's own decimal text.
    inline bool read_milliseconds(const json_value& arguments, const char* name, std::wstring& value, bool& present, std::string& error)
    {
        const json_value* item = arguments.find(name);
        present = item != nullptr;
        if (!item) return true;
        if (item->type != json_value::kind::number || !(item->number >= 0.0) || item->number > 1e12)
        {
            error = std::string(name) + " must be a number from 0 to 1000000000000";
            return false;
        }
        const std::string text = item->number == 0.0 ? std::string("0") : item->text;
        value.assign(text.begin(), text.end());
        return true;
    }

    inline bool read_timeout(const json_value& arguments, unsigned long long maximum, unsigned long long& timeout_ms, std::string& error)
    {
        bool present = false;
        return read_integer(arguments, "timeoutMs", false, 1, maximum, timeout_ms, present, error);
    }

    inline DWORD child_wait(unsigned long long timeout_ms, unsigned long long requests)
    {
        return static_cast<DWORD>(timeout_ms * requests + child_grace_ms);
    }

    // The CLI finds options anywhere in argv, so a free-form value must not equal an option name.
    inline bool is_cli_option(const std::wstring& text)
    {
        static const wchar_t* const options[] = {
            L"--pipe", L"--no-auto-start-service", L"--timeout-ms", L"--target-id", L"--target-pipe",
            L"--print-request", L"--interval-ms", L"--args", L"--pid", L"--file", L"--continue-on-error",
            L"--seconds", L"--output" };
        for (const wchar_t* option : options)
            if (text == option) return true;
        return false;
    }

    // ---- Tools (mirror opencode/tools/bh_agent_debugger.ts) -------------------------------

    inline bool build_listing(const wchar_t* command, const json_value& a, tool_call& call, std::string& e)
    {
        unsigned long long timeout = 5000;
        if (!only(a, { "timeoutMs" }, e) || !read_timeout(a, max_timeout_ms, timeout, e)) return false;
        call.args = { command, L"--timeout-ms", std::to_wstring(timeout) };
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    inline bool build_doctor(const json_value& a, tool_call& call, std::string& e) { return build_listing(L"doctor", a, call, e); }
    inline bool build_targets(const json_value& a, tool_call& call, std::string& e) { return build_listing(L"targets-list", a, call, e); }
    inline bool build_dte(const json_value& a, tool_call& call, std::string& e) { return build_listing(L"dte-list", a, call, e); }

    inline bool build_modules(const json_value& a, tool_call& call, std::string& e)
    {
        std::wstring target;
        unsigned long long timeout = 5000;
        bool present = false;
        if (!only(a, { "targetId", "timeoutMs" }, e) ||
            !read_text(a, "targetId", true, 64, text_target_id | text_ansi_argv, target, present, e) ||
            !read_timeout(a, max_timeout_ms, timeout, e))
            return false;
        call.args = { L"modules", L"--target-id", target, L"--timeout-ms", std::to_wstring(timeout) };
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    inline bool build_profile(const json_value& a, tool_call& call, std::string& e)
    {
        std::wstring action, target, output;
        unsigned long long pid = 0, seconds = 0, timeout = 30000;
        bool has_action = false, has_pid = false, has_seconds = false, has_target = false, has_output = false;
        if (!only(a, { "action", "processId", "seconds", "targetId", "output", "timeoutMs" }, e) ||
            !read_text(a, "action", true, 16, text_positional, action, has_action, e) ||
            !read_integer(a, "processId", false, 1, 4294967295ULL, pid, has_pid, e) ||
            !read_integer(a, "seconds", false, 1, 60, seconds, has_seconds, e) ||
            !read_text(a, "targetId", false, 64, text_target_id, target, has_target, e) ||
            !read_text(a, "output", false, 200, 0, output, has_output, e) ||
            !read_timeout(a, 120000, timeout, e))
            return false;
        if (action != L"start" && action != L"stop" && action != L"status" && action != L"report" && action != L"open")
        {
            e = "action must be start, stop, status, report or open";
            return false;
        }
        const bool start = action == L"start";
        if (start ? !has_pid : (has_pid || has_seconds || has_output))
        {
            e = "processId is required only for start; seconds and output are start-only";
            return false;
        }
        if ((action == L"open") != has_target)
        {
            e = "targetId is required only for open";
            return false;
        }
        if (has_target && (target.size() <= 3 || target.compare(0, 3, L"vs_") != 0 ||
            target.find_first_not_of(L"0123456789", 3) != std::wstring::npos))
        {
            e = "targetId must be vs_<pid>";
            return false;
        }
        if (has_output && output.size() < 3)
        {
            e = "output must be at least 3 characters";
            return false;
        }
        call.args = { L"profile", action };
        if (has_pid) { call.args.push_back(L"--pid"); call.args.push_back(std::to_wstring(pid)); }
        if (has_seconds) { call.args.push_back(L"--seconds"); call.args.push_back(std::to_wstring(seconds)); }
        if (has_target) { call.args.push_back(L"--target-id"); call.args.push_back(target); }
        if (has_output) { call.args.push_back(L"--output"); call.args.push_back(output); } // the CLI reads it as UTF-16
        call.args.push_back(L"--timeout-ms");
        call.args.push_back(std::to_wstring(timeout));
        call.args.push_back(L"--no-auto-start-service");
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    inline bool build_profile_analyze(const json_value& a, tool_call& call, std::string& e)
    {
        // The analysis helper reads these as UTF-16 and validates them again.
        std::wstring input, symbols, function_name, subtree, start_ms, end_ms;
        unsigned long long pid = 0, top = 20, thread = 0;
        bool has_input = false, has_pid = false, has_top = false, has_symbols = false, has_function = false;
        bool has_start = false, has_end = false, has_thread = false, has_subtree = false;
        if (!only(a, { "input", "processId", "top", "symbols", "function", "startMs", "endMs", "threadId", "subtree", "sections" }, e) ||
            !read_text(a, "input", true, 32767, 0, input, has_input, e) ||
            !read_integer(a, "processId", true, 1, 2147483647ULL, pid, has_pid, e) ||
            !read_integer(a, "top", false, 1, 50, top, has_top, e) ||
            !read_text(a, "symbols", false, 32767, 0, symbols, has_symbols, e) ||
            !read_text(a, "function", false, 1024, 0, function_name, has_function, e) ||
            !read_milliseconds(a, "startMs", start_ms, has_start, e) ||
            !read_milliseconds(a, "endMs", end_ms, has_end, e) ||
            !read_integer(a, "threadId", false, 1, 2147483647ULL, thread, has_thread, e) ||
            !read_text(a, "subtree", false, 1024, 0, subtree, has_subtree, e))
            return false;
        std::wstring sections;
        const json_value* section_list = a.find("sections");
        if (section_list)
        {
            static const char* const known[] = { "functionsBySelf", "functionsByInclusive", "callTree", "hotPaths" };
            if (section_list->type != json_value::kind::array || section_list->items.empty() || section_list->items.size() > 4)
            {
                e = "sections must be an array of 1 to 4 section names";
                return false;
            }
            for (size_t i = 0; i < section_list->items.size(); ++i)
            {
                const json_value& item = section_list->items[i];
                bool valid = item.type == json_value::kind::string;
                bool listed = false;
                for (const char* name : known) listed = listed || (valid && item.text == name);
                for (size_t j = 0; j < i; ++j) valid = valid && section_list->items[j].text != item.text;
                if (!valid || !listed)
                {
                    e = "sections must name distinct sections: functionsBySelf, functionsByInclusive, callTree, hotPaths";
                    return false;
                }
                if (!sections.empty()) sections += L",";
                sections.append(item.text.begin(), item.text.end());
            }
        }
        call.args = { L"profile", L"analyze", L"--input", input, L"--pid", std::to_wstring(pid), L"--top", std::to_wstring(top) };
        if (has_symbols) { call.args.push_back(L"--symbols"); call.args.push_back(symbols); }
        if (has_function) { call.args.push_back(L"--function"); call.args.push_back(function_name); }
        if (has_start) { call.args.push_back(L"--start-ms"); call.args.push_back(start_ms); }
        if (has_end) { call.args.push_back(L"--end-ms"); call.args.push_back(end_ms); }
        if (has_thread) { call.args.push_back(L"--thread"); call.args.push_back(std::to_wstring(thread)); }
        if (has_subtree) { call.args.push_back(L"--subtree"); call.args.push_back(subtree); }
        if (section_list) { call.args.push_back(L"--sections"); call.args.push_back(sections); }
        call.wait_ms = 120000 + child_grace_ms; // the CLI stops the helper after 120 seconds
        return true;
    }

    inline bool build_open_dump(const json_value& a, tool_call& call, std::string& e)
    {
        std::wstring target, path;
        unsigned long long timeout = 30000;
        bool present = false;
        if (!only(a, { "targetId", "path", "timeoutMs" }, e) ||
            !read_text(a, "targetId", true, 64, text_target_id | text_ansi_argv, target, present, e) ||
            !read_text(a, "path", true, 32767, text_positional | text_ansi_argv, path, present, e) ||
            !read_timeout(a, max_timeout_ms, timeout, e))
            return false;
        if (path.find(L'"') != std::wstring::npos)
        {
            e = "path must not contain '\"'";
            return false;
        }
        call.args = { L"open-dump", path, L"--target-id", target, L"--timeout-ms", std::to_wstring(timeout) };
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    inline bool build_start(const json_value& a, tool_call& call, std::string& e)
    {
        std::wstring hint, launch_arguments;
        unsigned long long timeout = 15000;
        bool present = false, has_arguments = false;
        if (!only(a, { "hint", "arguments", "timeoutMs" }, e) ||
            !read_text(a, "hint", true, 1024, text_positional | text_ansi_argv, hint, present, e) ||
            !read_text(a, "arguments", false, 30000, text_allow_empty | text_ansi_argv, launch_arguments, has_arguments, e) ||
            !read_timeout(a, max_timeout_ms, timeout, e))
            return false;
        if (has_arguments && is_cli_option(launch_arguments))
        {
            e = "arguments must not be exactly a CLI option name such as --timeout-ms";
            return false;
        }
        call.args = { L"start-debugging", hint };
        if (has_arguments) { call.args.push_back(L"--args"); call.args.push_back(launch_arguments); }
        call.args.push_back(L"--timeout-ms");
        call.args.push_back(std::to_wstring(timeout));
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    inline bool build_attach(const json_value& a, tool_call& call, std::string& e)
    {
        std::wstring hint;
        unsigned long long pid = 0, timeout = 15000;
        bool present = false;
        if (!only(a, { "hint", "processId", "timeoutMs" }, e) ||
            !read_text(a, "hint", true, 1024, text_positional | text_ansi_argv, hint, present, e) ||
            !read_integer(a, "processId", true, 1, 4294967295ULL, pid, present, e) ||
            !read_timeout(a, max_timeout_ms, timeout, e))
            return false;
        call.args = { L"attach-debugging", hint, L"--pid", std::to_wstring(pid), L"--timeout-ms", std::to_wstring(timeout) };
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    inline bool build_stop(const json_value& a, tool_call& call, std::string& e)
    {
        std::wstring hint;
        unsigned long long timeout = 15000;
        bool present = false;
        if (!only(a, { "hint", "timeoutMs" }, e) ||
            !read_text(a, "hint", true, 1024, text_positional | text_ansi_argv, hint, present, e) ||
            !read_timeout(a, max_timeout_ms, timeout, e))
            return false;
        call.args = { L"stop-debugging", hint, L"--timeout-ms", std::to_wstring(timeout) };
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    inline bool build_chain(const json_value& a, tool_call& call, std::string& e)
    {
        unsigned long long timeout = 30000;
        if (!only(a, { "hints", "timeoutMs" }, e) || !read_timeout(a, max_timeout_ms, timeout, e)) return false;
        const json_value* hints = a.find("hints");
        if (!hints || hints->type != json_value::kind::array || hints->items.empty() || hints->items.size() > 16)
        {
            e = "hints must be an array of 1 to 16 solution hints";
            return false;
        }
        call.args = { L"start-debugging-chain" };
        for (const json_value& item : hints->items)
        {
            std::wstring hint;
            if (item.type != json_value::kind::string)
            {
                e = "hints must contain only strings";
                return false;
            }
            if (!check_text("hints[]", item.text, 1024, text_positional | text_ansi_argv, hint, e)) return false;
            call.args.push_back(hint);
        }
        call.args.push_back(L"--timeout-ms");
        call.args.push_back(std::to_wstring(timeout));
        call.wait_ms = child_wait(timeout, hints->items.size());
        return true;
    }

    inline bool build_send(const json_value& a, tool_call& call, std::string& e)
    {
        std::wstring target, command;
        unsigned long long timeout = 15000;
        bool present = false;
        if (!only(a, { "targetId", "command", "timeoutMs" }, e) ||
            !read_text(a, "targetId", true, 64, text_target_id | text_ansi_argv, target, present, e) ||
            !read_text(a, "command", true, 8192, text_positional | text_ansi_argv, command, present, e) ||
            !read_timeout(a, max_timeout_ms, timeout, e))
            return false;
        call.args = { L"send-target", target, command, L"--timeout-ms", std::to_wstring(timeout) };
        call.wait_ms = child_wait(timeout, 1);
        return true;
    }

    enum class forwarding { all, pipe_only, none };

    struct tool_definition
    {
        const char* name;
        const char* description;
        const char* input_schema;
        bool read_only;
        forwarding forward;
        bool (*build)(const json_value& arguments, tool_call& call, std::string& error);
    };

#define BH_MCP_TIMEOUT_PROPERTY "\"timeoutMs\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000,\"description\":\"Timeout in milliseconds\"}"
#define BH_MCP_TARGET_PROPERTY "\"targetId\":{\"type\":\"string\",\"pattern\":\"^[A-Za-z0-9._][A-Za-z0-9._-]{0,63}$\",\"description\":\"Target id like vs_12345; refresh targets and DTE first\"}"
#define BH_MCP_HINT_PROPERTY "\"hint\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1024,\"description\":\"Solution hint used to select Visual Studio, eg bh_agent_debugger\"}"

    inline const std::vector<tool_definition>& tools()
    {
        static const std::vector<tool_definition> list = {
            { "bh_agent_debugger_doctor", "Run bh_agent_debugger doctor health check (service, CLI and extension versions)",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_TIMEOUT_PROPERTY "},\"additionalProperties\":false}",
              true, forwarding::all, build_doctor },
            { "bh_agent_debugger_targets", "List debugger targets (Visual Studio instances running the extension)",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_TIMEOUT_PROPERTY "},\"additionalProperties\":false}",
              true, forwarding::all, build_targets },
            { "bh_agent_debugger_dte", "List DTE instances (Visual Studio automation objects and their solutions)",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_TIMEOUT_PROPERTY "},\"additionalProperties\":false}",
              true, forwarding::all, build_dte },
            { "bh_agent_debugger_modules", "List native modules; one-line table, max 1000",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_TARGET_PROPERTY "," BH_MCP_TIMEOUT_PROPERTY "},\"required\":[\"targetId\"],\"additionalProperties\":false}",
              true, forwarding::all, build_modules },
            { "bh_agent_debugger_profile",
              "Capture VS CPU Usage into a diagsession; start/stop are asynchronous. Poll status until ready/failed. Analysis is in VS, not a text export.",
              "{\"type\":\"object\",\"properties\":{"
              "\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"stop\",\"status\",\"report\",\"open\"]},"
              "\"processId\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295,\"description\":\"Debuggee PID; required only for start, never the devenv PID\"},"
              "\"seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":60,\"description\":\"Capture duration; start only, default 10\"},"
              "\"targetId\":{\"type\":\"string\",\"pattern\":\"^vs_[0-9]+$\",\"description\":\"vs_<pid>; required only for open. Refresh targets/DTE and select an idle VS instance.\"},"
              "\"output\":{\"type\":\"string\",\"minLength\":3,\"maxLength\":200,\"description\":\"Start only: absolute local directory (unique capture_N.diagsession) or explicit .diagsession file; never overwrites; caller-owned\"},"
              "\"timeoutMs\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120000,\"description\":\"Timeout in milliseconds; default 30000\"}"
              "},\"required\":[\"action\"],\"additionalProperties\":false}",
              false, forwarding::pipe_only, build_profile },
            { "bh_agent_debugger_profile_analyze",
              "Offline CPU analysis of an ETL/diagsession into bounded JSON using TraceEvent. No capture, elevation, broker, or symbol downloads.",
              "{\"type\":\"object\",\"properties\":{"
              "\"input\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Absolute path to saved local ETL or single-ETL diagsession\"},"
              "\"processId\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2147483647,\"description\":\"Exact recorded PID; ambiguous reused PIDs are rejected\"},"
              "\"top\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":50,\"description\":\"Rows per section; default 20\"},"
              "\"symbols\":{\"type\":\"string\",\"minLength\":1,\"description\":\"One local PDB directory; no symbol servers\"},"
              "\"function\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1024,\"description\":\"Exact containing-function name or trace-local id for inline-aware self source-line drill-down\"},"
              "\"startMs\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000000000,\"description\":\"Inclusive trace-relative time in milliseconds\"},"
              "\"endMs\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000000000,\"description\":\"Exclusive trace-relative time; must exceed startMs (default 0)\"},"
              "\"threadId\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2147483647,\"description\":\"Recorded native thread ID, not list index\"},"
              "\"subtree\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1024,\"description\":\"Exact physical root function name or frame id; keeps samples containing it; unresolved nonmatches are reported as indeterminate\"},"
              "\"sections\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"functionsBySelf\",\"functionsByInclusive\",\"callTree\",\"hotPaths\"]},\"minItems\":1,\"maxItems\":4,\"uniqueItems\":true,\"description\":\"Analysis sections to include (default all); omitted sections are null and listed in sectionsOmitted\"}"
              "},\"required\":[\"input\",\"processId\"],\"additionalProperties\":false}",
              true, forwarding::none, build_profile_analyze },
            { "bh_agent_debugger_open_dump", "Open a native x86/x64 Windows dump in an explicit VS target; refuses active sessions",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_TARGET_PROPERTY ","
              "\"path\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Absolute local path to a .dmp file (read-only)\"},"
              "\"timeoutMs\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000,\"description\":\"Timeout in milliseconds; default 30000\"}"
              "},\"required\":[\"targetId\",\"path\"],\"additionalProperties\":false}",
              false, forwarding::all, build_open_dump },
            { "bh_agent_debugger_start", "Start debugging by solution hint, optionally overriding startup-project arguments",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_HINT_PROPERTY ","
              "\"arguments\":{\"type\":\"string\",\"maxLength\":30000,\"description\":\"Raw debugger command-line arguments\"},"
              BH_MCP_TIMEOUT_PROPERTY "},\"required\":[\"hint\"],\"additionalProperties\":false}",
              false, forwarding::all, build_start },
            { "bh_agent_debugger_attach", "Attach Visual Studio debugging to a process by PID",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_HINT_PROPERTY ","
              "\"processId\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295,\"description\":\"Target process ID\"},"
              BH_MCP_TIMEOUT_PROPERTY "},\"required\":[\"hint\",\"processId\"],\"additionalProperties\":false}",
              false, forwarding::all, build_attach },
            { "bh_agent_debugger_stop", "Stop debugging by solution hint",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_HINT_PROPERTY "," BH_MCP_TIMEOUT_PROPERTY "},\"required\":[\"hint\"],\"additionalProperties\":false}",
              false, forwarding::all, build_stop },
            { "bh_agent_debugger_chain", "Run start-debugging-chain with solution hints",
              "{\"type\":\"object\",\"properties\":{"
              "\"hints\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1024},\"minItems\":1,\"maxItems\":16,\"description\":\"Ordered solution hints\"},"
              "\"timeoutMs\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000,\"description\":\"Timeout in milliseconds per hint; default 30000\"}"
              "},\"required\":[\"hints\"],\"additionalProperties\":false}",
              false, forwarding::all, build_chain },
            { "bh_agent_debugger_send", "Send target command (status/break/bp/trap/modules or open dump with a quoted absolute path)",
              "{\"type\":\"object\",\"properties\":{" BH_MCP_TARGET_PROPERTY ","
              "\"command\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":8192,\"description\":\"Raw command text, eg status, modules, list bp, remove bp <index>, local variables [count], trap add symbol <symbol>\"},"
              BH_MCP_TIMEOUT_PROPERTY "},\"required\":[\"targetId\",\"command\"],\"additionalProperties\":false}",
              false, forwarding::all, build_send },
        };
        return list;
    }

#undef BH_MCP_TIMEOUT_PROPERTY
#undef BH_MCP_TARGET_PROPERTY
#undef BH_MCP_HINT_PROPERTY

    inline const tool_definition* find_tool(const std::string& name)
    {
        for (const tool_definition& tool : tools())
            if (name == tool.name) return &tool;
        return nullptr;
    }

    inline std::string tools_list_result()
    {
        std::string result = "{\"tools\":[";
        bool first = true;
        for (const tool_definition& tool : tools())
        {
            if (!first) result += ",";
            first = false;
            result += "{\"name\":" + quote(tool.name) + ",\"description\":" + quote(tool.description) +
                ",\"inputSchema\":" + tool.input_schema;
            // Tools that act on Visual Studio or a debuggee keep MCP's conservative default hints.
            if (tool.read_only) result += ",\"annotations\":{\"readOnlyHint\":true}";
            result += "}";
        }
        return result + "]}";
    }

    // ---- Child CLI process ----------------------------------------------------------------

    struct run_result
    {
        bool started = false;
        bool timed_out = false;
        bool output_complete = true;
        bool truncated = false;
        DWORD exit_code = 1;
        DWORD launch_error = 0;
        std::string out;
        std::string err;
    };

    struct pipe_reader
    {
        HANDLE pipe = nullptr;
        std::string data;
        bool truncated = false;
        std::atomic<bool> stop{ false };
    };

    inline DWORD WINAPI read_pipe_thread(LPVOID parameter)
    {
        pipe_reader* reader = static_cast<pipe_reader*>(parameter);
        char buffer[8192];
        DWORD got = 0;
        while (!reader->stop.load() && ReadFile(reader->pipe, buffer, sizeof(buffer), &got, nullptr))
        {
            const size_t room = max_capture_bytes - reader->data.size();
            if (got > room)
            {
                reader->data.append(buffer, room);
                reader->truncated = true; // keep draining so the child never blocks on a full pipe
            }
            else
            {
                reader->data.append(buffer, got);
            }
        }
        return 0;
    }

    inline void close_handle(HANDLE& handle)
    {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = nullptr;
    }

    inline bool make_pipe(HANDLE& read_end, HANDLE& write_end)
    {
        SECURITY_ATTRIBUTES security = {};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        if (!CreatePipe(&read_end, &write_end, &security, 0)) return false;
        if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0))
        {
            const DWORD error = GetLastError();
            close_handle(read_end);
            close_handle(write_end);
            SetLastError(error);
            return false;
        }
        return true;
    }

    // Runs the CLI with stdin on NUL and captured stdout/stderr. The child inherits only those three
    // handles, never this server's protocol pipes. No job object: a broker the CLI starts on demand
    // must outlive the call, so a timed-out call stops only the CLI process itself.
    inline run_result run_cli(const std::wstring& exe, const std::vector<std::wstring>& args, DWORD wait_ms)
    {
        run_result result;
        std::wstring command = quote_argument(exe);
        for (const std::wstring& arg : args) command += L" " + quote_argument(arg);
        if (command.size() >= 32767)
        {
            result.launch_error = ERROR_FILENAME_EXCED_RANGE;
            return result;
        }

        HANDLE null_input = nullptr, out_read = nullptr, out_write = nullptr, err_read = nullptr, err_write = nullptr;
        SECURITY_ATTRIBUTES security = {};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (null_input == INVALID_HANDLE_VALUE)
        {
            result.launch_error = GetLastError();
            return result;
        }
        if (!make_pipe(out_read, out_write) || !make_pipe(err_read, err_write))
        {
            result.launch_error = GetLastError();
            close_handle(null_input); close_handle(out_read); close_handle(out_write); close_handle(err_read); close_handle(err_write);
            return result;
        }

        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<unsigned char> attribute_storage(attribute_size ? attribute_size : 1);
        LPPROC_THREAD_ATTRIBUTE_LIST attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        HANDLE inherited[3] = { null_input, out_write, err_write };
        const bool attributes_initialized = attribute_size && InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size);
        const bool attributes_ready = attributes_initialized &&
            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
        PROCESS_INFORMATION child = {};
        BOOL created = FALSE;
        DWORD create_error = ERROR_INVALID_PARAMETER;
        if (attributes_ready)
        {
            STARTUPINFOEXW startup = {};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = null_input;
            startup.StartupInfo.hStdOutput = out_write;
            startup.StartupInfo.hStdError = err_write;
            startup.lpAttributeList = attributes;
            created = CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &child);
            create_error = created ? ERROR_SUCCESS : GetLastError();
        }
        else
        {
            create_error = GetLastError();
        }
        if (attributes_initialized) DeleteProcThreadAttributeList(attributes);
        close_handle(null_input);
        close_handle(out_write);
        close_handle(err_write);
        if (!created)
        {
            result.launch_error = create_error ? create_error : ERROR_INVALID_PARAMETER;
            close_handle(out_read);
            close_handle(err_read);
            return result;
        }
        CloseHandle(child.hThread);
        result.started = true;

        std::unique_ptr<pipe_reader> readers[2] = { std::unique_ptr<pipe_reader>(new pipe_reader()), std::unique_ptr<pipe_reader>(new pipe_reader()) };
        readers[0]->pipe = out_read;
        readers[1]->pipe = err_read;
        HANDLE threads[2] = {
            CreateThread(nullptr, 0, read_pipe_thread, readers[0].get(), 0, nullptr),
            CreateThread(nullptr, 0, read_pipe_thread, readers[1].get(), 0, nullptr) };
        if (!threads[0] || !threads[1])
        {
            // Without a reader the child could block on a full pipe; stop it rather than wait.
            result.launch_error = GetLastError();
            wait_ms = 0;
        }

        if (WaitForSingleObject(child.hProcess, wait_ms) == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(child.hProcess, &result.exit_code);
        }
        else
        {
            result.timed_out = !result.launch_error;
            TerminateProcess(child.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(child.hProcess, 5000);
            result.exit_code = ERROR_TIMEOUT;
        }
        CloseHandle(child.hProcess);

        // Output ends with the CLI. If a process it started still held a pipe, stop reading after a
        // short grace period instead of hanging the server.
        for (int i = 0; i < 2; ++i)
        {
            if (!threads[i]) continue;
            if (WaitForSingleObject(threads[i], 2000) != WAIT_OBJECT_0)
            {
                readers[i]->stop.store(true);
                for (int attempt = 0; attempt < 10 && WaitForSingleObject(threads[i], 0) != WAIT_OBJECT_0; ++attempt)
                {
                    CancelSynchronousIo(threads[i]);
                    WaitForSingleObject(threads[i], 200);
                }
            }
            if (WaitForSingleObject(threads[i], 0) != WAIT_OBJECT_0)
            {
                // Still blocked: leave the reader and its pipe to the thread rather than free them.
                result.output_complete = false;
                (void)readers[i].release();
                CloseHandle(threads[i]);
                continue;
            }
            CloseHandle(threads[i]);
            close_handle(readers[i]->pipe);
        }
        for (int i = 0; i < 2; ++i)
        {
            if (readers[i] && !threads[i]) close_handle(readers[i]->pipe);
        }
        if (readers[0]) { result.out = readers[0]->data; result.truncated = readers[0]->truncated; }
        if (readers[1]) { result.err = readers[1]->data; result.truncated = result.truncated || readers[1]->truncated; }
        return result;
    }

    // Text result in the shape the OpenCode wrapper returns: the response's result.text when the
    // CLI printed one JSON response carrying it, otherwise the raw output. A response with
    // "ok": false is a tool error carrying error.message, or the raw output when it has none.
    inline std::string tool_result(const run_result& run, DWORD wait_ms)
    {
        std::string body = trim(run.out);
        if (body.empty()) body = trim(run.err);
        if (body.empty()) body = "(no output)";
        body = sanitize_utf8(body);
        if (run.truncated) body += "\n(output truncated at 4 MiB)";
        if (!run.output_complete) body += "\n(output incomplete: a process started by the CLI kept its output pipe open)";

        bool is_error = true;
        std::string text;
        if (!run.started)
        {
            text = "bh_agent_debugger_cli could not start (win32 error " + std::to_string(run.launch_error) + ")";
        }
        else if (run.launch_error)
        {
            text = "bh_agent_debugger_cli output could not be read (win32 error " + std::to_string(run.launch_error) + "); the call was stopped";
        }
        else if (run.timed_out)
        {
            text = "bh_agent_debugger_cli did not finish within " + std::to_string(wait_ms) + " ms and was stopped: " + body;
        }
        else if (run.exit_code != 0)
        {
            text = "bh_agent_debugger_cli failed (code=" + std::to_string(run.exit_code) + "): " + body;
        }
        else
        {
            is_error = false;
            text = body;
            json_value parsed;
            if (json_parser(body).parse(parsed))
            {
                const json_value* ok = parsed.find("ok");
                if (ok && ok->type == json_value::kind::boolean && !ok->flag)
                {
                    is_error = true;
                    const json_value* error_value = parsed.find("error");
                    const json_value* message = error_value ? error_value->find("message") : nullptr;
                    if (message && message->type == json_value::kind::string && !message->text.empty())
                        text = message->text;
                }
                else
                {
                    const json_value* result = parsed.find("result");
                    const json_value* result_text = result ? result->find("text") : nullptr;
                    if (result_text && result_text->type == json_value::kind::string && !result_text->text.empty())
                        text = result_text->text;
                }
            }
        }
        return std::string("{\"content\":[{\"type\":\"text\",\"text\":") + quote(text) + "}],\"isError\":" +
            (is_error ? "true" : "false") + "}";
    }

    inline std::string tool_error_result(const std::string& text)
    {
        return "{\"content\":[{\"type\":\"text\",\"text\":" + quote(text) + "}],\"isError\":true}";
    }

    // ---- JSON-RPC over stdio --------------------------------------------------------------

    class line_reader
    {
    public:
        explicit line_reader(HANDLE input) : input_(input) {}

        // False at end of input. A line longer than max_message_bytes is dropped and reported
        // through oversized so the caller can answer it with a parse error.
        bool next(std::string& line, bool& oversized)
        {
            line.clear();
            oversized = false;
            bool any = false;
            for (;;)
            {
                if (pos_ == size_)
                {
                    DWORD got = 0;
                    if (eof_ || !ReadFile(input_, chunk_, sizeof(chunk_), &got, nullptr) || got == 0)
                    {
                        eof_ = true;
                        return any;
                    }
                    pos_ = 0;
                    size_ = got;
                }
                any = true;
                const char* start = chunk_ + pos_;
                const void* newline = std::memchr(start, '\n', size_ - pos_);
                const DWORD take = newline ? static_cast<DWORD>(static_cast<const char*>(newline) - start) : size_ - pos_;
                if (!oversized)
                {
                    if (line.size() + take > max_message_bytes)
                    {
                        oversized = true;
                        line.clear();
                    }
                    else
                    {
                        line.append(start, take);
                    }
                }
                pos_ += take;
                if (newline)
                {
                    ++pos_;
                    return true;
                }
            }
        }

    private:
        HANDLE input_;
        char chunk_[65536];
        DWORD size_ = 0;
        DWORD pos_ = 0;
        bool eof_ = false;
    };

    struct server_state
    {
        HANDLE output = nullptr;
        std::wstring exe;
        std::wstring pipe;
        bool no_autostart = false;
        bool initialized = false;
        bool output_failed = false;
    };

    inline void send_message(server_state& state, const std::string& message)
    {
        if (state.output_failed) return;
        const std::string framed = message + "\n";
        size_t written = 0;
        while (written < framed.size())
        {
            const size_t remaining = framed.size() - written;
            const DWORD request = remaining > (1u << 20) ? static_cast<DWORD>(1u << 20) : static_cast<DWORD>(remaining);
            DWORD chunk = 0;
            if (!WriteFile(state.output, framed.data() + written, request, &chunk, nullptr) || chunk == 0)
            {
                state.output_failed = true;
                return;
            }
            written += chunk;
        }
    }

    inline std::string id_text(const json_value& id)
    {
        return id.type == json_value::kind::string ? quote(id.text) : id.text;
    }

    inline void send_result(server_state& state, const std::string& id, const std::string& result)
    {
        send_message(state, "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
    }

    inline void send_error(server_state& state, const std::string& id, int code, const std::string& message)
    {
        send_message(state, "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) +
            ",\"message\":" + quote(message) + "}}");
    }

    inline std::string initialize_result(const std::string& requested_version)
    {
        // Echo a supported requested revision; otherwise offer the newest one this server implements.
        static const char* const supported[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };
        std::string version = supported[0];
        for (const char* candidate : supported)
            if (requested_version == candidate) version = candidate;
        return "{\"protocolVersion\":" + quote(version) +
            ",\"capabilities\":{\"tools\":{\"listChanged\":false}}"
            ",\"serverInfo\":{\"name\":\"bh_agent_debugger\",\"version\":" + quote(bh_agent_version::version) + "}"
            ",\"instructions\":" + quote(
                "Drives Visual Studio debugging through BH Agent Debugger. These tools act on live processes as "
                "the signed-in user: starting, attaching, breakpoints and expression evaluation can run code. "
                "Refresh bh_agent_debugger_targets and bh_agent_debugger_dte before the first "
                "bh_agent_debugger_send, and use target ids exactly as listed.") + "}";
    }

    inline void call_tool(server_state& state, const std::string& id, const json_value& params)
    {
        static const json_value empty_object = [] { json_value value; value.type = json_value::kind::object; return value; }();
        const json_value* name = params.find("name");
        if (!name || name->type != json_value::kind::string)
        {
            send_error(state, id, -32602, "Invalid params: name must be a string");
            return;
        }
        const json_value* arguments = params.find("arguments");
        if (arguments && arguments->type != json_value::kind::object)
        {
            send_error(state, id, -32602, "Invalid params: arguments must be an object");
            return;
        }
        const tool_definition* tool = find_tool(name->text);
        if (!tool)
        {
            send_error(state, id, -32602, "Unknown tool: " + name->text);
            return;
        }
        tool_call call;
        std::string problem;
        if (!tool->build(arguments ? *arguments : empty_object, call, problem))
        {
            bh_log::write("cli.mcp", std::string("tools/call rejected name=") + tool->name);
            send_result(state, id, tool_error_result("invalid arguments: " + problem));
            return;
        }
        if (tool->forward != forwarding::none && !state.pipe.empty())
        {
            call.args.push_back(L"--pipe");
            call.args.push_back(state.pipe);
        }
        if (tool->forward == forwarding::all && state.no_autostart) call.args.push_back(L"--no-auto-start-service");
        const run_result run = run_cli(state.exe, call.args, call.wait_ms);
        bh_log::write("cli.mcp", std::string("tools/call name=") + tool->name + " started=" + (run.started ? "1" : "0") +
            " exit=" + std::to_string(run.exit_code) + " timedOut=" + (run.timed_out ? "1" : "0"));
        send_result(state, id, tool_result(run, call.wait_ms));
    }

    inline void handle_line(server_state& state, const std::string& line)
    {
        json_value message;
        if (!json_parser(line).parse(message))
        {
            send_error(state, "null", -32700, "Parse error");
            return;
        }
        if (message.type == json_value::kind::array)
        {
            send_error(state, "null", -32600, "Invalid Request: JSON-RPC batches are not supported");
            return;
        }
        if (message.type != json_value::kind::object)
        {
            send_error(state, "null", -32600, "Invalid Request");
            return;
        }
        const json_value* id = message.find("id");
        const json_value* method = message.find("method");
        const json_value* version = message.find("jsonrpc");
        const bool id_valid = id && (id->type == json_value::kind::string || id->type == json_value::kind::number);
        const std::string reply_id = id_valid ? id_text(*id) : std::string("null");
        if (!method && (message.find("result") || message.find("error")))
            return; // a response to a server request; this server sends none
        if (!version || version->type != json_value::kind::string || version->text != "2.0" ||
            !method || method->type != json_value::kind::string)
        {
            if (id) send_error(state, reply_id, -32600, "Invalid Request: expected jsonrpc \"2.0\" and a method name");
            return;
        }
        if (!id) return; // notifications (initialized, cancelled, ...) need no reply
        if (!id_valid)
        {
            send_error(state, "null", -32600, "Invalid Request: id must be a string or a number");
            return;
        }
        const json_value* params = message.find("params");
        if (params && params->type != json_value::kind::object)
        {
            send_error(state, reply_id, -32602, "Invalid params: params must be an object");
            return;
        }
        static const json_value empty_object = [] { json_value value; value.type = json_value::kind::object; return value; }();
        const json_value& request = params ? *params : empty_object;
        const std::string& name = method->text;
        if (name == "ping")
        {
            send_result(state, reply_id, "{}");
            return;
        }
        if (name == "initialize")
        {
            const json_value* requested = request.find("protocolVersion");
            if (!requested || requested->type != json_value::kind::string)
            {
                send_error(state, reply_id, -32602, "Invalid params: protocolVersion must be a string");
                return;
            }
            if (state.initialized)
            {
                send_error(state, reply_id, -32600, "Invalid Request: already initialized");
                return;
            }
            state.initialized = true;
            send_result(state, reply_id, initialize_result(requested->text));
            return;
        }
        if (name != "tools/list" && name != "tools/call")
        {
            send_error(state, reply_id, -32601, "Method not found: " + name);
            return;
        }
        if (!state.initialized)
        {
            send_error(state, reply_id, -32002, "Server not initialized: send initialize first");
            return;
        }
        if (name == "tools/list")
        {
            send_result(state, reply_id, tools_list_result());
            return;
        }
        call_tool(state, reply_id, request);
    }

    inline int run(int argc, char* argv[])
    {
        server_state state;
        for (int i = 2; i < argc; ++i)
        {
            const std::string option = argv[i];
            if (option == "--no-auto-start-service" && !state.no_autostart)
            {
                state.no_autostart = true;
                continue;
            }
            if (option == "--pipe" && state.pipe.empty() && i + 1 < argc)
            {
                const std::string value = argv[++i];
                bool valid = !value.empty() && value.size() <= 200 && value[0] != '-';
                for (char ch : value)
                    valid = valid && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-');
                if (valid)
                {
                    state.pipe.assign(value.begin(), value.end());
                    continue;
                }
            }
            std::fprintf(stderr, "invalid mcp option: %s\nUsage: bh_agent_debugger_cli mcp [--pipe <name>] [--no-auto-start-service]\n", option.c_str());
            return 1;
        }

        wchar_t module[32768] = {};
        const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
        if (!length || length >= 32768)
        {
            std::fprintf(stderr, "mcp: CLI path unavailable (win32 error %lu)\n", GetLastError());
            return 1;
        }
        state.exe.assign(module, length);
        state.output = GetStdHandle(STD_OUTPUT_HANDLE);
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (!state.output || state.output == INVALID_HANDLE_VALUE || !input || input == INVALID_HANDLE_VALUE)
        {
            std::fprintf(stderr, "mcp: stdin and stdout must be available\n");
            return 1;
        }
        bh_log::write("cli.mcp", "server start");

        std::unique_ptr<line_reader> reader(new line_reader(input));
        std::string line;
        bool oversized = false;
        bool first_line = true;
        while (!state.output_failed && reader->next(line, oversized))
        {
            // Windows UTF-8 writers (for example .NET with code page 65001) may start the stream with a BOM.
            if (first_line && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            first_line = false;
            if (oversized)
            {
                send_error(state, "null", -32700, "Parse error: message exceeds 4 MiB");
                continue;
            }
            const std::string message = trim(line);
            if (!message.empty()) handle_line(state, message);
        }
        bh_log::write("cli.mcp", state.output_failed ? "server stop: output closed" : "server stop: input closed");
        return 0;
    }
}
