@echo off
rem Builds the WiX Toolset v6.0.2 from source for the BH Agent Debugger MSI.
rem No WiX binary release or WiX package is downloaded: this clones the v6.0.2 source tag and
rem builds it in a Visual Studio 2022 developer prompt with the steps of WiX's own
rem "devbuild.cmd release", up to and including the extensions (see :build_wix).
rem WiX's build still restores its third-party build dependencies (for example MSTest) from
rem nuget.org. Visual Studio 2022 prerequisites: https://github.com/wixtoolset/wix#prerequisites
rem WiX's build uses the .NET 9 SDK (pin_wix_dotnet_sdk.ps1): the .NET 10 SDK needs Visual Studio 2026.
rem The build runs with USERPROFILE and NUGET_PACKAGES redirected under Temp, so WiX's clean
rem steps and its self-built 6.0.2 packages never touch your global NuGet cache.
rem audit_wix_package_sources.ps1 then checks that every WiX package came from this build.
rem Output: Temp\wix_v6.0.2_src\build, used by installer\build_installer.bat.
setlocal EnableExtensions DisableDelayedExpansion
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "WIX_TAG=v6.0.2"
set "WIX_COMMIT=b3f340393117094a75ea8ced77f2357e4aa095e7"
set "WIX_SRC=%ROOT%\Temp\wix_v6.0.2_src"
set "WIX_HOME=%ROOT%\Temp\wix_build_home"
set "TOOLS=%ROOT%\Temp\tools"

where git >nul 2>&1
if errorlevel 1 (
  echo error: git not found. Install Git for Windows.
  exit /b 1
)

