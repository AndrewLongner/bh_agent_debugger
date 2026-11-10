#pragma once

#include <atlbase.h>
#include <atlcom.h>
#include <msdbg110.h>

class ATL_NO_VTABLE bh_agent_debugger_event_handler :
    public CComObjectRootEx<CComSingleThreadModel>,
    public IVsCustomDebuggerEventHandler110,
    public IVsCustomDebuggerStoppingEventHandler110
{
public:
    BEGIN_COM_MAP(bh_agent_debugger_event_handler)
        COM_INTERFACE_ENTRY(IVsCustomDebuggerEventHandler110)
        COM_INTERFACE_ENTRY(IVsCustomDebuggerStoppingEventHandler110)
    END_COM_MAP()

    void SetServiceProvider(IServiceProvider* provider)
    {
        m_serviceProvider = provider;
    }

    IFACEMETHOD(OnCustomDebugEvent)(REFGUID processId, ::VsComponentMessage message) override;
    IFACEMETHOD(OnCustomStoppingDebugEvent)(REFGUID processId, ::VsComponentMessage message, BOOL* pContinueExecution) override;

private:
    CComPtr<IServiceProvider> m_serviceProvider;
};
