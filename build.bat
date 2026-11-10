@echo off
setlocal

call "%~dp0find_build_vs.cmd"
if errorlevel 1 exit /b 1
set "MSBUILD_EXE=%BH_MSBUILD%"
set "SOLUTION=bh_agent_debugger.sln"

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

if errorlevel 1 exit /b %errorlevel%

echo Building standalone service target=%TARGET% config=%CONFIG% platform=%PLATFORM%
"%MSBUILD_EXE%" ^
  "bh_agent_debugger_service\bh_agent_debugger_service\bh_agent_debugger_service.vcxproj" ^
  /t:%TARGET% ^
  /p:Configuration=%CONFIG% ^
  /p:Platform=%PLATFORM% ^
  /nologo ^
  /v:m ^
  /m ^
  /clp:NoSummary;NoItemAndPropertyList

if errorlevel 1 exit /b %errorlevel%

echo Building standalone CLI target=%TARGET% config=%CONFIG% platform=%PLATFORM%
"%MSBUILD_EXE%" ^
  "bh_agent_debugger_cli\bh_agent_debugger_cli\bh_agent_debugger_cli.vcxproj" ^
  /t:%TARGET% ^
  /p:Configuration=%CONFIG% ^
  /p:Platform=%PLATFORM% ^
  /nologo ^
  /v:m ^
  /m ^
  /clp:NoSummary;NoItemAndPropertyList

if errorlevel 1 exit /b %errorlevel%

call "bh_profile_analyzer\build_native.bat" "%TARGET%"
if errorlevel 1 exit /b %errorlevel%

rem Build the offline managed helper beside the native CLI, without editing projects.
set "ANALYZER_TARGET=build"
if /I "%TARGET%"=="Clean" set "ANALYZER_TARGET=clean"
dotnet %ANALYZER_TARGET% "bh_profile_analyzer\bh_profile_analyzer.csproj" -c "%CONFIG%" -o "Temp\bh_agent_debugger_cli_%PLATFORM%_%CONFIG%" -p:RestoreLockedMode=true --nologo
exit /b %errorlevel%
