@echo off
setlocal
call "%~dp0..\find_build_vs.cmd"
if errorlevel 1 exit /b 1

"%BH_MSBUILD%" ^
  "%~dp0TargetApp.sln" ^
  /t:Build ^
  /p:Configuration=Debug ^
  /p:Platform=x64 ^
  /nologo ^
  /v:m ^
  /m
