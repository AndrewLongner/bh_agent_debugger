# BH Agent Debugger

BH Agent Debugger lets AI coding agents and scripts drive the Visual Studio debugger for native C and
C++ programs. It works with Visual Studio 2022 and Visual Studio 2026.

**Use the Download button on this page to get the installer** (`bh_agent_debugger.msi`), or get it
from the [GitHub release](https://github.com/AndrewLongner/bh_agent_debugger/releases/latest).

The extension needs the broker service and command-line client that come with the installer, so it is
installed with the MSI rather than from Visual Studio's Extension Manager.

## What an agent can do

- **Debug control:** start debugging a solution (optionally with launch arguments), attach to a
  process, stop, break, continue, step into, over and out, and wait for a break.
- **Breakpoints and traps:** file and line breakpoints, and address or symbol traps.
- **Inspection:** call stack, threads, registers, raw stack, memory, locals and modules.
- **Expression evaluation:** side effects and function calls are blocked unless the agent asks for
  them explicitly.
- **Crash dumps:** open native x86 and x64 user-mode dumps read-only and inspect them with the same
  commands.
- **CPU profiling:** capture with Visual Studio's CPU Usage tool and analyze the capture offline:
  hot functions, call tree, hot paths and source-line drill-down, using local PDBs only.
- **Several Visual Studio instances:** each running instance is a separate target.

## Agents and tools

- An [MCP](https://modelcontextprotocol.io) server (`bh_agent_debugger_cli mcp`) for any MCP client.
- The installer can set up Claude Code, Codex (Windows and WSL) and OpenCode for you: it adds the
  skill and registers the MCP server. Hosts that are not installed are skipped.
- A command-line client, `bh_agent_debugger_cli`, for scripts.

## Install

1. Close every Visual Studio instance.
2. Download `bh_agent_debugger.msi` from this page or from the
   [latest release](https://github.com/AndrewLongner/bh_agent_debugger/releases/latest) and run it.
   It installs per user and does not need administrator rights. The MSI is not code-signed, so
   SmartScreen may warn; the GitHub release lists its SHA-256.
3. Start Visual Studio and run `bh_agent_debugger_cli doctor`.

## Requirements

- Windows 10 or 11, x64.
- Visual Studio 2022 (17.x) or 2026 (18.x), Community, Professional or Enterprise, with the C++
  desktop development workload.
- Native x64 or x86 debuggees.

## Security

A connected agent can run code with your account's rights: debugging, launching programs and
evaluating expressions all do. All pipes are local and restricted to your user account; nothing
listens on the network. Connect only agents you trust and keep their approval prompts on. See
[SECURITY.md](https://github.com/AndrewLongner/bh_agent_debugger/blob/main/SECURITY.md).

## Source and support

Open source under the MIT license:
[github.com/AndrewLongner/bh_agent_debugger](https://github.com/AndrewLongner/bh_agent_debugger).
Report bugs in [GitHub issues](https://github.com/AndrewLongner/bh_agent_debugger/issues) and
vulnerabilities privately through the repository's Security tab.
