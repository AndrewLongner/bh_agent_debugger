# AGENTS.md

This file is for coding agents working in `bh_agent_debugger`.
It documents build/validation commands and local code conventions.

## Scope

- Applies to the full repository rooted at `bh_agent_debugger/`.
- Primary language/tooling: C++ (ATL/COM, VSSDK, DKM) + MSBuild + PowerShell.
- Primary deliverable: VSIX containing debugger engine + package.

## Repository Layout

- `bh_agent_debugger.sln`: solution with all projects.
- `bh_agent_debugger/`: scratch console project (not core extension logic).
- `bh_agent_debugger_engine/`: debugger engine DLL and DKM components.
- `bh_agent_debugger_package/`: Visual Studio package/service side.
- `bh_agent_debugger_cli/`: standalone CLI client (`bh_agent_debugger_cli.exe`).
- `bh_agent_debugger_service/`: standalone broker service (`bh_agent_debugger_service.exe`).
- `vsix/`: packaging project producing `bh_agent_debugger.vsix`.
- `bh_agent_debugger.ps1`: primary automation script for main/exp start/stop + agent commands + queued workflows.
- `build.bat`: convenience full build for Debug|x64.

## Required Environment

- Windows with Visual Studio 2022 (17.x) and its C++ desktop and extension development workloads (v143 toolset, ATL). The extension also runs in Visual Studio 2026 (18.x); builds always use Visual Studio 2022.
- `find_build_vs.cmd` finds that instance through `vswhere` and sets `BH_MSBUILD`, `BH_VCVARS64` and `BH_DIA_SDK_INCLUDE`; set `BH_VS_ROOT` to choose an instance. `build.bat` and the installer scripts call it.
- NuGet package restore is required for debugger SDK packages.
- If running from WSL, shell out to `cmd.exe` for VS/MSBuild commands.

## Restore Commands

Run restore before first build or after deleting `packages/`.

```bat
nuget.exe restore bh_agent_debugger.sln -PackagesDirectory packages
```

If `nuget.exe` is not on `PATH`, fetch the pinned copy (its SHA-256 and Microsoft signature are checked) and run that:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File installer\get_nuget_exe.ps1 -Destination Temp\tools\nuget.exe
Temp\tools\nuget.exe restore bh_agent_debugger.sln -PackagesDirectory packages
```

## Build Commands

Agent rule:
- Always use `build.bat` as the default build command for validation unless the user explicitly requests a narrower project-only build.

### Full solution (default)

```bat
build.bat
```

From this agent CLI (bash-based shell), run batch files through PowerShell/cmd, for example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '.\\build.bat'"
```

Equivalent direct MSBuild (`find_build_vs.cmd` sets `BH_MSBUILD`; the project-only commands below assume it ran):

```bat
call find_build_vs.cmd
"%BH_MSBUILD%" bh_agent_debugger.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /m
```

### Build a single project (closest equivalent to single-test targeting)

Build only engine x64:

```bat
"%BH_MSBUILD%" bh_agent_debugger.sln /t:bh_agent_debugger_engine /p:Configuration=Debug /p:Platform=x64 /m
```

Build only package x64:

```bat
"%BH_MSBUILD%" bh_agent_debugger.sln /t:bh_agent_debugger_package /p:Configuration=Debug /p:Platform=x64 /m
```

Build only VSIX x64:

```bat
"%BH_MSBUILD%" bh_agent_debugger.sln /t:vsix /p:Configuration=Debug /p:Platform=x64 /m
```

Notes:
- `vsix` project also builds engine for `x64` and `Win32` via `BuildEngineBinaries`.
- Outputs are intentionally under `Temp/`.

## MSI Release and Installation (Explicit Request Only)

**Do not update, build, install, repair, or uninstall the MSI unless the user explicitly asks for MSI work.** Routine code changes, builds, and validation must not modify the installed product.

When the user explicitly requests an MSI release/update/install:

1. Confirm the requested version. The MSI installs per user only (`Scope="perUser"`, no administrator rights); there is no per-machine mode.
2. Update the repository-owned agent skill first at `opencode\skills\bh-agent-debugger\SKILL.md` (shared by OpenCode and Claude Code; rules in `docs\skill_maintenance.md`) and the repository tool source when needed, then synchronize Windows OpenCode files:

   ```bat
   opencode\sync_opencode_windows.cmd
   ```

