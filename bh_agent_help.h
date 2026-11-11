#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>

namespace bh_agent_help
{
    constexpr size_t max_response_bytes = 8192;
    constexpr size_t max_chain_response_bytes = 8192;

    struct entry
    {
        const char* topics;
        const char* syntax;
        const char* detail;
    };

    inline const entry* catalog(size_t& count)
    {
        static const entry entries[] =
        {
            {"help|commands", "help [command]", "Lists all public commands or details for one command. Help is static, read-only, mode-independent, and bounded to 8192 bytes."},
            {"status", "status", "Reports Visual Studio and engine state. Available in design, run, break, and dump sessions."},
            {"modules", "modules", "Lists native modules for the selected live process or active dump process."},
            {"open dump", "open dump \"<absolute-local-path.dmp>\"", "Opens one native x86/x64 user-mode dump read-only. Requires design mode and refuses an active debug session."},
            {"poll break", "poll break [timeoutMs] [intervalMs]", "Waits for break mode. Live defaults are 15000 ms and 200 ms; interval values below 50 ms become 50 ms. In a dump session it returns the current paused status."},
            {"eval|evaluate", "eval [--side-effects] <expression>", "Evaluates an expression in the frame selected in Visual Studio during break mode. By default it uses Concord with side effects and function evaluation blocked; only the Microsoft C/C++, C#, and VB evaluators are accepted and other frames fail closed. --side-effects, accepted only as the first token, uses the Visual Studio automation evaluator, which can assign variables or run code in the debuggee; dump sessions refuse it. Expression input is limited to 1024 bytes."},
            {"locals|local variables", "locals [count] / local variables [count]", "Lists current-frame locals in break mode. Default count is 32 and maximum count is 128."},
            {"list threads|threads", "list threads", "Lists native threads for the active break session and marks the selected native TID."},
            {"select thread", "select thread <tid>", "Selects a positive decimal native TID from list threads for DTE and engine inspection."},
            {"where|call stack|symbolic stack", "where [frames]", "Canonical symbolic call stack command. Default is 16 frames and maximum is 128; requires an active break session."},
            {"read regs|registers", "read regs", "Reads registers for the selected break-session thread."},
            {"read stack", "read stack [bytes]", "Reads raw memory from the selected thread stack pointer. Default is 64 bytes live or 128 bytes in a dump; maximum returned is 256 bytes live or 4096 bytes in a dump."},
            {"stack", "stack [value]", "Context-sensitive legacy alias: live sessions treat value as raw stack bytes (like read stack); dump sessions treat value as symbolic frame count (like where). Prefer where for symbolic call stacks and read stack for bytes."},
            {"read mem|memory", "read mem <address> <bytes> [hex|ascii|u32|u64|ptr]", "Reads process or captured dump memory. Size must be positive; maximum returned is 256 bytes live or 4096 bytes in a dump. Typed sizes must align to their element width."},
            {"step|step into|step over|step out", "step into / step over / step out", "Executes the selected source-level step from break mode and waits for a stable new break."},
            {"continue", "continue", "Continues from break mode and returns once Visual Studio reports run mode, so a following break works; it is a safe no-op when already running or not in break mode."},
            {"start|start debugging", "start debugging / start", "Starts debugging through Visual Studio DTE."},
            {"break|async break", "async break / break", "Requests Break All while running; it is a safe no-op when already broken."},
            {"stop|stop debugging", "stop debugging / stop", "Stops the active Visual Studio debugging session."},
            {"set bp|breakpoint add", "set bp <file>:<line>", "Adds a Visual Studio file/line breakpoint. Line is a positive decimal number."},
            {"list bp|breakpoint list", "list bp", "Lists Visual Studio breakpoints with zero-based display indices."},
            {"remove bp|breakpoint remove", "remove bp <zero-based-index>", "Deletes the breakpoint at the zero-based index shown by list bp."},
            {"clear bp|clear all bp|breakpoint clear", "clear bp / clear all bp", "Deletes every Visual Studio breakpoint."},
            {"set target pid|target pid", "set target pid <pid>", "Selects a positive decimal live debug-process PID for module, symbol, and trap operations. Unsupported in dump sessions."},
            {"trap add|set trap", "trap add <address> / set trap <address>", "Adds a live instruction trap at a decimal or 0x-prefixed address."},
            {"trap add symbol|set trap symbol", "trap add symbol <symbol> / set trap symbol <symbol>", "Resolves a live native symbol and adds an instruction trap. Run status, modules, and set target pid first."},
            {"trap list", "trap list", "Lists live instruction traps and their IDs."},
            {"trap remove", "trap remove <id>", "Removes the positive trap ID shown by trap list."},
            {"trap clear", "trap clear", "Removes every live instruction trap."},
            {"queue continue", "queue continue", "Queues a continue action for the live engine."},
            {"queue list", "queue list", "Lists pending live-engine actions."},
            {"queue clear", "queue clear", "Deletes all pending live-engine actions."},
            {"queue run", "queue run", "Applies pending live-engine actions."},
            {"trap resume", "trap resume", "Clears the engine's recorded trap/break session without issuing Visual Studio Continue."},
            {"resolve symbol", "resolve symbol <symbol>", "Resolves a native symbol for the selected live target PID without setting a trap."},
            {"chain|chaining", "<command>; <command> / <command> && <command>", "Runs commands sequentially in one send. Execution stops on the first error, and the aggregate response is limited to 8192 bytes."}
        };
        count = sizeof(entries) / sizeof(entries[0]);
        return entries;
    }

