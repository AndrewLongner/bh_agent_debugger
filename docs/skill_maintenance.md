# Maintaining the agent skill and tool definitions

`opencode/skills/bh-agent-debugger/SKILL.md` is the one agent skill. It is installed:

- into OpenCode on Windows and WSL (MSI features `OpenCodeWindowsFeature` and `OpenCodeWslFeature`, or
  `opencode\sync_opencode_windows.cmd`, `opencode/sync_opencode_wsl.sh` and
  `opencode/sync_bh_agent_debugger.sh` from a checkout);
- into Claude Code (MSI feature `ClaudeCodeFeature` copies it to
  `<Claude config folder>\skills\bh-agent-debugger\SKILL.md`);
- into Codex on Windows and in WSL (MSI features `CodexFeature` and `CodexWslFeature` copy it to
  `.agents\skills\bh-agent-debugger\SKILL.md` in the user's home folder);
- into any other agent host that reads [Agent Skills](https://agentskills.io).

## Rules for the skill

- Write it for someone using an installed release: what to call, in which order, what the output
  means and what to refuse. Build steps, test commands, implementation details and version history
  belong in this file instead.
- Use the tool and argument names of the tool definitions; they are the same in every host.
- Update the version line ("This skill matches BH Agent Debugger x.y.z") for every release.
- The `description` in the front matter decides when a host loads the skill. Keep it specific and under
  1024 characters.

## Changing a tool

The tool definitions exist in three places that must match:

- `opencode/tools/bh_agent_debugger.ts`, the OpenCode wrapper;
- `bh_agent_debugger_cli/bh_agent_debugger_cli/bh_mcp_server.h`, the MCP server.
  `tests\test_mcp_server.ps1` checks that its `tools/list` mirrors the wrapper;
- any agent host that registers the tools natively instead of through MCP. Such a host must forward every
  argument itself, for example `output` for `bh_agent_debugger_profile` and `function`, `startMs`,
  `endMs`, `threadId`, `subtree` and `sections` for `bh_agent_debugger_profile_analyze`.

Then update the skill's tool table and [mcp.md](mcp.md).

Editing the repository copies does not change installed tools or skills: the MSI or the sync scripts
do.

## Feature history

Useful when a user runs older installed binaries (`doctor` shows the versions):

| Version | Added |
|---|---|
| 0.11.8 | Crash dumps (`open_dump`, `open dump`), in-band `help` |
| 0.12.1 | CPU profiling (`profile`) and offline analysis (`profile analyze`); the MSI includes the self-contained analyzer |
| 0.12.2 | Source drill-down (`function`) with the native DIA helper; analysis filters (`startMs`, `endMs`, `threadId`, `subtree`) |
| 0.12.3 | Caller-selected capture output (`output`) |
| 0.12.4 | `doctor` versions and per-target `vsix=`; compact subtree output; `sections` |
| 0.13.0 | MCP server (`bh_agent_debugger_cli mcp`); traps stop on the thread that hit them; per-user installer |
| 0.13.1 | The MSI installs the skill into Claude Code and registers the MCP server |
| 0.13.2 | A failed broker reply (`"ok": false`) is a tool error carrying its message; a closed Visual Studio leaves the target list at once |
| 0.13.3 | The MSI installs the skill into Codex on Windows and in WSL and registers the MCP server |
| 1.0.0 | First public release; the MSI skips the Windows Codex MCP registration when `codex --version` fails |
| 1.0.1 | `continue` returns once Visual Studio reports run mode, so a `break` right after it works |

## Profiling internals

- Capture runs `VSDiagnostics.exe` with the CPU Usage agent of the newest Visual Studio 2022 or 2026,
  found through `vswhere`, never `PATH`. The custom ETW collector tested by `test_cpu_profiler` is an
  inactive experimental draft.
- Analysis runs in a separate .NET 10 helper (`bh_profile_analyzer`) with a pinned Microsoft
  TraceEvent, built beside the CLI by `build.bat`. The MSI ships it self-contained (single file with the
  .NET runtime); development builds are framework-dependent and need the .NET 10 runtime.
- Source drill-down loads the native `bh_profile_dia.dll` only inside the analyzer, with the bundled DIA
  reader and local PDB GUID/age checks. Limits and the output schema: `bh_profile_analyzer/README.md`.
- The CLI's global `--pipe <name>` selects an isolated development broker.

## Validation

- Profiling: `build.bat`, `tests\test_vs_profiler.bat`, `tests\test_profile_cli.ps1`;
  `Temp\tests\test_vs_profiler.exe --live` records a short self-profile.
- Analysis: `tests\test_profile_analysis.ps1`; pass `-InputFile`, `-RecordedPid` and optionally
  `-Symbols`, `-ExpectedSamples`, `-Function` and `-ExpectedFunctionSamples` to check an archived
  capture without starting a target. `tests\test_profile_dia.bat` covers optimized nested inline
  locations and PDB mismatches.
- Crash dumps: `build.bat`, then `tests\test_dump_support.bat`. Open a real dump in an Experimental
  Visual Studio instance started from the main instance; don't install the MSI or the normal-profile
  extension for this.
- MCP server: `tests\test_mcp_server.ps1`, plus `-Live` against a running broker.