3. Update release metadata:
   - Set `Version` in `installer\Product.wxs` and `bh_agent_version.h` for each release; keep the existing `UpgradeCode` stable. `ProductCode` is intentionally omitted, so WiX generates a new one per build.
   - Set the matching version in `vsix\source.extension.vsixmanifest`.
4. Build the full solution first, then the installer, and stop if either command fails:

   ```bat
   build.bat
   installer\build_installer.bat
   ```

   The MSI output is `Temp\installer\bh_agent_debugger.msi`.
5. Close all Visual Studio instances before any MSI uninstall, install, or repair. Keep Visual Studio closed until the MSI operation finishes. Also stop other processes running from a Visual Studio install folder, such as the idle MSBuild nodes `build.bat` leaves behind: VSIXInstaller refuses to change extensions while they run (exit code 2004), so the extension silently stays at the old version. A per-user install cannot upgrade a legacy per-machine install (0.12.x allowed one), so uninstall that ProductCode first.
6. Run every MSI operation with `/norestart REBOOT=ReallySuppress` and a verbose log. Only uninstalling a legacy per-machine ProductCode needs an **elevated `cmd.exe`**; the per-user install and its upgrades do not. Replace placeholders with the approved release values and repository path:

   ```bat
   rem Only when a legacy per-machine install is registered (elevated cmd.exe):
   msiexec.exe /x "{EXISTING-PRODUCT-CODE}" /qn /L*v "<repo-root>\Temp\installer\uninstall_<version>.log" /norestart REBOOT=ReallySuppress

   msiexec.exe /i "<repo-root>\Temp\installer\bh_agent_debugger.msi" /qn /L*v "<repo-root>\Temp\installer\install_<version>.log" /norestart REBOOT=ReallySuppress
   ```

   For an ordinary upgrade, `installer\update.ps1` (run `-DryRun` first) performs the steps above, removes a legacy per-machine install first (elevating only for that step), and validates the result; see `installer\README.md`.
7. Validate before reporting success:
   - `ProductState=5`, expected version, and `AssignmentType=0` (per user) through `WindowsInstaller.Installer`; obsolete ProductCodes must be unregistered.
   - The verbose log reports installation status `0` and `MainEngineThread is returning 0`, with no `Return value 3` or reboot-required marker.
   - Installed CLI, service, VSIX, OpenCode tool, and skill (including the Claude Code and Codex copies when those hosts are present) are non-empty and SHA-256-identical to repository/build sources; `claude mcp get bh_agent_debugger` and `codex mcp get bh_agent_debugger` show the installed CLI.
   - The normal Visual Studio profile has the expected VSIX version and package plus x64/x86 engine DLLs matching the build outputs.
   - When runtime validation was requested, use the normal debugger-tool workflow and perform only non-destructive command checks.

Report MSI, VSIX, or debugger-agent errors immediately. Never reboot automatically.
## Test / Lint Status

- There is currently **no formal unit-test project** in this repository.
- There is currently **no configured linter/formatter command** in this repository.
- Validation today is build + debug scenario checks.
- In this repo, when asked to "test", prefer CLI/service command chains (or `bh_agent_debugger.ps1` if requested) in addition to `build.bat`.

## Preferred Runtime Workflow (CLI + Service)

Use the split runtime by default.

- Service exe: `Temp\bh_agent_debugger_service_x64_Debug\bh_agent_debugger_service.exe`
- CLI exe: `Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe`

Common commands:

```powershell
# start service (no console window)
Start-Process -FilePath "<repo-root>\Temp\bh_agent_debugger_service_x64_Debug\bh_agent_debugger_service.exe"

# health + discovery
<repo-root>\Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe doctor --timeout-ms 5000
<repo-root>\Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe dte-list --timeout-ms 5000
<repo-root>\Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe targets-list --timeout-ms 5000

# one-call start chain
<repo-root>\Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe start-debugging-chain bh_agent_debugger TargetApp --timeout-ms 15000

# stop by solution hint
<repo-root>\Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe stop-debugging TargetApp --timeout-ms 15000
```

Notes:
- CLI no longer requires `agentctl` prefix.
- CLI assumes one broker pipe (`bh_agent_ctl`) by default.
- `start-debugging`/`stop-debugging` use DTE-first routing and fallback as needed.

Example:

