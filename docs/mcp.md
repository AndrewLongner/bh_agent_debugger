# MCP server

`bh_agent_debugger_cli mcp` runs BH Agent Debugger as a [Model Context Protocol](https://modelcontextprotocol.io)
server over stdio. Any MCP client that can launch a local stdio server (for example Claude Code,
Claude Desktop, Codex, VS Code or Cursor) gets the same twelve debugger tools as the OpenCode wrapper in
`opencode/tools/bh_agent_debugger.ts`.

> **Security:** these tools drive Visual Studio debugging as the signed-in user. Starting or
> attaching a debugger, setting breakpoints and evaluating expressions can run code in the
> debuggee and in Visual Studio. Connect only agents you trust, and keep your MCP client's
> per-tool approval prompts turned on for the tools that are not read-only.

## Requirements

- A CLI built with the `mcp_stdio` feature. Run `bh_agent_debugger_cli --version`; the
  `features=` list must include `mcp_stdio`.
- The BH Agent Debugger extension in Visual Studio 2022 (17.x) or Visual Studio 2026 (18.x), and a
  running Visual Studio instance for every tool except `bh_agent_debugger_profile_analyze`.
- The broker service. The CLI starts it on demand unless `--no-auto-start-service` is passed.

The MSI installs the CLI to `%LOCALAPPDATA%\Programs\BH Agent Debugger\bin` and adds that folder to
the user `PATH`. If your client does not inherit `PATH`, use the full path to
`bh_agent_debugger_cli.exe` as the command (in JSON, write each backslash twice).

## Client configuration

Most clients accept an `mcpServers` entry (Claude Desktop, Cursor, and a Claude Code `.mcp.json`):

```json
{
  "mcpServers": {
    "bh_agent_debugger": {
      "command": "bh_agent_debugger_cli.exe",
      "args": ["mcp"]
    }
  }
}
```

VS Code (`.vscode/mcp.json`):

```json
{
  "servers": {
    "bh_agent_debugger": {
      "type": "stdio",
      "command": "bh_agent_debugger_cli.exe",
      "args": ["mcp"]
    }
  }
}
```

Claude Code: the MSI's Claude Code feature registers the server for your account (with the full path to
the installed CLI) and installs the skill. To register it by hand:

```bat
claude mcp add --scope user bh_agent_debugger -- bh_agent_debugger_cli.exe mcp
```

Without `--scope user`, Claude Code adds the server only for the current project folder.

### Codex

The MSI's Codex features register the server and install the skill for Codex on Windows and for Codex in
the default WSL distribution. They do nothing where Codex is not installed, and they keep a
`bh_agent_debugger` server that you added yourself.

The Codex CLI, the IDE extension and the ChatGPT desktop app share one configuration
(`~/.codex/config.toml`, or `config.toml` in `CODEX_HOME`). To register the server by hand, use Codex's
own command and the full path to the CLI:

```bat
codex mcp add bh_agent_debugger -- "%LOCALAPPDATA%\Programs\BH Agent Debugger\bin\bh_agent_debugger_cli.exe" mcp
```

Codex in WSL starts the same Windows CLI through WSL interop, so debugging still happens in Visual Studio
on Windows. Use the CLI's `/mnt/c/...` path:

```bash
codex mcp add bh_agent_debugger -- "/mnt/c/Users/<you>/AppData/Local/Programs/BH Agent Debugger/bin/bh_agent_debugger_cli.exe" mcp
```

- The command writes a `[mcp_servers.bh_agent_debugger]` table with `command` and `args = ["mcp"]`.
  `codex mcp list` shows it and `codex mcp remove bh_agent_debugger` removes it.
- `codex mcp add` replaces an existing table of that name, including keys you added to it by hand. The
  MSI features therefore leave a registration alone when it already starts the installed CLI.
- Codex ends a tool call after 60 seconds by default. For longer calls, such as a 60-second
  `bh_agent_debugger_profile` capture, add `tool_timeout_sec = 180` to the table.
- Without the MSI features, copy the skill to
  `%USERPROFILE%\.agents\skills\bh-agent-debugger\SKILL.md` (in WSL,
  `~/.agents/skills/bh-agent-debugger/SKILL.md`). It is `opencode\skills\bh-agent-debugger\SKILL.md` in
  a checkout and under `%LOCALAPPDATA%\Programs\BH Agent Debugger` after an MSI install.

Registration and tool discovery were checked with Codex CLI 0.167.0 on Windows and in WSL (Ubuntu 22.04).

### Server options

| Option | Effect |
|---|---|
| `--pipe <name>` | Broker pipe name passed to every broker tool (default `bh_agent_ctl`). Letters, digits, `.`, `_` and `-` only. |
| `--no-auto-start-service` | Never start the broker service; tools fail if it is not running. |

