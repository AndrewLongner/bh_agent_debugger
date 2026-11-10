@echo off
setlocal

call "%~dp0..\find_build_vs.cmd"
if errorlevel 1 exit /b 1
set "MSBUILD_EXE=%BH_MSBUILD%"
set "SOLUTION=bh_agent_debugger_cli.sln"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=Build"

set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=Debug"

set "PLATFORM=%~3"
if "%PLATFORM%"=="" set "PLATFORM=x64"

if not exist "%MSBUILD_EXE%" (
  echo MSBuild not found at "%MSBUILD_EXE%"
  exit /b 1
)

echo Building %SOLUTION% target=%TARGET% config=%CONFIG% platform=%PLATFORM%

"%MSBUILD_EXE%" ^
  "%SOLUTION%" ^
  /t:%TARGET% ^
  /p:Configuration=%CONFIG% ^
  /p:Platform=%PLATFORM% ^
  /nologo ^
  /v:m ^
  /m ^
  /clp:NoSummary;NoItemAndPropertyList

exit /b %errorlevel%