    inline bool is_space(char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }

    inline bool begins_with_help(const char* message)
    {
        if (!message) return false;
        while (is_space(*message)) ++message;
        const char word[] = "help";
        for (size_t i = 0; i < sizeof(word) - 1; ++i)
        {
            if (!message[i] || static_cast<char>(std::tolower(static_cast<unsigned char>(message[i]))) != word[i]) return false;
        }
        return message[4] == 0 || is_space(message[4]);
    }

    inline bool normalize(const char* text, std::string& result)
    {
        result.clear();
        bool pending_space = false;
        for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text ? text : ""); *cursor; ++cursor)
        {
            if (is_space(static_cast<char>(*cursor)))
            {
                pending_space = !result.empty();
                continue;
            }
            if (*cursor < 0x20 || *cursor > 0x7e) return false;
            if (pending_space)
            {
                result.push_back(' ');
                pending_space = false;
            }
            result.push_back(static_cast<char>(std::tolower(*cursor)));
            if (result.size() > 1024) return false;
        }
        return true;
    }

    inline bool topic_matches(const char* aliases, const std::string& topic)
    {
        const char* start = aliases;
        while (start && *start)
        {
            const char* end = std::strchr(start, '|');
            const size_t length = end ? static_cast<size_t>(end - start) : std::strlen(start);
            if (topic.size() == length && topic.compare(0, length, start, length) == 0) return true;
            start = end ? end + 1 : nullptr;
        }
        return false;
    }

    inline void enforce_bound(std::string& response)
    {
        if (response.size() > max_response_bytes)
        {
            static const char suffix[] = "... [help truncated at 8192 bytes]";
            response.resize(max_response_bytes - (sizeof(suffix) - 1));
            response += suffix;
        }
    }

    inline bool try_handle(const char* message, std::string& response)
    {
        if (!begins_with_help(message)) return false;

        std::string normalized;
        if (!normalize(message, normalized))
        {
            response = "error: help command must be printable ASCII and at most 1024 bytes";
            return true;
        }

        if (normalized == "help")
        {
            response = "ok: help read-only=true mode-independent=true; use help <command>; commands:";
            size_t count = 0;
            const entry* entries = catalog(count);
            for (size_t i = 0; i < count; ++i)
            {
                response += " ";
                response += entries[i].syntax;
                response += ";";
            }
            enforce_bound(response);
            return true;
        }

        const std::string topic = normalized.substr(5);
        if (topic.empty())
        {
            response = "error: help usage: help [command]";
            return true;
        }
        if (topic.size() > 96)
        {
            response = "error: help topic too long (max 96 bytes); use help";
            return true;
        }

        size_t count = 0;
        const entry* entries = catalog(count);
        for (size_t i = 0; i < count; ++i)
        {
            if (topic_matches(entries[i].topics, topic))
            {
                response = "ok: help ";
                response += entries[i].syntax;
                response += " -- ";
                response += entries[i].detail;
                enforce_bound(response);
                return true;
            }
        }

        response = "error: help topic not found: ";
        response += topic;
        response += "; use help";
        enforce_bound(response);
        return true;
    }

    inline bool append_chain_item(std::string& combined, const std::string& item)
    {
        if (combined.size() + item.size() > max_chain_response_bytes) return false;
        combined += item;
        return true;
    }
}
