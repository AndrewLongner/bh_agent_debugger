#pragma once

#include <Windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <vsdebugeng.h>
#include <vsdebugeng.templates.h>

#include "bh_agent_local_component_contract.h"

using namespace ATL;
using namespace Microsoft::VisualStudio::Debugger;

class ATL_NO_VTABLE bh_agent_local_component :
    public bh_agent_local_componentContract,
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<bh_agent_local_component, &bh_agent_local_componentContract::ClassId>
{
protected:
    bh_agent_local_component() {}
    ~bh_agent_local_component() {}

public:
    DECLARE_NO_REGISTRY();
    DECLARE_NOT_AGGREGATABLE(bh_agent_local_component);

    HRESULT STDMETHODCALLTYPE SendHigher(
        DkmCustomMessage* pCustomMessage,
        DkmCustomMessage** ppReplyMessage);

    HRESULT STDMETHODCALLTYPE SendLower(
        DkmCustomMessage* pCustomMessage,
        DkmCustomMessage** ppReplyMessage);
};

OBJECT_ENTRY_AUTO(bh_agent_local_component::ClassId, bh_agent_local_component)
