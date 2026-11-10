@echo off
setlocal

set SRC=%~dp0
set SHFILE=%SRC%sync_opencode_wsl.sh
set WSL_SH=
set HAS_DISTRO=

if not exist "%SHFILE%" (
  echo Missing %SHFILE%
  exit /b 1
)

for /f "usebackq delims=" %%I in (`wsl.exe wslpath -a "%SHFILE%" 2^>nul`) do set WSL_SH=%%I
if "%WSL_SH%"=="" (
  echo Failed to resolve WSL path for %SHFILE%
  exit /b 1
)

for /f "usebackq delims=" %%I in (`wsl.exe -l -q 2^>nul`) do (
  if not "%%I"=="" set HAS_DISTRO=1
)

if "%HAS_DISTRO%"=="" (
  echo No WSL distributions are installed.
  exit /b 1
)

wsl.exe bash -lc "chmod +x '%WSL_SH%' && '%WSL_SH%'" 2>nul
if errorlevel 1 (
  echo Failed to sync OpenCode files to WSL.
  exit /b 1
)

echo OpenCode WSL sync completed.
exit /b 0
