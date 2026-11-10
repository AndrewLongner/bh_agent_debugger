#pragma once

#include <string.h>

// Shared grammar for "eval [--side-effects] <expression>". The package parser and the
// read-only dump gate both use it so they cannot disagree about which form was requested.
// --side-effects is recognized only as the first token after "eval".
namespace bh_eval_grammar
{
    enum class parse_result
    {
        not_eval,
        usage,
        ok
    };

    inline bool is_blank(char ch)
    {
        return ch == ' ' || ch == '\t';
    }

    inline parse_result parse(const char* message, bool& allow_side_effects, const char*& expression)
    {
        allow_side_effects = false;
        expression = nullptr;
        if (!message)
        {
            return parse_result::not_eval;
        }

        if (_stricmp(message, "eval") == 0)
        {
            return parse_result::usage;
        }

        if (_strnicmp(message, "eval ", 5) != 0)
        {
            return parse_result::not_eval;
        }

        const char* cursor = message + 5;
        while (is_blank(*cursor))
        {
            ++cursor;
        }

        static const char option[] = "--side-effects";
        const size_t option_length = sizeof(option) - 1;
        if (_strnicmp(cursor, option, option_length) == 0 &&
            (cursor[option_length] == 0 || is_blank(cursor[option_length])))
        {
            allow_side_effects = true;
            cursor += option_length;
            while (is_blank(*cursor))
            {
                ++cursor;
            }
        }

        if (*cursor == 0)
        {
            return parse_result::usage;
        }

        expression = cursor;
        return parse_result::ok;
    }
}
