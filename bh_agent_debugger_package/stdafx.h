#pragma once

#define STRICT
#define _WIN32_WINNT 0x0601
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#define _ATL_APARTMENT_THREADED
#define _ATL_REGISTER_PER_USER

#include <atlbase.h>
#include <atlcom.h>
#include <atlwin.h>
#include <atlstr.h>

#include <windowsx.h>

#include <dte.h>
#include <objext.h>
#include <vshelp.h>
#include <uilocale.h>
#include <IVsQueryEditQuerySave2.h>
#include <vbapkg.h>
#include <fpstfmt.h>
#include <VSRegKeyNames.h>
#include <stdidcmd.h>
#include <stdiduie.h>
#include <textfind.h>
#include <textmgr.h>
#include <vsshell140.h>

#define VSLASSERT _ASSERTE
#define VSLASSERTEX(exp, szMsg) _ASSERT_BASE(exp, szMsg)
#define VSLTRACE ATLTRACE

#include <VSLPackage.h>
#include <VSLCommandTarget.h>
#include <VSLWindows.h>

using namespace VSL;
