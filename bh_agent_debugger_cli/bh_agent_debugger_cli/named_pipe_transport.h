#pragma once

#include <string>

namespace bh_agent_transport
{
    bool send_request(
        const std::wstring& pipe_name,
        const std::string& request,
        std::string& response,
        unsigned int connect_timeout_ms);
}
