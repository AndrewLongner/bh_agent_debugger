@echo off
setlocal
set "OUT=%~dp0..\Temp\bh_profile_native_x64"
if /I "%~1"=="Clean" (
  if exist "%OUT%" rmdir /s /q "%OUT%"
  exit /b 0
)
call "%~dp0..\find_build_vs.cmd"
if errorlevel 1 exit /b 1
call "%BH_VCVARS64%" >nul
if errorlevel 1 exit /b %errorlevel%
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /LD /I"%BH_DIA_SDK_INCLUDE%" /Fo:"%OUT%\bh_profile_dia.obj" "%~dp0bh_profile_dia.cpp" /link /OUT:"%OUT%\bh_profile_dia.dll" /IMPLIB:"%OUT%\bh_profile_dia.lib" ole32.lib oleaut32.lib uuid.lib
exit /b %errorlevel%
