#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bh_agent_command_chain
{
inline std::string trim_ascii(const std::string& text)
{
    std::size_t start = 0;
    while (start < text.size() &&
           (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
    {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }

    return text.substr(start, end - start);
}

inline void split(const char* message, std::vector<std::string>& commands)
{
    commands.clear();
    if (!message)
    {
        return;
    }

    const std::string input(message);
    std::string current;
    bool quoted = false;
    std::size_t consecutive_backslashes = 0;
    std::size_t index = 0;
    while (index < input.size())
    {
        const bool is_semicolon = input[index] == ';';
        const bool is_double_amp =
            input[index] == '&' && index + 1 < input.size() && input[index + 1] == '&';
        if (!quoted && (is_semicolon || is_double_amp))
        {
            const std::string trimmed = trim_ascii(current);
            if (!trimmed.empty())
            {
                commands.push_back(trimmed);
            }
            current.clear();
            consecutive_backslashes = 0;
            index += is_double_amp ? 2 : 1;
            continue;
        }

        const char value = input[index];
        if (value == '"' && (consecutive_backslashes % 2U) == 0)
        {
            quoted = !quoted;
        }
        current.push_back(value);
        consecutive_backslashes = value == '\\' ? consecutive_backslashes + 1U : 0U;
        ++index;
    }

    const std::string trailing = trim_ascii(current);
    if (!trailing.empty())
    {
        commands.push_back(trailing);
    }
}
} // namespace bh_agent_command_chain
