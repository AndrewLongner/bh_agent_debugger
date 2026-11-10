#pragma once

#include <chrono>
#include <cstdint>

namespace bh_agent_symbol_search
{
constexpr std::uint64_t default_time_limit_ms = 5000;
constexpr std::uint64_t default_symbol_limit = 250000;
constexpr std::uint64_t default_module_limit = 64;

enum class limit
{
    none,
    elapsed_time,
    symbol_work,
    modules
};

class budget
{
public:
    explicit budget(
        std::uint64_t time_limit_ms = default_time_limit_ms,
        std::uint64_t symbol_limit = default_symbol_limit,
        std::uint64_t module_limit = default_module_limit)
        : m_deadline(clock::now() + std::chrono::milliseconds(time_limit_ms)),
          m_symbol_limit(symbol_limit),
          m_module_limit(module_limit)
    {
    }

    bool try_begin_module()
    {
        if (check_time())
        {
            return false;
        }
        if (m_modules_examined >= m_module_limit)
        {
            m_limit = limit::modules;
            return false;
        }
        ++m_modules_examined;
        return true;
    }

    bool try_examine_symbol()
    {
        if (check_time())
        {
            return false;
        }
        if (m_symbols_examined >= m_symbol_limit)
        {
            m_limit = limit::symbol_work;
            return false;
        }
        ++m_symbols_examined;
        return true;
    }

    bool exhausted()
    {
        return check_time();
    }

    limit limit_reason()
    {
        (void)check_time();
        return m_limit;
    }

    std::uint64_t symbols_examined() const
    {
        return m_symbols_examined;
    }

    std::uint64_t modules_examined() const
    {
        return m_modules_examined;
    }

private:
    using clock = std::chrono::steady_clock;

    bool check_time()
    {
        if (m_limit != limit::none)
        {
            return true;
        }
        if (clock::now() >= m_deadline)
        {
            m_limit = limit::elapsed_time;
            return true;
        }
        return false;
    }

    clock::time_point m_deadline;
    std::uint64_t m_symbol_limit;
    std::uint64_t m_module_limit;
    std::uint64_t m_symbols_examined = 0;
    std::uint64_t m_modules_examined = 0;
    limit m_limit = limit::none;
};
} // namespace bh_agent_symbol_search
