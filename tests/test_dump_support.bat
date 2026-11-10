@echo off
setlocal
pushd "%~dp0.."
call "%~dp0..\find_build_vs.cmd"
if errorlevel 1 exit /b 1
call "%BH_VCVARS64%"
if errorlevel 1 exit /b %errorlevel%
if not exist "Temp\tests" mkdir "Temp\tests"
cl /nologo /std:c++17 /EHsc /W4 /Fe:"Temp\tests\test_dump_support.exe" /Fo:"Temp\tests\test_dump_support.obj" "tests\test_dump_support.cpp" /link ole32.lib oleaut32.lib
if errorlevel 1 exit /b %errorlevel%
"Temp\tests\test_dump_support.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
