#include "stdafx.h"
#include "bh_agent_debugger_package.h"

class PackageModule : public CAtlDllModuleT<PackageModule>
{
};

PackageModule _AtlModule;

#define DEFAULT_REGISTRY_ROOT LREGKEY_VISUALSTUDIOROOT
#define VSL_REGISTER_TYPE_LIB FALSE

#include <VSLPackageDllEntryPoints.cpp>
