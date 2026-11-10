#pragma once

// Named-pipe hardening shared by the broker service, the Visual Studio package and
// every client (CLI, broker-to-package, package-to-broker).
//
// Server side: each pipe is owned by, and only accessible to, the user account that
// created it (protected DACL, so no Everyone/Administrators/SYSTEM entries), and
// network clients are rejected with PIPE_REJECT_REMOTE_CLIENTS.
// Client side: before sending a request, the client checks that the pipe it opened is
// owned by its own user account, so another account cannot pre-create a bh_agent pipe
// name and capture requests. Both sides fail closed.

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace bh_agent_pipe_security
{
    // Process token user SID (binary). Empty on failure.
    inline std::vector<BYTE> current_user_sid()
    {
        std::vector<BYTE> sid;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            return sid;
        }

        DWORD size = 0;
        (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        if (size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
        {
            PSID user = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
            if (IsValidSid(user))
            {
                const DWORD length = GetLengthSid(user);
                sid.resize(length);
                if (!CopySid(length, sid.data(), user))
                {
                    sid.clear();
                }
            }
        }

        CloseHandle(token);
        return sid;
    }

    // Security attributes for CreateNamedPipeW: owner and sole grantee is the current user.
    // get() returns nullptr when the descriptor could not be built; callers must then
    // refuse to create the pipe rather than fall back to default security.
    class server_attributes
    {
    public:
        server_attributes()
        {
            std::vector<BYTE> sid = current_user_sid();
            LPWSTR sid_text = nullptr;
            if (sid.empty() || !ConvertSidToStringSidW(reinterpret_cast<PSID>(sid.data()), &sid_text))
            {
                return;
            }

            const std::wstring sddl = std::wstring(L"O:") + sid_text + L"D:P(A;;GA;;;" + sid_text + L")";
            LocalFree(sid_text);
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_descriptor, nullptr))
            {
                m_descriptor = nullptr;
                return;
            }

            m_attributes.nLength = sizeof(m_attributes);
            m_attributes.lpSecurityDescriptor = m_descriptor;
            m_attributes.bInheritHandle = FALSE;
        }

        ~server_attributes()
        {
            if (m_descriptor)
            {
                LocalFree(m_descriptor);
            }
        }

        server_attributes(const server_attributes&) = delete;
        server_attributes& operator=(const server_attributes&) = delete;

        SECURITY_ATTRIBUTES* get()
        {
            return m_descriptor ? &m_attributes : nullptr;
        }

    private:
        PSECURITY_DESCRIPTOR m_descriptor = nullptr;
        SECURITY_ATTRIBUTES m_attributes = {};
    };

    // Pipe-mode flags every bh_agent pipe server adds.
    constexpr DWORD server_pipe_mode_flags = PIPE_REJECT_REMOTE_CLIENTS;

    // True when the pipe behind a connected client handle is owned by the current user.
    // The handle needs READ_CONTROL, which GENERIC_READ includes.
    inline bool server_owned_by_current_user(HANDLE pipe)
    {
        if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const std::vector<BYTE> user = current_user_sid();
        if (user.empty())
        {
            return false;
        }

        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const DWORD status = GetSecurityInfo(
            pipe,
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        if (status != ERROR_SUCCESS)
        {
            return false;
        }

        const bool same = owner != nullptr && IsValidSid(owner) &&
            EqualSid(owner, const_cast<BYTE*>(user.data())) != FALSE;
        LocalFree(descriptor);
        return same;
    }
}
