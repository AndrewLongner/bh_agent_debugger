@echo off
setlocal
pushd "%~dp0.."
call "%~dp0..\find_build_vs.cmd"
if errorlevel 1 exit /b 1
call "%BH_VCVARS64%"
if errorlevel 1 exit /b %errorlevel%
if not exist "Temp\tests" mkdir "Temp\tests"
cl /nologo /std:c++17 /EHsc /W4 /Zi /Fe:"Temp\tests\test_vs_profiler.exe" /Fo:"Temp\tests\test_vs_profiler.obj" /Fd:"Temp\tests\test_vs_profiler_compile.pdb" "tests\test_vs_profiler.cpp" /link /DEBUG /PDB:"Temp\tests\test_vs_profiler.pdb"
if errorlevel 1 exit /b %errorlevel%
"Temp\tests\test_vs_profiler.exe" %*
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