if not exist "%WIX_SRC%\.git" (
  git clone --depth 1 --branch %WIX_TAG% https://github.com/wixtoolset/wix.git "%WIX_SRC%"
  if errorlevel 1 exit /b 1
)
set "WIX_HEAD="
for /f %%c in ('git -C "%WIX_SRC%" rev-parse HEAD') do set "WIX_HEAD=%%c"
if /i not "%WIX_HEAD%"=="%WIX_COMMIT%" (
  echo error: %WIX_SRC% is at "%WIX_HEAD%", expected %WIX_TAG% commit %WIX_COMMIT%.
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get_nuget_exe.ps1" -Destination "%TOOLS%\nuget.exe"
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pin_wix_dotnet_sdk.ps1" -WixSource "%WIX_SRC%"
if errorlevel 1 exit /b 1

call "%ROOT%\find_build_vs.cmd"
if errorlevel 1 exit /b 1
call "%BH_VS_ROOT%\Common7\Tools\VsDevCmd.bat" -no_logo
if not defined VCToolsVersion (
  echo error: the Visual Studio 2022 developer prompt did not initialize.
  exit /b 1
)

set "PATH=%TOOLS%;%PATH%"
set "USERPROFILE=%WIX_HOME%"
set "NUGET_PACKAGES=%WIX_HOME%\.nuget\packages"
set "NUGET_HTTP_CACHE_PATH=%WIX_HOME%\.nuget\http-cache"
set "DOTNET_CLI_HOME=%WIX_HOME%"
set "DOTNET_CLI_TELEMETRY_OPTOUT=1"
set "DOTNET_NOLOGO=1"
rem No MSBuild node stays behind holding files in the WiX build folders.
set "MSBUILDDISABLENODEREUSE=1"
rem WiX builds with -warnaserror. Keep these as visible warnings instead of errors:
rem - NU1901/NU1902: low/moderate NuGet audit advisories published after the v6.0.2 release
rem   (for example on build-time SourceLink packages). High and critical (NU1903/NU1904) still fail.
set "MSBuildWarningsNotAsErrors=NU1901;NU1902"
rem WiX v6.0.2 still targets net6.0, which is now out of support. Its SDK tests require a
rem warning-free build, so turn off the end-of-life check (NETSDK1138) for this build only.
set "CheckEolTargetFramework=false"
if not exist "%NUGET_PACKAGES%" mkdir "%NUGET_PACKAGES%"
rem Burn's unit tests compare its known-folder paths with .NET's, and .NET reports "" for a
rem folder that does not exist, so give the redirected profile its standard folders.
for %%d in (Desktop Documents Favorites Pictures "AppData\Roaming\Microsoft\Windows\SendTo" "AppData\Roaming\Microsoft\Windows\Templates" "AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup") do if not exist "%WIX_HOME%\%%~d" mkdir "%WIX_HOME%\%%~d"
rem Start with an empty NuGet HTTP cache, so the audit after the build sees every remote request.
if exist "%NUGET_HTTP_CACHE_PATH%" rd /s /q "%NUGET_HTTP_CACHE_PATH%"

echo Building WiX %WIX_TAG% from source (this takes a while)...
pushd "%WIX_SRC%\src"
call :build_wix
set "WIX_BUILD_EXIT=%errorlevel%"
popd

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0audit_wix_package_sources.ps1" -WixHome "%WIX_HOME%" -Artifacts "%WIX_SRC%\build\artifacts"
if errorlevel 1 exit /b 1
if not "%WIX_BUILD_EXIT%"=="0" (
  echo error: the WiX source build failed.
  exit /b 1
)
call "%~dp0find_wix.cmd"
if errorlevel 1 (
  echo error: the WiX source build did not produce wix.exe and the UI extension.
  exit /b 1
)
echo WiX built from source: "%BH_WIX_EXE%"
echo UI extension: "%BH_WIX_UI_EXT%"
exit /b 0

rem The steps of WiX's src\build_all.cmd through the extensions, which is all the MSI needs.
rem WiX's own setup, integration-test and finalize steps are skipped. The wix and ext steps are
rem WiX's wix\wix.cmd and ext\ext.cmd, with two WiX tests excluded because they fetch WiX
rem packages from nuget.org while they run:
rem - ExtensionFixture.TryingToAddAV4ExtensionFails adds WixToolset.UI.wixext/4.0.4.
rem - MsmqExtensionFixture.CanBuildUsingMessageQueue adds the newest WixToolset.Util.wixext on
rem   any feed, which is now a v7 package from nuget.org.
rem Exit code 8 means that no test ran: the other Msmq test is skipped by WiX itself.
:build_wix
set "WIX_C=Release"
set "WIX_L=%WIX_SRC%\build\logs"
set "WIX_B=%WIX_SRC%\build\wix\Release"
call clean.cmd
call build_init.cmd || exit /b 1
for %%s in (dtf\dtf.cmd internal\internal.cmd libs\libs.cmd api\api.cmd burn\burn.cmd) do (
  call %%s %WIX_C% || exit /b 1
)
call wix\wix.cmd %WIX_C% clean
pushd wix
call :build_wix_segment
set "WIX_SEGMENT_EXIT=%errorlevel%"
popd
if not "%WIX_SEGMENT_EXIT%"=="0" exit /b 1
call tools\tools.cmd %WIX_C% || exit /b 1
msbuild ext\ext_t.proj -p:Configuration=%WIX_C% -p:SuppressWixClean=false "-p:TestingPlatformCommandLineArguments=--filter FullyQualifiedName!=WixToolsetTest.Msmq.MsmqExtensionFixture.CanBuildUsingMessageQueue --ignore-exit-code 8" -m -tl -nologo -warnaserror "-bl:%WIX_L%\ext_build.binlog" || exit /b 1
exit /b 0

:build_wix_segment
msbuild -t:Restore wix.sln -p:Configuration=%WIX_C% -tl -nologo -m -warnaserror "-bl:%WIX_L%\wix_restore.binlog" || exit /b 1
msbuild wixnative\wixnative_t.proj -p:Configuration=%WIX_C% -tl -nologo -m -warnaserror "-bl:%WIX_L%\wixnative_build.binlog" || exit /b 1
msbuild wix.sln -p:Configuration=%WIX_C% -tl -nologo -m -warnaserror "-bl:%WIX_L%\wix_build.binlog" || exit /b 1
msbuild publish_t.proj -p:Configuration=%WIX_C% -tl -nologo -warnaserror "-bl:%WIX_L%\wix_publish.binlog" || exit /b 1
msbuild -t:Publish -p:Configuration=%WIX_C% -tl -nologo -warnaserror WixToolset.Sdk\WixToolset.Sdk.csproj "-bl:%WIX_L%\wix_sdk_publish.binlog" || exit /b 1
msbuild -t:Test test\WixToolsetTest.Sdk\WixToolsetTest.Sdk.csproj -p:Configuration=%WIX_C% -tl -nologo -warnaserror || exit /b 1
dotnet test ^
 "%WIX_B%\test\WixToolsetTest.Converters\net6.0\WixToolsetTest.Converters.dll" ^
 "%WIX_B%\test\WixToolsetTest.Converters.Symbolizer\net472\WixToolsetTest.Converters.Symbolizer.dll" ^
 "%WIX_B%\test\WixToolsetTest.Core\net6.0\WixToolsetTest.Core.dll" ^
 "%WIX_B%\test\WixToolsetTest.Core.Native\net6.0\win-x64\WixToolsetTest.Core.Native.dll" ^
 "%WIX_B%\test\WixToolsetTest.CoreIntegration\net6.0\WixToolsetTest.CoreIntegration.dll" ^
 "%WIX_B%\test\WixToolsetTest.BuildTasks\net472\WixToolsetTest.BuildTasks.dll" ^
 --nologo --filter "FullyQualifiedName!=WixToolsetTest.CoreIntegration.ExtensionFixture.TryingToAddAV4ExtensionFails" -l "trx;LogFileName=%WIX_L%\TestResults\wix.trx" || exit /b 1
msbuild pack_t.proj -p:Configuration=%WIX_C% -tl -nologo -m -warnaserror "-bl:%WIX_L%\wix_pack.binlog" || exit /b 1
exit /b 0
