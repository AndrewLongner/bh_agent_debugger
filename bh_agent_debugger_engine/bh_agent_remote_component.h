#pragma once

#include <Windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <vsdebugeng.h>
#include <vsdebugeng.templates.h>

#include "bh_agent_remote_component_contract.h"

using namespace ATL;
using namespace Microsoft::VisualStudio::Debugger;

class ATL_NO_VTABLE bh_agent_remote_component :
    public bh_agent_remote_componentContract,
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<bh_agent_remote_component, &bh_agent_remote_componentContract::ClassId>
{
protected:
    bh_agent_remote_component() {}
    ~bh_agent_remote_component() {}

public:
    DECLARE_NO_REGISTRY();
    DECLARE_NOT_AGGREGATABLE(bh_agent_remote_component);

    HRESULT STDMETHODCALLTYPE OnModuleInstanceLoad(
        DkmModuleInstance* moduleInstance,
        DkmWorkList* workList,
        DkmEventDescriptorS* eventDescriptor);

    HRESULT STDMETHODCALLTYPE OnRuntimeBreakpointReceived(
        Breakpoints::DkmRuntimeBreakpoint* pRuntimeBreakpoint,
        DkmThread* pThread,
        bool HasException,
        DkmEventDescriptorS* pEventDescriptor);

    HRESULT STDMETHODCALLTYPE OnStepCompleteReceived(
        Stepping::DkmStepper* pStepper,
        DkmThread* pThread,
        bool HasException,
        DkmEventDescriptorS* pEventDescriptor);

    HRESULT STDMETHODCALLTYPE OnSingleStepCompleteReceived(
        Stepping::DkmSingleStepRequest* pSingleStepRequest,
        DkmEventDescriptorS* pEventDescriptor);

    HRESULT STDMETHODCALLTYPE OnAsyncBreakCompleteReceived(
        DkmProcess* pProcess,
        DkmAsyncBreakStatus_t Status,
        DkmThread* pThread,
        DkmEventDescriptorS* pEventDescriptor);

    HRESULT STDMETHODCALLTYPE EnableRuntimeBreakpoint(
        Breakpoints::DkmRuntimeBreakpoint* pRuntimeBreakpoint);

    HRESULT STDMETHODCALLTYPE TestRuntimeBreakpoint(
        Breakpoints::DkmRuntimeBreakpoint* pRuntimeBreakpoint);

    HRESULT STDMETHODCALLTYPE DisableRuntimeBreakpoint(
        Breakpoints::DkmRuntimeBreakpoint* pRuntimeBreakpoint);

    HRESULT STDMETHODCALLTYPE OnDebugMonitorException(
        Exceptions::DkmExceptionInformation* pException,
        DkmWorkList* pWorkList,
        DkmEventDescriptorS* pEventDescriptor);

    HRESULT STDMETHODCALLTYPE OnProcessContinue(
        DkmProcess* pProcess);

    HRESULT STDMETHODCALLTYPE SendLower(
        DkmCustomMessage* pCustomMessage,
        DkmCustomMessage** ppReplyMessage);
};

OBJECT_ENTRY_AUTO(bh_agent_remote_component::ClassId, bh_agent_remote_component)
