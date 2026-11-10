#include "stdafx.h"
#include "bh_agent_debugger_event_handler.h"
#include "..\bh_agent_log.h"

#include <stdio.h>

namespace
{
}

HRESULT bh_agent_debugger_event_handler::OnCustomDebugEvent(REFGUID processId, ::VsComponentMessage message)
{
    (void)processId;

    bh_agent_logf("package.event", "message=%u", (unsigned)message.MessageCode);

    return S_OK;
}

HRESULT bh_agent_debugger_event_handler::OnCustomStoppingDebugEvent(REFGUID processId, ::VsComponentMessage message, BOOL* pContinueExecution)
{
    (void)processId;
    (void)message;

    if (!pContinueExecution)
    {
        return E_POINTER;
    }

    *pContinueExecution = TRUE;

    return S_OK;
}
