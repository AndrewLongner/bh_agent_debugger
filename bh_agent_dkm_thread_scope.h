#pragma once
#include <vsdebugeng.h>

// Design-mode package autoload can precede Concord's component store. Retry
// initialization for later commands and balance only initialization we own.
class bh_agent_dkm_thread_scope
{
    bool owned_ = false;
    bool ready_ = false;

    static const GUID& component()
    {
        static const GUID id = {0x87f9e4dd, 0x3fd0, 0x4a2f, {0x9f, 0x56, 0xec, 0xf4, 0xc9, 0xa6, 0xd8, 0x02}};
        return id;
    }

public:
    HRESULT ensure()
    {
        if (ready_) return S_OK;
        const HRESULT hr = Microsoft::VisualStudio::Debugger::DkmComponentManager::InitializeThread(component());
        ready_ = SUCCEEDED(hr);
        owned_ = hr == S_OK;
        return hr;
    }

    ~bh_agent_dkm_thread_scope()
    {
        if (owned_) (void)Microsoft::VisualStudio::Debugger::DkmComponentManager::UninitializeThread(component());
    }

    bh_agent_dkm_thread_scope() = default;
    bh_agent_dkm_thread_scope(const bh_agent_dkm_thread_scope&) = delete;
    bh_agent_dkm_thread_scope& operator=(const bh_agent_dkm_thread_scope&) = delete;
};
