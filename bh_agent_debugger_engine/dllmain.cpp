#include <atlbase.h>
#include <atlcom.h>

class bh_agent_debugger_engine_module : public CAtlDllModuleT<bh_agent_debugger_engine_module>
{
};

class bh_agent_debugger_engine_module _AtlModule;

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    (void)hInstance;
    return _AtlModule.DllMain(dwReason, lpReserved);
}

__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow(void)
{
    return _AtlModule.DllCanUnloadNow();
}

_Check_return_
STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}
