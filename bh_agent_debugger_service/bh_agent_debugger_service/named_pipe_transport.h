#pragma once

#include <string>

namespace bh_agent_transport
{
    bool run_service_loop(const std::wstring& pipe_name, const std::wstring& default_target_pipe_name);
    bool send_request(
        const std::wstring& pipe_name,
        const std::string& request,
        std::string& response,
        unsigned int connect_timeout_ms);
}