For example `"args": ["mcp", "--no-auto-start-service"]`.

## Tools

The tool names, descriptions and argument schemas match the OpenCode wrapper. Every schema is a
closed object (`additionalProperties: false`); unknown arguments are rejected.

| Tool | Read-only | Purpose |
|---|---|---|
| `bh_agent_debugger_doctor` | yes | Health check: service, CLI and extension versions |
| `bh_agent_debugger_targets` | yes | Visual Studio instances running the extension (`vs_<pid>` target ids) |
| `bh_agent_debugger_dte` | yes | Visual Studio automation instances and their solutions |
| `bh_agent_debugger_modules` | yes | Native modules loaded in a target's debuggee |
| `bh_agent_debugger_profile` | no | Asynchronous CPU Usage capture (`start`, `stop`, `status`, `report`, `open`) |
| `bh_agent_debugger_profile_analyze` | yes | Offline analysis of a saved ETL or diagsession; no broker or Visual Studio |
| `bh_agent_debugger_open_dump` | no | Open a native crash dump in an idle Visual Studio target |
| `bh_agent_debugger_start` | no | Start debugging by solution hint, optionally with launch arguments |
| `bh_agent_debugger_attach` | no | Attach to a process by PID |
| `bh_agent_debugger_stop` | no | Stop debugging by solution hint |
| `bh_agent_debugger_chain` | no | Start debugging several solutions in order |
| `bh_agent_debugger_send` | no | Send a target command (`status`, `break`, `continue`, breakpoints, traps, locals, eval, ...) |

Read-only tools carry the MCP `readOnlyHint` annotation. Refresh `bh_agent_debugger_targets` and
`bh_agent_debugger_dte` before the first `bh_agent_debugger_send`, and use target ids exactly as
listed. `bh_agent_debugger_send` with `command` set to `help` lists the target commands.

Expression evaluation blocks side effects by default. The `--side-effects` opt-in is part of the
command text, so your client's approval prompt shows it.

## Behavior

- Each `tools/call` runs `bh_agent_debugger_cli.exe` (the same executable) as a child process with the
  arguments the OpenCode wrapper would use, and returns its output as one text content item: the
  broker reply's `result.text` when there is one, otherwise the raw output.
  A broker reply with `"ok": false` is returned with `isError: true` and its `error.message`. A
  non-zero exit code, a launch failure or a timeout is also returned with `isError: true`; invalid
  arguments are rejected before anything runs, also as `isError: true`.
- Requests are handled one at a time, in order.
- A call waits for the tool's own `timeoutMs` (multiplied by the number of hints for
  `bh_agent_debugger_chain`) plus 30 seconds, then stops the child process. Profile analysis waits
  up to 150 seconds.
- The child's stdin is the NUL device and it inherits only its standard handles. A broker service
  it starts keeps running after the call.
- Output is capped at 4 MiB per call.
- Text arguments must not contain control characters, and positional values (hints, commands, dump
  paths) must not start with `-`. Text that the CLI reads from its narrow command line (everything
  except the `bh_agent_debugger_profile_analyze` arguments) must be representable exactly in the
  system ANSI code page. Anything else is rejected rather than converted.
- Activity is logged with the `cli.mcp` tag in `%LOCALAPPDATA%\bh_agent_debugger\bh_agent_debugger.log`.

## Protocol

- Transport: stdio, one JSON-RPC 2.0 message per line (UTF-8, at most 4 MiB). A leading UTF-8 BOM
  on the first line is ignored. The server exits with code 0 when stdin closes.
- Protocol versions: `2025-11-25`, `2025-06-18`, `2025-03-26` and `2024-11-05`. A supported requested
  version is echoed; otherwise the server answers with `2025-11-25`.
- Methods: `initialize`, `ping`, `tools/list` and `tools/call`. `tools/list` and `tools/call` before
  `initialize` return `-32002`; a second `initialize` is rejected. Notifications get no reply.
  Other methods (resources, prompts, ...) return `-32601`.
- JSON is parsed strictly: invalid UTF-8, lone surrogates, duplicate member names and nesting
  deeper than 64 levels are parse errors (`-32700`); batches are rejected with `-32600`.

## Testing

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_mcp_server.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_mcp_server.ps1 -Live
```

The default run checks the handshake, that `tools/list` mirrors the OpenCode wrapper, JSON-RPC
error handling and argument validation, and makes one offline tool call. `-Live` adds read-only
`doctor`, `targets` and `dte` calls against a running broker. `-CliPath` selects another CLI build
(the default is `Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe`).
