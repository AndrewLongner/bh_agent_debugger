@echo off
setlocal

set SRC=%~dp0
set DEST=%USERPROFILE%\.config\opencode

if not exist "%DEST%\tools" mkdir "%DEST%\tools"
if not exist "%DEST%\skills\bh-agent-debugger" mkdir "%DEST%\skills\bh-agent-debugger"

copy /Y "%SRC%tools\bh_agent_debugger.ts" "%DEST%\tools\bh_agent_debugger.ts" >nul
if errorlevel 1 (
  echo Failed to copy tool file.
  exit /b 1
)

copy /Y "%SRC%skills\bh-agent-debugger\SKILL.md" "%DEST%\skills\bh-agent-debugger\SKILL.md" >nul
if errorlevel 1 (
  echo Failed to copy skill file.
  exit /b 1
)

echo OpenCode Windows tool/skill synced to %DEST%
exit /b 0
