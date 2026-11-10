@echo off
setlocal

rem Builds the MSI from Release binaries with WiX built from source (build_wix_from_source.cmd).
rem Set BH_INSTALLER_CONFIGURATION=Debug only to debug the installer locally; releases use Release.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "OUTDIR=%ROOT%\Temp\installer"
set "CONFIG=Release"
if defined BH_INSTALLER_CONFIGURATION set "CONFIG=%BH_INSTALLER_CONFIGURATION%"
if /i not "%CONFIG%"=="Release" if /i not "%CONFIG%"=="Debug" (
  echo error: BH_INSTALLER_CONFIGURATION must be Release or Debug, not "%CONFIG%".
  exit /b 1
)
call "%ROOT%\find_build_vs.cmd"
if errorlevel 1 exit /b 1
set "MSBUILD=%BH_MSBUILD%"
call "%~dp0find_wix.cmd"
if errorlevel 1 exit /b 1

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

if not exist "%MSBUILD%" (
  echo MSBuild not found: %MSBUILD%
  exit /b 1
)

echo Building installer custom action DLL (%CONFIG%)...
"%MSBUILD%" "%~dp0bh_agent_debugger_installer_ca.vcxproj" /t:Build /p:Configuration=%CONFIG% /p:Platform=x64 /m
if errorlevel 1 (
  echo custom action build failed
  exit /b 1
)

echo Rebuilding VSIX payload and dependencies (%CONFIG%)...
"%MSBUILD%" "%ROOT%\bh_agent_debugger.sln" /t:vsix:Rebuild /p:Configuration=%CONFIG% /p:Platform=x64 /m
if errorlevel 1 (
  echo vsix rebuild failed
  exit /b 1
)

echo Rebuilding broker service and CLI (%CONFIG%)...
"%MSBUILD%" "%ROOT%\bh_agent_debugger_service\bh_agent_debugger_service\bh_agent_debugger_service.vcxproj" /t:Rebuild /p:Configuration=%CONFIG% /p:Platform=x64 /m
if errorlevel 1 (
  echo service build failed
  exit /b 1
)
"%MSBUILD%" "%ROOT%\bh_agent_debugger_cli\bh_agent_debugger_cli\bh_agent_debugger_cli.vcxproj" /t:Rebuild /p:Configuration=%CONFIG% /p:Platform=x64 /m
if errorlevel 1 (
  echo CLI build failed
  exit /b 1
)

echo Building native DIA helper...
call "%ROOT%\bh_profile_analyzer\build_native.bat" Build
if errorlevel 1 (
  echo DIA helper build failed
  exit /b 1
)

echo Publishing self-contained CPU analyzer payload (%CONFIG%)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish_analyzer.ps1" -Configuration %CONFIG%
if errorlevel 1 (
  echo analyzer publish failed
  exit /b 1
)

echo Building BH Agent Debugger MSI (%CONFIG% binaries)...
"%BH_WIX_EXE%" build "%~dp0Product.wxs" -d BhConfiguration=%CONFIG% -ext "%BH_WIX_UI_EXT%" -o "%OUTDIR%\bh_agent_debugger.msi"
if errorlevel 1 (
  echo wix build failed
  exit /b 1
)

echo Verifying VSIX and MSI payload freshness...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify_release_payload.ps1" ^
  -Configuration %CONFIG% ^
  -VsixPath "%ROOT%\Temp\vsix_x64_%CONFIG%\bh_agent_debugger.vsix" ^
  -PackageDllPath "%ROOT%\Temp\bh_agent_debugger_package_x64_%CONFIG%\bh_agent_debugger_package.dll" ^
  -SourceManifestPath "%ROOT%\vsix\source.extension.vsixmanifest" ^
  -ProductWxsPath "%~dp0Product.wxs" ^
  -MsiPath "%OUTDIR%\bh_agent_debugger.msi" ^
  -AnalyzerDirectory "%OUTDIR%\analyzer"
if errorlevel 1 (
  echo release payload verification failed
  exit /b 1
)

echo MSI output: %OUTDIR%\bh_agent_debugger.msi
exit /b 0
