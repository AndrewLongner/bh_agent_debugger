@echo off
rem Finds the Visual Studio 2022 installation that builds BH Agent Debugger.
rem The projects use the VS 2022 (v143) C++ toolset, so building needs Visual Studio 2022
rem with "Desktop development with C++". The built extension runs in VS 2022 and VS 2026.
rem Set BH_VS_ROOT to a Visual Studio 2022 installation folder to skip the search.
rem Sets BH_VS_ROOT, BH_MSBUILD, BH_VCVARS64 and BH_DIA_SDK_INCLUDE for the caller.

if defined BH_VS_ROOT goto :validate

set "BH_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%BH_VSWHERE%" goto :no_vswhere

rem Read vswhere's output directly: no temp file, and a vswhere failure leaves BH_VS_ROOT empty.
for /f "usebackq delims=" %%i in (`"%BH_VSWHERE%" -nologo -prerelease -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath`) do set "BH_VS_ROOT=%%i"
if not defined BH_VS_ROOT goto :not_found

:validate
set "BH_MSBUILD=%BH_VS_ROOT%\MSBuild\Current\Bin\amd64\MSBuild.exe"
set "BH_VCVARS64=%BH_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "BH_DIA_SDK_INCLUDE=%BH_VS_ROOT%\DIA SDK\include"
if not exist "%BH_MSBUILD%" goto :incomplete
if not exist "%BH_VCVARS64%" goto :incomplete
exit /b 0

:no_vswhere
echo error: vswhere.exe not found. Install Visual Studio 2022 with "Desktop development with C++".
exit /b 1

:not_found
echo error: no Visual Studio 2022 installation with the C++ x64/x86 build tools was found.
echo Install Visual Studio 2022 with "Desktop development with C++", or set BH_VS_ROOT.
exit /b 1

:incomplete
echo error: "%BH_VS_ROOT%" is missing MSBuild or the C++ build tools.
exit /b 1