```powershell
.\bh_agent_debugger.ps1 -queue @(
  "start debugging",
  "start exp debugging",
  "set trap symbol target_work",
  "status"
) -solution "<repo-root>\bh_agent_debugger.sln" -startupProject "<repo-root>\vsix\vsix.vcxproj" -configuration Debug -platform x64 -expConfiguration Debug -expPlatform x64 -experimentalSuffix Exp
```

## Single-Test Guidance (Practical Equivalent)

Because there is no test harness, run one narrow scenario:

1. Build only the project you changed (engine/package/vsix).
2. Start main then Exp using `bh_agent_debugger.ps1`.
3. Verify one behavior via Output/trace logs.

Example scenario command:

```powershell
.\bh_agent_debugger.ps1 -queue @("start debugging","start exp debugging") -solution "<repo-root>\bh_agent_debugger.sln" -startupProject "<repo-root>\vsix\vsix.vcxproj" -configuration Debug -platform x64 -expConfiguration Debug -expPlatform x64 -experimentalSuffix Exp
```

## Contract Generation Commands

When `*.vsdconfigxml` changes, regenerate corresponding `*_contract.h`.

Engine remote contract:

```bat
bh_agent_debugger_engine\gen_remote_component_contract.bat
```

Do not hand-edit generated contract files.

## Debugging Workflow Commands

Standard launch flow (recommended):

```powershell
.\bh_agent_debugger.ps1 -message "start debugging" -solution "<repo-root>\bh_agent_debugger.sln" -startupProject "<repo-root>\vsix\vsix.vcxproj" -configuration Debug -platform x64
.\bh_agent_debugger.ps1 -message "start exp debugging" -expConfiguration Debug -expPlatform x64 -experimentalSuffix Exp
```

Agent script message:

```powershell
.\bh_agent_debugger.ps1 -message "status"
```

Useful `bh_agent_debugger.ps1` commands:

