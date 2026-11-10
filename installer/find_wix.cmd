@echo off
rem Locates the WiX v6.0.2 tools built from source by installer\build_wix_from_source.cmd.
rem Sets BH_WIX_EXE and BH_WIX_UI_EXT for the caller. Set both to use another source build.
if defined BH_WIX_EXE if defined BH_WIX_UI_EXT goto :validate
for %%I in ("%~dp0..\Temp\wix_v6.0.2_src\build") do set "BH_WIX_BUILD=%%~fI"
set "BH_WIX_EXE=%BH_WIX_BUILD%\wix\Release\publish\wix\wix.exe"
set "BH_WIX_UI_EXT=%BH_WIX_BUILD%\UI.wixext\Release\netstandard2.0\WixToolset.UI.wixext.dll"

:validate
if not exist "%BH_WIX_EXE%" goto :missing
if not exist "%BH_WIX_UI_EXT%" goto :missing
exit /b 0

:missing
echo error: WiX built from source was not found.
echo   expected "%BH_WIX_EXE%"
echo   and "%BH_WIX_UI_EXT%"
echo Run installer\build_wix_from_source.cmd first.
exit /b 1
