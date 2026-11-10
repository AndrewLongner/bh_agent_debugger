# BH Agent Debugger MSI

This folder contains a WiX installer for BH Agent Debugger runtime artifacts.

License agreement shown in installer UI: MIT License
Reference: https://opensource.org/license/mit

## Build

The MSI is built with WiX Toolset v6.0.2 compiled from source; no WiX binary release or WiX
package is downloaded. Build WiX once (this takes a while):

```bat
installer\build_wix_from_source.cmd
```

The script clones the `v6.0.2` tag into `Temp\wix_v6.0.2_src`, checks the commit, fetches a pinned and
signature-checked `nuget.exe` (`get_nuget_exe.ps1`), pins WiX's build to the .NET 9 SDK
(`pin_wix_dotnet_sdk.ps1`) and runs the steps of WiX's own `devbuild.cmd release` in a Visual Studio
2022 developer prompt, with its NuGet caches redirected under `Temp\wix_build_home`. It needs Git, the
.NET 9 SDK and the Visual Studio 2022 components listed in WiX's
[prerequisites](https://github.com/wixtoolset/wix#prerequisites). WiX's third-party build
dependencies are restored from nuget.org; WiX's own packages come only from its build output.

The script stops after WiX's extensions, which is all the MSI needs; WiX's own setup and integration
tests are skipped. WiX's unit tests run, except two that download WiX packages from nuget.org while they
run (`ExtensionFixture.TryingToAddAV4ExtensionFails` and
`MsmqExtensionFixture.CanBuildUsingMessageQueue`). Afterwards, `audit_wix_package_sources.ps1` fails
the build if any WiX package was requested from a remote feed or came from anywhere but the build
output.

`find_wix.cmd` locates the result (`BH_WIX_EXE`, `BH_WIX_UI_EXT`); set both variables to use another
source build.

Then run from repo root:

```bat
installer\build_installer.bat
```

`build_installer.bat` rebuilds the custom action, VSIX, broker service, CLI and analyzer in **Release**,
builds the MSI with the source-built WiX and checks the payload with `verify_release_payload.ps1`.
Set `BH_INSTALLER_CONFIGURATION=Debug` only to debug the installer locally.

## Automated verification

Full verification builds the MSI, checks its UI routes (by decompiling it) and that it installs per
user only, then **silently installs the MSI for the current user and uninstalls it**, which replaces any
installed copy. Add `-SkipInstall` to check the MSI without installing it:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File installer\verify_installer.ps1
```

Useful switches:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File installer\verify_installer.ps1 -SkipInstall
powershell -NoProfile -ExecutionPolicy Bypass -File installer\verify_installer.ps1 -SkipUninstall
powershell -NoProfile -ExecutionPolicy Bypass -File installer\verify_installer.ps1 -RunUiFlow
```

Interactive UI flow validation (catches dialog navigation/runtime UI issues like `Error 2343`):

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File installer\verify_installer_ui.ps1
```

Notes:

- Requires a desktop session (not headless/SSH-only).
- The script automates `Next`/`Install` and then silently uninstalls for cleanup.

Output:

- `Temp\installer\bh_agent_debugger.msi`

## What it installs

- `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt` (this product)
- `bin\bh_agent_debugger_cli.exe`
- `bin\bh_agent_debugger_service.exe`
- the self-contained CPU profile analyzer in `bin\` (`bh_profile_analyzer.exe`, `bh_profile_dia.dll`,
  `amd64\`), with the .NET runtime's `dotnet-LICENSE.txt` and `dotnet-THIRD-PARTY-NOTICES.txt`
- bundled `bh_agent_debugger.vsix` under `extension\`
- optional OpenCode payload under `opencode\`

## Installer behavior

- Installs per user only and needs no administrator rights: the files go under
  `%LOCALAPPDATA%\Programs\BH Agent Debugger`, the settings under HKCU and the extension into your own
  Visual Studio profiles. Each Windows account that wants the debugger installs it for itself.
- Sets environment variables in user scope:
  - `BH_AGENT_SERVICE_EXE=[BINFOLDER]bh_agent_debugger_service.exe`
  - `BH_AGENT_SERVICE_TASK=bh_agent_debugger_start_service_desktop_user`
- Optional feature checkbox in UI: `Add CLI to PATH`.
  - Appends `[BINFOLDER]` to user PATH.
- Creates user scheduled task:
  - `bh_agent_debugger_start_service_desktop_user`
  - Interactive token, limited run level.
- Installs the VS extension into every VS 2022 and VS 2026 instance with that instance's
  `VSIXInstaller.exe /quiet` from a custom action (VS must be closed), then runs
  `devenv.exe /updateconfiguration` for each instance so Visual Studio loads the package on its next start
  (VSIXInstaller does not always refresh the configuration). Uninstalling removes the extension the same
  way and refreshes the configuration again.
- Optional feature `Claude Code Skill and MCP Server` (on by default). When Claude Code is installed
  (a `%CLAUDE_CONFIG_DIR%` or `%USERPROFILE%\.claude` folder, or a `claude` command on the user `PATH` or
  in `%USERPROFILE%\.local\bin`), it copies the skill to
  `<Claude config folder>\skills\bh-agent-debugger\SKILL.md` and runs
  `claude mcp add --scope user bh_agent_debugger -- "[BINFOLDER]bh_agent_debugger_cli.exe" mcp`
  (60-second limit). It never edits Claude Code's configuration files itself, and keeps an MCP server
  named `bh_agent_debugger` that you added yourself. Without Claude Code it only logs
  `skip=claude_code_not_found`. Uninstalling removes only what the installer added, as recorded under
  `HKCU\Software\Boiling Lava Hot\BH Agent Debugger\ClaudeCode`. The verbose log lines start with
  `SyncClaudeCode` and `CleanupClaudeCode`.
- Optional feature `Codex Skill and MCP Server (Windows)` (on by default). When Codex is installed on
  Windows (a `%CODEX_HOME%` or `%USERPROFILE%\.codex` folder, or `codex.exe` or `codex.cmd` on the user
  `PATH`), it copies the skill to `%USERPROFILE%\.agents\skills\bh-agent-debugger\SKILL.md` and runs
  `codex mcp add bh_agent_debugger -- "[BINFOLDER]bh_agent_debugger_cli.exe" mcp` (60-second limit for
  each command). Because `codex mcp add` replaces a server of the same name together with the settings
  added to it by hand, the installer runs `codex mcp get bh_agent_debugger` first. It keeps a server that
  you added yourself (`mcp skip=server_added_by_user_kept`) and its own registration when that already
  starts this CLI (`mcp kept=already_registered`), so a `tool_timeout_sec` you added stays. It never edits
  `config.toml` itself. When `codex --version` fails, for example for an npm install on a Node.js older
  than Codex needs, it copies the skill but registers nothing (`mcp skip=codex_does_not_run`). Without Codex it only logs `skip=codex_not_found`. Uninstalling removes only what
  the installer added, as recorded under `HKCU\Software\Boiling Lava Hot\BH Agent Debugger\Codex`. The
  verbose log lines start with `SyncCodex` and `CleanupCodex`.
- Optional feature `Codex Skill and MCP Server (WSL)` (on by default). Does the same for Codex in the
  default WSL distribution: the skill goes to `~/.agents/skills/bh-agent-debugger/SKILL.md`, and the
  registered command is the Windows CLI under `/mnt/<drive>/...`, which Codex starts through WSL interop.
  The commands run through `wsl.exe -e bash -lic` (120-second limit); the interactive login shell finds a
  `codex` installed with a Node version manager that `~/.bashrc` sets up. The log line is
  `SyncCodexWsl exit_code=<n> result=<text>`, where the result is `done`, `mcp_already_registered`,
  `server_added_by_user_kept`, `codex_not_found`, `codex_does_not_run`, `skill_copy_failed`,
  `cli_not_reachable` or `mcp_add_failed`; without WSL it is `SyncCodexWsl skip=no_distros`. State is
  under `HKCU\Software\Boiling Lava Hot\BH Agent Debugger\CodexWsl`; `CleanupCodexWsl` removes only what
  is recorded there and does not start WSL when nothing is.

## Feature IDs and Silent Install Examples

Feature ids:

- `MainFeature` (required)
- `CliPathFeature`
- `OpenCodeWindowsFeature`
- `OpenCodeWslFeature`
- `ClaudeCodeFeature`
- `CodexFeature`
- `CodexWslFeature`

Per-user silent install with core + CLI PATH + OpenCode Windows:

```bat
msiexec /i Temp\installer\bh_agent_debugger.msi ADDLOCAL=MainFeature,CliPathFeature,OpenCodeWindowsFeature /qn /l*v Temp\installer\install_user.log
```

Per-user silent install with core + OpenCode WSL only:

```bat
msiexec /i Temp\installer\bh_agent_debugger.msi ADDLOCAL=MainFeature,OpenCodeWslFeature /qn /l*v Temp\installer\install_user_wsl.log
```

Quiet install fails when VS is open (extension feature enabled):

```bat
msiexec /i Temp\installer\bh_agent_debugger.msi /qn /l*v Temp\installer\install_quiet.log
```

## Updating an existing install (recommended)

After `build.bat` and `installer\build_installer.bat`, close Visual Studio and run:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File installer\update.ps1 -DryRun
powershell -NoProfile -ExecutionPolicy Bypass -File installer\update.ps1
```

- Installs per user. The MSI's `MajorUpgrade` replaces an older per-user version in one transaction. A per-machine install left by 0.12.x is invisible to a per-user upgrade, so the script uninstalls it first; only that step asks for administrator rights (UAC).
- Refuses same-version or downgrade MSIs (bump the version first; WiX generates a new ProductCode on every build), a running Visual Studio, other processes running from a Visual Studio install folder (VSIXInstaller exits 2004 while, for example, idle MSBuild nodes from a build run), or a running broker unless `-StopBroker`.
- Writes `Temp\installer\update_<version>_<timestamp>.log` (verbose) plus `.summary.txt`, never reboots, then validates ProductState, version, per-user registration, old ProductCodes, log status, the extension install and configuration refresh for each Visual Studio instance and the installed CLI `--version`, and lists the slowest installer actions.
- Options: `-Quiet` (no progress window), `-InstallDir <dir>` (custom install folder), `-LogPath <file>`.

Upgrade speed (0.12.5+ MSIs): no System Restore point (`MSIFASTINSTALL=1`); the old product no longer uninstalls the VSIX/OpenCode payloads while being upgraded (the new one updates them in place); the analyzer is a single-file bundle (~7 files instead of ~205). The cleanup skip applies when upgrading *from* a version that has it, so the first upgrade away from 0.12.4 or older still runs the old cleanups.

## msiexec examples

Install (interactive UI, per-user):

```bat
msiexec /i Temp\installer\bh_agent_debugger.msi
```

Uninstall:

```bat
msiexec /x Temp\installer\bh_agent_debugger.msi
```