- `start debugging` (main instance; requires `-solution`, `-startupProject`, `-configuration`, `-platform`)
- `start exp debugging` (requires running Exp instance; use after `start debugging`)
- `start exp debugging force` (cold-launch Exp if missing)
- `stop main debugging`
- `stop exp debugging`
- `set trap symbol <name>`
- `list threads` (lists native threads for the selected target PID and marks the selected thread)
- `select thread <tid>` (selects a native system thread for stepping, register/stack reads, and `where`)
- `read regs` (reads register snapshot for the selected break-session thread)
- `read stack [bytes]` (reads raw stack memory from the selected thread's current SP)
- `stack [frames]` (DTE stack frame summary when VS is in break mode)
- `wait break <seconds>`
- `where`
- `diag session` (quick DTE/agent/queue diagnostics)
- `preset restart arm <symbol>` (stop exp -> stop main -> start main -> start exp -> set trap symbol -> status)
- `preset health` (diag session + status + queue list in one response)
- `use exp suffix <name>` (switch active experimental RootSuffix for subsequent exp commands)
- `use exp solution <path>` (set active exp solution path for subsequent exp commands)
- `use exp startup <path>` (set active exp startup project path for subsequent exp commands)

Notes:
- `start exp debugging` does **not** cold-launch Exp by default.
- Use `start exp debugging force` or `-launchExpIfMissing` when you explicitly want cold-launch behavior.
- Use distinct suffixes (for example `Exp`, `ExpTarget`) to keep multiple experimental VS instances alive without replacing each other's open solution.

Useful script options:

- `-clean`
- `-waitForBreak`
- `-breakTimeoutSeconds <n>`
- `-configuration Debug|Release`
- `-platform x64`
- `-experimentalSuffix <suffix>`
- `-launchExpIfMissing` (allow `start exp debugging` to cold-launch Exp)
- `-strictDteMatch` (fail if resolved DTE instance does not match expected main/exp role)
- `-diagnostics`

Script behavior notes:

- `bh_agent_debugger.ps1` relaunches itself in STA when required; do not remove this.
- `bh_agent_debugger.ps1` resolves DTE via ProgID + ROT fallback, and uses PID/root-suffix matching for experimental instance attach.
- `bh_agent_debugger.ps1` waits for command readiness (`Debug.Start`/`Debug.StopDebugging`) before execution.
- `-waitForBreak` waits for debugger mode `Break` and prints location details from `BreakpointLastHit` or current stack frame.
- `-clean` runs `Build.CleanSolution` and waits until build state is no longer `InProgress`.
- Both main and experimental DTE solution configurations can be set with `-configuration` and `-platform`; script requires both when either is provided.

Agent messaging notes:

- `bh_agent_debugger.ps1` talks to pipe `bh_agent_debugger_agent` (default) using duplex read/write and returns one-line responses.
- `bh_agent_debugger.ps1` supports `prime trap symbol <name>` helper: it sends `step over`, waits briefly, then sends `set trap symbol <name>`.
- `bh_agent_debugger.ps1` supports queue chaining via `-queue @("cmd1","cmd2",...)` and wait commands like `wait status ...` / `wait break ...`.
- `diag session` includes capability-style fields (`canStatus`, `canQueue`, `canSetTrap`, `canReadMem`, `pendingCount`) and DTE identity fields (`mainRoot`, `expRoot`, `mainSolution`, `expSolution`) to quickly verify routing and command readiness.

Build troubleshooting:

- If build hits linker/PDB write errors (for example `LNK1201` writing engine `.pdb`), first stop Exp instance locks with `./bh_agent_debugger.ps1 -message "stop debugging force" -experimentalSuffix Exp`, then retry `build.bat`.
- If build hits linker write errors for service/package outputs (for example `LNK1168`/`LNK1201`), ensure lock holders are stopped:
  - `taskkill /F /IM bh_agent_debugger_service.exe`
  - close Experimental `devenv.exe` instances when VSIX/package DLLs are locked.

## Logging

- Unified runtime log path:
  - `%LOCALAPPDATA%\bh_agent_debugger\bh_agent_debugger.log`
- Components log with tags like `[cli]`, `[service]`, `[package.pipe]`, `[engine.local]`.
- Prefer this log first when diagnosing routing/start/stop issues.

## Coding Style Guidelines

### General C++ style

- Follow existing file/function naming with `bh_agent_*` prefix.
- Prefer `snake_case` for files, functions, local variables, and members.
- Keep class names lower snake case as established in this repo.
- Use braces on new lines (Allman-style) consistent with current code.
- Keep one responsibility per function when possible.

### Includes and dependencies

- Include order: platform/system headers, ATL/VS headers, then local headers.
- Use project-local includes with quotes (`"..."`).
- Avoid introducing unnecessary new third-party dependencies.

### COM / ATL / DKM conventions

- Use `STDMETHODCALLTYPE`/`HRESULT` signatures for COM interfaces.
- Return `HRESULT` and validate pointers early (`E_POINTER` / `E_FAIL` as needed).
- Use `SUCCEEDED(hr)` / `FAILED(hr)` checks.
- Use `CComPtr<T>` for COM ownership; avoid raw owning pointers.
- Keep `DECLARE_NO_REGISTRY`, `DECLARE_NOT_AGGREGATABLE`, `OBJECT_ENTRY_AUTO` patterns.

### Error handling and logging

- Prefer explicit defensive checks at function start.
- Do not throw C++ exceptions across COM boundaries.
- Use `OutputDebugStringA/W` for debug traces in this codebase.
- Use concise, stable log messages suitable for troubleshooting.

### Unused parameters / warnings

- Mark intentionally unused params with `(void)param;` or `UNREFERENCED_PARAMETER`.
- Keep warning levels and project compiler settings unchanged unless required.

### GUIDs and message IDs

- Keep service/component GUIDs synchronized across:
  - `.vsdconfigxml`
  - `guids.h`
  - `.pkgdef`
  - any hardcoded GUID constants used for messaging
- Centralize message code enums/constants where possible.

### Generated and manifest files

- Treat `*_contract.h` as generated artifacts.
- Update `source.extension.vsixmanifest` assets when adding/removing engine/package outputs.
- Keep `VSIXSourceItems.props` aligned with produced binaries.

### PowerShell script conventions

- Preserve STA relaunch behavior in `bh_agent_debugger.ps1`.
- Preserve COM retry behavior for `RPC_E_CALL_REJECTED` patterns.
- Keep diagnostics gated behind the `-diagnostics` switch.

## Agent Working Rules

- Make minimal, scoped edits; avoid broad refactors unless requested.
- Do not rename projects/paths casually; many build scripts assume exact names.
- Validate with the narrowest build command first, then full build if needed.
- If touching engine messaging, verify both:
  - engine compile for `x64` and `Win32`
  - VSIX payload includes expected files

## Cursor / Copilot Rule Files

- `.cursor/rules/`: not present.
- `.cursorrules`: not present.
- `.github/copilot-instructions.md`: not present.

If these files are added later, update this AGENTS.md to incorporate them.
