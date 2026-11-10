#pragma once

#include <cstddef>
#include <string>

namespace bh_startup_arguments
{
constexpr std::size_t max_argument_bytes = 16U * 1024U;
static const char command_prefix[] = "__bh_transient_start_hex ";

inline bool is_valid_utf8(const std::string& input)
{
    std::size_t i = 0;
    while (i < input.size())
    {
        const unsigned char first = static_cast<unsigned char>(input[i]);
        if (first <= 0x7f)
        {
            if (first == 0) return false;
            ++i;
            continue;
        }

        std::size_t length = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xbf;
        if (first >= 0xc2 && first <= 0xdf)
        {
            length = 2;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            length = 3;
            if (first == 0xe0) second_min = 0xa0;
            if (first == 0xed) second_max = 0x9f;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            length = 4;
            if (first == 0xf0) second_min = 0x90;
            if (first == 0xf4) second_max = 0x8f;
        }
        else
        {
            return false;
        }

        if (input.size() - i < length) return false;
        const unsigned char second = static_cast<unsigned char>(input[i + 1]);
        if (second < second_min || second > second_max) return false;
        for (std::size_t continuation = 2; continuation < length; ++continuation)
        {
            const unsigned char value = static_cast<unsigned char>(input[i + continuation]);
            if (value < 0x80 || value > 0xbf) return false;
        }
        i += length;
    }
    return true;
}

inline bool try_encode(const std::string& arguments, std::string& encoded)
{
    encoded.clear();
    if (arguments.size() > max_argument_bytes || !is_valid_utf8(arguments)) return false;
    if (arguments.empty())
    {
        encoded = "-";
        return true;
    }

    static const char digits[] = "0123456789abcdef";
    encoded.reserve(arguments.size() * 2U);
    for (unsigned char value : arguments)
    {
        encoded.push_back(digits[value >> 4]);
        encoded.push_back(digits[value & 0x0f]);
    }
    return true;
}

inline bool try_decode(const std::string& encoded, std::string& arguments)
{
    arguments.clear();
    if (encoded == "-") return true;
    if (encoded.empty() || encoded.size() > max_argument_bytes * 2U || (encoded.size() % 2U) != 0) return false;

    auto nibble = [](char value) -> int
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };

    arguments.reserve(encoded.size() / 2U);
    for (std::size_t i = 0; i < encoded.size(); i += 2U)
    {
        const int high = nibble(encoded[i]);
        const int low = nibble(encoded[i + 1]);
        if (high < 0 || low < 0)
        {
            arguments.clear();
            return false;
        }
        arguments.push_back(static_cast<char>((high << 4) | low));
    }

    if (!is_valid_utf8(arguments))
    {
        arguments.clear();
        return false;
    }
    return true;
}
} // namespace bh_startup_arguments
