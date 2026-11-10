#include "bh_agent_local_component.h"
#include "..\bh_agent_log.h"

#include <string>

namespace
{
const GUID bh_guid_agent_local_component = { 0x87f9e4dd, 0x3fd0, 0x4a2f, { 0x9f, 0x56, 0xec, 0xf4, 0xc9, 0xa6, 0xd8, 0x02 } };
const GUID bh_guid_agent_message_to_local = { 0x1f3d8a6e, 0x8de6, 0x4ccb, { 0xb1, 0x8c, 0x3a, 0xa4, 0x41, 0x2, 0xf5, 0x1 } };
const GUID bh_guid_agent_message_to_remote = { 0xdd0faea9, 0x3c8a, 0x44b8, { 0xa4, 0xb7, 0xcb, 0x53, 0xa9, 0xd6, 0x2b, 0x0b } };
const GUID bh_guid_agent_message_to_local_worker = { 0xa5d0071b, 0xd3b8, 0x43b5, { 0x8e, 0xd9, 0xdd, 0xb2, 0x4, 0xf6, 0xe3, 0x3 } };
const UINT32 bh_message_to_local_worker_resolve_symbol = 1;
const UINT32 bh_message_to_local_worker_resolve_address = 2;
const UINT32 bh_agent_message_from_script = 2;

bool bh_try_utf8_to_wide(const char* text, std::wstring& wide)
{
    wide.clear();
    if (!text)
    {
        return false;
    }

    int wide_len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wide_len <= 0)
    {
        return false;
    }

    if (wide_len == 1)
    {
        return true;
    }

    wide.resize((size_t)wide_len - 1);
    return MultiByteToWideChar(CP_UTF8, 0, text, -1, &wide[0], wide_len) > 0;
}

bool bh_try_wide_to_utf8(const wchar_t* text, std::string& utf8)
{
    utf8.clear();
    if (!text)
    {
        return false;
    }

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0)
    {
        return false;
    }

    if (utf8_len == 1)
    {
        return true;
    }

    utf8.resize((size_t)utf8_len - 1);
    return WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], utf8_len, nullptr, nullptr) > 0;
}

bool bh_get_active_debug_process(CComPtr<DkmProcess>& process)
{
    process.Release();

    CAutoDkmArray<DkmProcess*> processes;
    if (FAILED(DkmProcess::GetProcesses(&processes)) || processes.Length == 0)
    {
        return false;
    }

    for (UINT32 i = 0; i < processes.Length; ++i)
    {
        DkmProcess* candidate = processes.Members[i];
        if (!candidate || !candidate->Connection())
        {
            continue;
        }

        if (candidate->LivePart())
        {
            process = candidate;
            return true;
        }
    }

    for (UINT32 i = 0; i < processes.Length; ++i)
    {
        DkmProcess* candidate = processes.Members[i];
        if (!candidate || !candidate->Connection())
        {
            continue;
        }

        process = candidate;
        return true;
    }

    return false;
}

std::string bh_forward_script_message_to_remote(const char* message)
{
    if (!message || !message[0])
    {
        return "error: empty message";
    }

    CComPtr<DkmProcess> process;
    if (!bh_get_active_debug_process(process) || !process)
    {
        return "error: no active debug process";
    }

    std::wstring message_wide;
    if (!bh_try_utf8_to_wide(message, message_wide))
    {
        return "error: message conversion failed";
    }

    CComPtr<DkmVariant> message_variant;
    HRESULT hr = DkmVariant::Create(message_wide.c_str(), &message_variant);
    if (FAILED(hr) || !message_variant)
    {
        char err[128] = {};
        _snprintf_s(err, sizeof(err), _TRUNCATE, "error: DkmVariant::Create failed hr=0x%08X", (unsigned)hr);
        return err;
    }

    CComPtr<DkmCustomMessage> request;
    hr = DkmCustomMessage::Create(
        process->Connection(),
        process,
        bh_guid_agent_message_to_remote,
        bh_agent_message_from_script,
        message_variant,
        nullptr,
        &request);
    if (FAILED(hr) || !request)
    {
        char err[128] = {};
        _snprintf_s(err, sizeof(err), _TRUNCATE, "error: DkmCustomMessage::Create failed hr=0x%08X", (unsigned)hr);
        return err;
    }

    CComPtr<DkmCustomMessage> reply;
    hr = request->SendLower(&reply);
    if (FAILED(hr))
    {
        char err[128] = {};
        _snprintf_s(err, sizeof(err), _TRUNCATE, "error: SendLower failed hr=0x%08X", (unsigned)hr);
        return err;
    }

    if (!reply)
    {
        return "ok: forwarded to remote";
    }

    DkmVariant* response_variant = reply->Parameter1();
    if (!response_variant)
    {
        return "ok: forwarded to remote";
    }

    const VARIANT& value = response_variant->Value();
    if (value.vt != VT_BSTR || !value.bstrVal)
    {
        return "ok: forwarded to remote";
    }

    std::string response_text;
    if (!bh_try_wide_to_utf8(value.bstrVal, response_text))
    {
        return "error: response conversion failed";
    }

    return response_text.empty() ? "ok: forwarded to remote" : response_text;
}

}

