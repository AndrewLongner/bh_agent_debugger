#pragma once

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

// LaunchDebugTargets2 is [in,out]. Its COM proxy may free/replace every nested
// pointer during unmarshalling. Do not lend it stack arrays or CComBSTR aliases.
// Target is VsDebugTargetInfo2 in production; the test uses its owning fields.
template<class Target>
class bh_dump_launch_target
{
public:
    Target value = {};

    bh_dump_launch_target(const wchar_t* path, const GUID& engine)
    {
        value.cbSize = sizeof(Target);
        value.bstrExe = SysAllocString(path);
        value.guidLaunchDebugEngine = engine;
        value.pDebugEngines = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
        if (value.pDebugEngines)
        {
            value.pDebugEngines[0] = engine;
            value.dwDebugEngineCount = 1;
        }
    }

    bool valid() const
    {
        return value.bstrExe && value.pDebugEngines;
    }

    ~bh_dump_launch_target()
    {
        // Free the returned pointers, not copies of the original input pointers.
        SysFreeString(value.bstrRemoteMachine);
        SysFreeString(value.bstrExe);
        SysFreeString(value.bstrArg);
        SysFreeString(value.bstrCurDir);
        SysFreeString(value.bstrEnv);
        SysFreeString(value.bstrPortName);
        SysFreeString(value.bstrOptions);
        CoTaskMemFree(value.pDebugEngines);
        if (value.pUnknown) value.pUnknown->Release();
    }

    bh_dump_launch_target(const bh_dump_launch_target&) = delete;
    bh_dump_launch_target& operator=(const bh_dump_launch_target&) = delete;
};
