@echo off
setlocal
pushd "%~dp0.."
call "%~dp0..\find_build_vs.cmd"
if errorlevel 1 exit /b 1
call "%BH_VCVARS64%" >nul
if errorlevel 1 exit /b %errorlevel%
if not exist Temp\tests mkdir Temp\tests
cl /nologo /EHsc /W4 /O2 /Ob2 /Zo /Zi /MT /LD tests\source_inline_fixture.cpp /Fo:Temp\tests\source_inline_fixture.obj /Fd:Temp\tests\source_inline_fixture_compile.pdb /link /DEBUG /OUT:Temp\tests\source_inline_fixture.dll /PDB:Temp\tests\source_inline_fixture.pdb /IMPLIB:Temp\tests\source_inline_fixture.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /MT /I"%BH_DIA_SDK_INCLUDE%" tests\test_profile_dia.cpp /Fo:Temp\tests\test_profile_dia.obj /Fe:Temp\tests\test_profile_dia.exe /link ole32.lib oleaut32.lib uuid.lib
if errorlevel 1 exit /b %errorlevel%
Temp\tests\test_profile_dia.exe "%CD%\Temp\bh_agent_debugger_cli_x64_Debug\amd64\msdia140.dll" "%CD%\Temp\tests\source_inline_fixture.pdb"
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