HRESULT STDMETHODCALLTYPE bh_agent_local_component::SendHigher(
    DkmCustomMessage* pCustomMessage,
    DkmCustomMessage** ppReplyMessage)
{
    if (!pCustomMessage)
    {
        bh_agent_logf("engine.local", "SendHigher missing custom message");
        return E_POINTER;
    }

    DkmProcess* process = pCustomMessage->Process();
    if (!process)
    {
        bh_agent_logf("engine.local", "SendHigher missing process");
        return E_FAIL;
    }

    UINT32 message_code = pCustomMessage->MessageCode();
    if (message_code != bh_message_to_local_worker_resolve_symbol &&
        message_code != bh_message_to_local_worker_resolve_address)
    {
        bh_agent_logf("engine.local", "SendHigher ignored messageCode=%u", (unsigned)message_code);
        return S_OK;
    }

    bh_agent_logf("engine.local", "SendHigher worker request code=%u", (unsigned)message_code);

    CComPtr<DefaultPort::DkmWorkerProcessConnection> worker_connection;
    HRESULT hr = DefaultPort::DkmWorkerProcessConnection::GetLocalSymbolsConnection(&worker_connection);
    if (FAILED(hr) || !worker_connection)
    {
        bh_agent_logf("engine.local", "GetLocalSymbolsConnection failed hr=0x%08X", (unsigned)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    CComPtr<DkmCustomMessage> message_to_worker;
    hr = DkmCustomMessage::Create(
        process->Connection(),
        process,
        bh_guid_agent_message_to_local_worker,
        message_code,
        pCustomMessage->Parameter1(),
        pCustomMessage->Parameter2(),
        nullptr,
        worker_connection,
        &message_to_worker);
    if (FAILED(hr) || !message_to_worker)
    {
        bh_agent_logf("engine.local", "Create messageToWorker failed hr=0x%08X", (unsigned)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    CComPtr<DkmCustomMessage> reply;
    hr = message_to_worker->SendLower(&reply);
    if (FAILED(hr))
    {
        bh_agent_logf("engine.local", "messageToWorker SendLower failed hr=0x%08X", (unsigned)hr);
        return hr;
    }

    bh_agent_logf("engine.local", "SendHigher resolve symbol reply ok");

    if (ppReplyMessage)
    {
        *ppReplyMessage = reply.Detach();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_local_component::SendLower(
    DkmCustomMessage* pCustomMessage,
    DkmCustomMessage** ppReplyMessage)
{
    if (!pCustomMessage)
    {
        return E_POINTER;
    }

    if (pCustomMessage->MessageCode() != bh_agent_message_from_script)
    {
        return S_OK;
    }

    DkmProcess* process = pCustomMessage->Process();
    if (!process)
    {
        return E_FAIL;
    }

    DkmVariant* parameter = pCustomMessage->Parameter1();
    if (!parameter)
    {
        return E_INVALIDARG;
    }

    const VARIANT& value = parameter->Value();
    if (value.vt != VT_BSTR || !value.bstrVal)
    {
        return E_INVALIDARG;
    }

    std::string message;
    if (!bh_try_wide_to_utf8(value.bstrVal, message))
    {
        return E_FAIL;
    }

    std::string response;
    if (_stricmp(message.c_str(), "local ping") == 0)
    {
        response = "ok: local pong";
    }
    else
    {
        response = bh_forward_script_message_to_remote(message.c_str());
        if (response.empty())
        {
            response = "ok: forwarded to remote";
        }
    }

    bh_agent_logf("engine.local", "custom message from package: '%s' -> '%s'", message.c_str(), response.c_str());

    if (!ppReplyMessage)
    {
        return S_OK;
    }

    std::wstring response_wide;
    if (!bh_try_utf8_to_wide(response.c_str(), response_wide))
    {
        return E_FAIL;
    }

    CComPtr<DkmVariant> response_variant;
    HRESULT hr = DkmVariant::Create(response_wide.c_str(), &response_variant);
    if (FAILED(hr) || !response_variant)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    CComPtr<DkmCustomMessage> reply;
    hr = DkmCustomMessage::Create(
        process->Connection(),
        process,
        bh_guid_agent_message_to_local,
        bh_agent_message_from_script,
        response_variant,
        nullptr,
        &reply);
    if (FAILED(hr) || !reply)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    *ppReplyMessage = reply.Detach();
    return S_OK;
}
