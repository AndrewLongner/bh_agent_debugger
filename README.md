# BH Agent Debugger

BH Agent Debugger lets AI coding agents and scripts drive the Visual Studio debugger for native
C and C++ programs: start, attach and stop debugging, set breakpoints, step, inspect threads,
registers, stacks, memory and locals, evaluate expressions, open crash dumps and profile CPU usage.
It works through a command-line client, an [MCP](https://modelcontextprotocol.io) server and an
OpenCode tool, and supports Visual Studio 2022 and Visual Studio 2026.

> **Security:** a connected agent can run code as you. Starting or attaching a debugger, launching
> with arguments and evaluating expressions all run with your account's rights. Connect only agents
> you trust and keep their approval prompts on. See [SECURITY.md](SECURITY.md).

## Features

- **Debug control:** start debugging by solution hint (optionally with one-shot launch arguments),
  attach to a PID, stop, break, continue, step into/over/out, and wait for a break.
- **Breakpoints and traps:** `set bp <file>:<line>`, list/remove/clear breakpoints, and address or
  symbol traps.
- **Inspection:** status, call stack (`where`), thread list and selection, registers, raw stack,
  memory (`hex`, `ascii`, `u32`, `u64`, `ptr`), locals, and modules.
- **Expression evaluation:** side effects and function calls are blocked unless
  `eval --side-effects` is requested explicitly.
- **Crash dumps:** open native x86/x64 user-mode `.dmp` files read-only and inspect them with the
  same commands.
- **CPU profiling:** capture with Visual Studio's CPU Usage tool, then analyze the capture offline
  into JSON (functions by self/inclusive time, call tree, hot paths, source-line drill-down) using
  local PDBs only.
- **Several Visual Studio instances:** every running instance with the extension is a separate target
  (`vs_<pid>`); commands name their target explicitly.
- **Command chains:** several commands in one request, for example
  `set bp main.cpp:42; continue; poll break 15000 200; where 5`.

## Requirements

- Windows 10 or 11, x64.
- Visual Studio 2022 (17.x) or Visual Studio 2026 (18.x), x64, Community, Professional or Enterprise,
  with the C++ desktop development workload.
- Native C or C++ debuggees (x64 or x86).
- CPU profiling uses Visual Studio's profiling tools.

## How it works

```text
agent or script
  |  CLI, MCP server (bh_agent_debugger_cli mcp) or OpenCode tool
  v
bh_agent_debugger_cli
  |  local named pipe (bh_agent_ctl)
  v
bh_agent_debugger_service      broker: routes requests, Visual Studio automation, profiling
  |  local named pipe per Visual Studio instance
  v
Visual Studio package + Concord debug engine components (one target per devenv.exe)
```

All pipes are local and restricted to your user account; nothing listens on the network.

## Install

1. Close every Visual Studio instance.
2. Run `bh_agent_debugger.msi` (from the
   [latest GitHub release](https://github.com/AndrewLongner/bh_agent_debugger/releases/latest), or build
   it: see [installer/README.md](installer/README.md)). It installs per user and does not need administrator
   rights.
3. Choose features:
   - **Visual Studio 2022/2026 extension**: installed into each supported Visual Studio instance.
   - **Add CLI to PATH**.
   - **OpenCode tool/skill** for Windows and/or WSL.
   - **Claude Code skill and MCP server**.
   - **Codex skill and MCP server** for Windows and/or WSL.

   Agent host features are skipped when that host is not installed.
4. Start Visual Studio, open your solution, and check the setup:

   ```bat
   bh_agent_debugger_cli doctor
   ```

Files go to `%LOCALAPPDATA%\Programs\BH Agent Debugger`. Silent installs, feature ids and updates are
described in [installer/README.md](installer/README.md).

## Quick start (CLI)

```bat
rem Health check, Visual Studio targets and their solutions
bh_agent_debugger_cli doctor
bh_agent_debugger_cli targets-list
bh_agent_debugger_cli dte-list

rem Start debugging the solution named MyApp (name or part of the solution path)
bh_agent_debugger_cli start-debugging MyApp --timeout-ms 15000

rem Send commands to the target listed by targets-list
bh_agent_debugger_cli send-target vs_12345 "set bp C:\src\MyApp\main.cpp:42"
bh_agent_debugger_cli send-target vs_12345 "continue; poll break 15000 200; where 5"
bh_agent_debugger_cli send-target vs_12345 "locals"
bh_agent_debugger_cli send-target vs_12345 "eval count * 2"

bh_agent_debugger_cli stop-debugging MyApp
```

- `bh_agent_debugger_cli` with no arguments prints the CLI commands.
- `send-target <id> "help"` lists the target commands; `help <command>` shows one command's syntax.
- The CLI starts the broker service on demand (`--no-auto-start-service` turns that off).
- Logs: `%LOCALAPPDATA%\bh_agent_debugger\bh_agent_debugger.log`.

## Using it from an AI agent

### MCP

Add a stdio server that runs `bh_agent_debugger_cli.exe mcp`:

```json
{
  "mcpServers": {
    "bh_agent_debugger": { "command": "bh_agent_debugger_cli.exe", "args": ["mcp"] }
  }
}
```

The server exposes twelve tools (`bh_agent_debugger_doctor`, `_targets`, `_dte`, `_start`, `_send`, ...).
Client examples, tool list and protocol details: [docs/mcp.md](docs/mcp.md).

### OpenCode

The MSI's OpenCode features install the tool and skill. From a source checkout, run
`opencode\sync_opencode_windows.cmd` to copy `opencode\tools\bh_agent_debugger.ts` and
`opencode\skills\bh-agent-debugger\SKILL.md` into `%USERPROFILE%\.config\opencode`. The tool runs
`bh_agent_debugger_cli.exe` from `PATH`, or the path in `BH_AGENT_CLI_EXE`.

### Claude Code

The MSI's Claude Code feature (on by default) copies the skill into Claude Code and registers the MCP
server for your account with `claude mcp add --scope user`. It does nothing when Claude Code is not
installed. To set it up by hand, run

```bat
claude mcp add --scope user bh_agent_debugger -- bh_agent_debugger_cli.exe mcp
```

and copy `opencode\skills\bh-agent-debugger\SKILL.md` to
`%USERPROFILE%\.claude\skills\bh-agent-debugger\SKILL.md`.

### Codex

The MSI's Codex features (on by default) copy the skill to the user skill folder that Codex reads and
register the MCP server with `codex mcp add`, for Codex on Windows and for Codex in the default WSL
distribution. They do nothing where Codex is not installed. To set it up by hand, run Codex's own command
with the full path to the installed CLI (the Codex CLI, the IDE extension and the ChatGPT desktop app
share the configuration):

```bat
codex mcp add bh_agent_debugger -- "%LOCALAPPDATA%\Programs\BH Agent Debugger\bin\bh_agent_debugger_cli.exe" mcp
```

and copy `opencode\skills\bh-agent-debugger\SKILL.md` to
`%USERPROFILE%\.agents\skills\bh-agent-debugger\SKILL.md`. Codex in WSL works the same way with the
CLI's `/mnt/c/...` path and `~/.agents/skills`; see [docs/mcp.md](docs/mcp.md#codex).

OpenCode, Claude Code and Codex use the same skill ([SKILL.md](opencode/skills/bh-agent-debugger/SKILL.md)),
which is also the most complete reference for agent workflows and command behavior.

## Building from source

Prerequisites:

- Visual Studio 2022 (17.x) with the C++ desktop workload (v143 toolset, ATL), the Visual Studio
  extension development workload and a Windows SDK. Builds use Visual Studio 2022 even when the
  extension runs in Visual Studio 2026.
- The .NET 10 SDK, for the offline profile analyzer.
- `nuget.exe` on `PATH` for the package restore (`installer\get_nuget_exe.ps1 -Destination <path>`
  fetches a pinned, signature-checked copy).

```bat
nuget.exe restore bh_agent_debugger.sln -PackagesDirectory packages
build.bat
```

`build.bat [target] [configuration] [platform]` defaults to `Build Debug x64`. Outputs go under
`Temp\`, for example `Temp\vsix_x64_Debug\bh_agent_debugger.vsix` and
`Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe`. `find_build_vs.cmd` selects a
Visual Studio 2022 instance with the C++ tools through `vswhere`; set `BH_VS_ROOT` to choose one.

The MSI is built with WiX v6.0.2 compiled from source: see [installer/README.md](installer/README.md).

### Testing

There is no unit-test framework; tests are scripts under `tests\`:

- `tests\test_mcp_server.ps1`: MCP JSON-RPC handshake, tool schemas and argument validation.
- `tests\test_debug_launch_cli.ps1`: launch and attach request generation.
- `tests\test_dump_support.bat`, `tests\test_vs_profiler.bat`, `tests\test_profile_dia.bat`,
  `tests\test_cpu_profiler.bat`: native unit tests.
- `tests\test_profile_cli.ps1`, `tests\test_profile_analysis.ps1`: profiling CLI and offline analysis.

Test extension changes in a Visual Studio Experimental instance (`devenv /rootsuffix Exp`), never in
your normal Visual Studio profile.

## Repository layout

| Path | Contents |
|---|---|
| `bh_agent_debugger_package/` | Visual Studio package: per-instance pipe server, command handling, DTE |
| `bh_agent_debugger_engine/` | Concord debug engine components (x64 and x86) |
| `bh_agent_debugger_service/` | Broker service (`bh_agent_debugger_service.exe`) |
| `bh_agent_debugger_cli/` | CLI and MCP server (`bh_agent_debugger_cli.exe`) |
| `bh_profile_analyzer/` | Offline CPU profile analyzer (.NET 10) and DIA helper |
| `vsix/` | VSIX packaging |
| `installer/` | WiX MSI, custom actions and installer tests |
| `opencode/` | OpenCode tool and skill |
| `docs/` | MCP server documentation |
| `tests/` | Test scripts |

Profile analyzer details: [bh_profile_analyzer/README.md](bh_profile_analyzer/README.md). The original
service and CLI design notes ([SERVICE_CLI_ARCHITECTURE.md](SERVICE_CLI_ARCHITECTURE.md),
[SERVICE_CLI_PROTOCOL.md](SERVICE_CLI_PROTOCOL.md)) are historical; some names have changed since.

## License

MIT License, copyright (c) 2026 Boiling Lava Hot LLC. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
