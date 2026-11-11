---
name: bh-agent-debugger
description: Drive the Visual Studio 2022/2026 debugger with BH Agent Debugger (bh_agent_debugger tools, its MCP server or bh_agent_debugger_cli) to start, attach and stop debugging, set breakpoints and instruction traps, step, inspect threads, locals, expressions, registers and memory, open native crash dumps, and capture or analyze CPU profiles. Use when the user asks to debug, inspect, profile or open a crash dump of a Windows program in Visual Studio, or when bh_agent_debugger tools are available.
---

# BH Agent Debugger

This skill matches BH Agent Debugger **1.0.1**. BH Agent Debugger lets an agent drive Visual Studio 2022
(17.x) or 2026 (18.x) instances that have the BH Agent Debugger extension, through a local broker
service.

## Tools

The same twelve tools exist in every agent host:

- OpenCode: from the `bh_agent_debugger` tool file.
- Claude Code and other MCP clients: from the `bh_agent_debugger` MCP server
  (`bh_agent_debugger_cli mcp`). A host may show a server prefix, for example
  `mcp__bh_agent_debugger__bh_agent_debugger_send` in Claude Code.
- Without the tools, run `bh_agent_debugger_cli` from a shell; see [CLI equivalents](#cli-equivalents).

| Tool | Use |
|---|---|
| `bh_agent_debugger_doctor(timeoutMs?)` | Health check with service, CLI and extension versions |
| `bh_agent_debugger_targets(timeoutMs?)` | Visual Studio instances running the extension, as `vs_<pid>` target ids |
| `bh_agent_debugger_dte(timeoutMs?)` | Visual Studio automation instances and their open solutions |
| `bh_agent_debugger_modules(targetId, timeoutMs?)` | Native modules of a target's debuggee |
| `bh_agent_debugger_start(hint, arguments?, timeoutMs?)` | Start debugging the solution matching `hint` |
| `bh_agent_debugger_attach(hint, processId, timeoutMs?)` | Attach that Visual Studio to a local process |
| `bh_agent_debugger_stop(hint, timeoutMs?)` | Stop debugging the solution matching `hint` |
| `bh_agent_debugger_chain(hints[], timeoutMs?)` | Start debugging several solutions in order |
| `bh_agent_debugger_send(targetId, command, timeoutMs?)` | Send a debugger command to a target (see [Commands](#commands)) |
| `bh_agent_debugger_open_dump(targetId, path, timeoutMs?)` | Open a native crash dump |
| `bh_agent_debugger_profile(action, processId?, seconds?, targetId?, output?, timeoutMs?)` | CPU Usage capture |
| `bh_agent_debugger_profile_analyze(input, processId, top?, symbols?, function?, startMs?, endMs?, threadId?, subtree?, sections?)` | Offline analysis of a saved capture |

A hint is a solution name or part of the solution path. The broker service starts on demand for the
debugging tools; profiling never starts it.

## Safety

- The tools act with the user's rights. Starting or attaching a debugger, launching with arguments and
  `eval --side-effects` run code as the user.
- Don't stop, restart or take over a debugging session you didn't start unless the user asks. Opening a
  dump or a profiling report refuses an active session instead of stopping it.
- Use `eval --side-effects` only when the user asks for that effect.
- `clear bp` and `clear all bp` remove every Visual Studio breakpoint, including the user's. Use them only
  when asked; remove your own breakpoints with `remove bp <index>`.

## Versions

- `doctor` starts with `versions service=<v> cli=<v> features=<flags>`, and each target shows `vsix=<v>`
  (`unreported` means an extension older than 0.12.4). `bh_agent_debugger_cli --version` works without
  the broker.
- If the versions differ, or a command returns `error: unknown command`, the installed pieces are out
  of step. Tell the user instead of working around it.

## Standard workflow

1. Run `bh_agent_debugger_doctor`, `bh_agent_debugger_targets` and `bh_agent_debugger_dte`.
2. Start debugging with `bh_agent_debugger_start`, or attach with `bh_agent_debugger_attach`.
3. Refresh targets and DTE, pick the `vs_<pid>` target of the intended solution, and send commands with
   `bh_agent_debugger_send`.
4. Report exception-interrupt responses immediately.
5. When done, stop the session you started with `bh_agent_debugger_stop`.

Starting:

- `bh_agent_debugger_start` returns the matching targets entry, for example
  `debug.startHint ok ... | #2 id='vs_26764' pipe='bh_agent_debugger_agent_vs_26764' ageSec=0`.
- `arguments` sets the command line for this launch only; it does not change the startup project's
  properties or `.vcxproj.user`. Starting with `arguments` while already in run or break mode returns
  `already_debugging`.

## Target routing rules (required)

- Refresh both `bh_agent_debugger_targets` and `bh_agent_debugger_dte` immediately before the first
  `send` in a run, and again after any `bh_agent_debugger_start` or `bh_agent_debugger_stop`.
- If a `send` returns `target_not_found`, take the target id from fresh `targets` output and retry once.
  Never keep using a cached `vs_<pid>` after `target_not_found`.
- Use a registered `vs_<pid>` target for debugger commands; there is no default target.
- If several Visual Studio instances are open, match the target by solution path and root suffix before
  sending commands.

## Symbol and trap preflight (required)

- Before `resolve symbol`, `set trap symbol` or `trap add symbol`, run on the selected target:
  1. `status`
  2. `modules`
  3. `set target pid <pid>` with the debuggee PID from `modules` or `status`.
- If `modules` shows `devenv.exe` as `#0`, the process context is wrong: set the target PID explicitly
  before symbol commands.
- If symbol resolution fails, report the exact symbol and the current target PID.

## Status model

- Running: `ok: running (no active break session)`, optionally with the target PID.
- Paused: `ok: paused break pid=<pid> tid=<tid> ip=0x... reason=<kind>`, where `reason` is `async_break`,
  `step`, `trap` or `break`.
- Running status does not include pending exception text.

## Continue, break and step

- `async break` and `break` are aliases. In break mode they return `ok: already break`; in run mode they
  break, and `status` then shows `reason=async_break`.
- `continue` in break mode continues and returns once Visual Studio reports run mode, so a `break`
  right after it works. Success is `ok: executed continue | running`, `| break hit | stable break` (it
  stopped again during the wait, for example at a breakpoint hit at once) or `| debugging ended`. In run mode it is a no-op (`ok: already running`), and outside
  run and break mode too (`ok: not in break mode`).
- `step into`, `step over` and `step out` wait for the debugger to leave break mode and settle in a new
  break. Success is `ok: executed step <kind> | stable break`. Use `timeoutMs` of 15000 or more: the
  internal stabilization wait is 10 seconds.
- `error: step stabilization timeout waiting for debugger to leave break state` and
  `error: step stabilization timeout waiting for stable break state` are real step failures; report them.
- If `continue` seems to do nothing, check for an immediate breakpoint or trap re-hit first.
- To stop at one place, set a breakpoint there and send `continue; poll break <ms> 200; where 1` once,
  then check where it stopped. If another breakpoint stopped it first, continue one round per `send` and
  check `where` each time. Don't chain several `continue; poll break` rounds in one `send`: a breakpoint
  in a loop, such as a message loop, stops every round, and a round that times out fails the chain and
  skips the rest. Tell the user about breakpoints you didn't set instead of removing them.

## Commands

Commands go through `bh_agent_debugger_send` as plain text.

- `help` lists every command; `help <command>` shows its syntax, the modes it works in and its limits,
  for example `help open dump`, `help where`, `help read mem`, `help set trap symbol`. Help is handled
  locally, works in every mode and never changes debugger state.
- Chain commands in one `send` with `;` or `&&`, for example
  `set bp <file>:<line>; continue; poll break 5000 200; where 5`. The response is
  `ok: chain count=N | #0 ... | #1 ...`.
- `poll break [timeoutMs] [intervalMs]` (defaults 15000 and 200) waits for a break:
  `ok: poll break tries=<n> | ...` or `error: poll break timeout tries=<n> | <last status>`.

### Breakpoints and traps

- `set bp <file>:<line>`, `list bp`, `remove bp <index>` (the zero-based index from `list bp`),
  `clear bp` and `clear all bp`. Responses: `ok: bp set ...`, `ok: bp count=N | #0 ...`,
  `ok: bp removed #<index> ...`, `ok: bp cleared count=N`, `ok: bp empty`.
- Traps are instruction breakpoints set by the debug engine: `set trap symbol <name>`,
  `set trap <address>`, `trap add <address>`, `trap add symbol <symbol>`, `trap list`,
  `trap remove <id>` (the id from `trap list`) and `trap clear`. A trap stops with `reason=trap` on the
  thread that hit it. Run the [preflight](#symbol-and-trap-preflight-required) first.
- `resolve symbol <symbol>` shows the address a symbol resolves to.

### Expressions and locals (break mode)

- `eval <expr>` evaluates in the frame selected in Visual Studio with side effects and function calls
  blocked, and returns `ok: eval expr=<expr> type=<type> value=<value>`. An assignment, increment or
  function call returns `error: eval invalid expression=<expr> value=<evaluator message>`. Only the
  Microsoft C/C++, C# and VB evaluators are accepted; other languages, frames without a debug engine
  frame and a busy Visual Studio fail with an error.
- `eval --side-effects <expr>` (the option must come first) uses the Visual Studio automation evaluator,
  which can change variables or run code in the debuggee.
- `locals [count]` (alias `local variables [count]`) shows 32 locals by default, at most 128:
  `ok: locals count=<total> shown=<n> | #0 name:type=value | ...`. Outside break mode it returns
  `error: local variables requires break mode`.

### Stacks, registers and memory

- `where [frames]` shows the symbolic call stack. `read regs` reads the registers and
  `read stack [bytes]` reads raw stack memory from the selected thread's stack pointer.
- `read mem <address> <size> [hex|ascii|u32|u64|ptr]`: hex bytes by default. `ascii` shows non-printable
  bytes as `.`; `u32`, `u64` and `ptr` show little-endian hexadecimal elements, need a size that is a
  multiple of the element width, and `ptr` uses the target's pointer width. If you don't know an address,
  run `status` or `read regs` first.
- Legacy `stack [value]` means raw bytes in live sessions but frames in dump sessions; prefer `where` and
  `read stack`.

### Threads (break mode)

- `list threads` needs an active break or trap session. Output is sorted by native TID:
  `threads: pid=<pid> count=<n> selected=<tid> | num marker tid ip start main name | #0 * tid=...`, where
  `*` marks the selected thread.
- `select thread <tid>` takes the native TID from `list threads`, not the `#<index>`. It also selects the
  thread in Visual Studio: `ok: selected thread tid=<tid> ip=0x...`. Afterwards `status`, stepping,
  `read regs`, `read stack` and `where` use that thread; `read mem` stays address based.
- A new break selects the stopping thread again; continuing or ending the session clears the selection.
- Errors: `error: select thread usage: select thread <tid>`; a TID outside the debugged process is
  rejected; without a break session, `error: no active trap session (wait for trap hit)`.

### Modules

- `modules` (or `bh_agent_debugger_modules`) lists up to 1000 loaded native modules of the trap
  session's process, or else the active debug process:
  `modules: pid=4916 count=36 | num base name | #0 0x7FF73A560000 TargetApp.exe | #1 0x7FFE36B80000 ntdll.dll`.

### Exceptions

- If a command returns `error: exception thrown ...`, report it directly. Exceptions can interrupt any
  command except `status`.

## Recovery playbook (required)

- `target_not_found`: run `bh_agent_debugger_targets` and `bh_agent_debugger_dte`, pick a fresh
  `vs_<pid>` for the intended solution, and retry the original command once.
- `error: unknown command`: run `help` to see the commands this version supports and check versions with
  `doctor`; retry once.
- `error: local worker failed to resolve symbol`: run `status` and `modules`, then
  `set target pid <debuggee_pid>`, and retry `resolve symbol <name>` once. If it still fails, stop
  guessing and report the exact command and error.
- `continue` returns ok but `status` still shows a break at the same address: treat it as an immediate
  breakpoint or trap re-hit, run `list bp` and `trap list`, and report which ones are active.

## Crash dumps

- Native Windows user-mode x86/x64 `.dmp` files only, opened read-only. Kernel and managed-only dumps
  are not supported.
- Refresh targets and DTE, then pick the intended `vs_<pid>` by solution and root suffix. Opening refuses
  any active debug session; never stop the user's work to make room.
- Open with `bh_agent_debugger_open_dump(targetId, path, timeoutMs?)`, or
  `bh_agent_debugger_send(targetId, 'open dump "C:\CrashDumps\app.dmp"', 30000)`.
- Success means a dump break session, not just an open document. If opening fails or times out, check
  Visual Studio before retrying: it may still be settling.
- Inspect with `status`, `modules`, `list threads`, `select thread <tid>`, `where [frames]`, `read regs`,
  `read stack [bytes]`, `read mem <address> <bytes> [format]`, `eval <expression>` and `locals [count]`.
- Dump memory reads are limited to 4096 bytes; `ptr` follows the dump's architecture. Memory or registers
  the dump didn't capture are reported as unavailable, never read from a live process.
- Stacks, evaluation and locals need the matching executables, DLLs and PDBs. Missing symbols,
  optimized-out locals and unavailable memory are not evidence of corruption. Quote Visual Studio's
  message when evaluation fails.
- `status` shows `session=dump readOnly=true`. Don't use `set target pid` or the symbol and trap
  preflight in a dump session: the dump's PID must never select a live process.
- Execution (`start`, `continue`, stepping, break), traps, queues, breakpoint changes,
  `eval --side-effects` and unknown commands are rejected. `list bp` lists Visual Studio breakpoints,
  not dump events.
- Close only a dump session you opened, with `stop debugging` or `bh_agent_debugger_stop`, then refresh
  targets and DTE.

## CPU profiling

Capture uses Visual Studio's CPU Usage collector from the newest installed Visual Studio 2022 or 2026.

- The broker must already be running: profiling never starts it and never asks for elevation. Don't
  profile through `send`.
- Refresh targets and DTE and use the **debuggee PID**, never the `vs_<pid>` Visual Studio process.
  Capturing does not start, stop, break or resume the target.
- `bh_agent_debugger_profile(action="start", processId=<pid>, seconds=<1..60>)` (default 10 seconds) is
  asynchronous: `state=starting` means accepted, not captured. Poll `status` until `state=running`,
  then exercise the target. The capture stops at the deadline or when the target exits; `stop` ends it
  early, also asynchronously. Poll until `ready` or `failed`.
- `report` returns the path of the finished `.diagsession`, not timing data. Captures and command logs
  stay under `%LOCALAPPDATA%\bh_agent_debugger\profiles\{session-guid}`; nothing is overwritten.
- `output` (start only) is an absolute local drive-letter path of at most 200 characters: a directory
  (created if missing; the broker picks the first unused `capture_N.diagsession`) or a new
  `.diagsession` file. Relative, UNC, device, mapped-network, `.`/`..`, wildcard and stream paths, a
  trailing dot or space, an existing file and paths through a symlink, junction or other reparse point
  are rejected at `start`, before the target is touched. The capture is written to a hidden
  `.bh_partial_*` file and renamed without replacing anything; if the name appears in the meantime the
  state is `failed` and the temporary file is kept (`kept="..."`). The broker never deletes a capture
  you placed. `status` and `report` show `destination="..." caller_owned=1`, and `output="..."` once
  ready. Don't run the broker elevated just to write somewhere.
- `open` with `targetId` opens the last finished report in that Visual Studio. It refuses an active run
  or break session.
- One capture runs per broker. An uncertain stop blocks new captures and reports `cleanup_required=1`:
  read the saved command logs and retry `stop`. Never stop unrelated profiler sessions. Elevated targets
  may need elevation, which is never automatic.

### Offline analysis

- `bh_agent_debugger_profile_analyze(input, processId, ...)` turns a saved `.etl` or single-ETL
  `.diagsession` into bounded JSON: functions by self and inclusive sampled time, a call tree, hot paths
  and quality counters. It needs no broker, Visual Studio or elevation. `symbols` is one local PDB
  directory; nothing is downloaded.
- `processId` must be the exact recorded debuggee PID; absent or reused PIDs fail. An archive with more
  than one ETL fails rather than choosing one.
- Missing stacks stay in the percentage denominators, unresolved frames keep their addresses, and ETW
  warnings are reported. CPU milliseconds are estimates from the sampling interval, not elapsed time or
  per-call durations.
- Analysis is limited to 120 seconds and 1 GiB. A forced stop can leave `%TEMP%\bh-profile-analysis-*`
  folders behind.
- **Source drill-down:** `function` is an exact containing-function name or trace-local id (prefer the id
  when a name is ambiguous; there is no substring matching). `sourceDrilldown` lists self samples by
  PDB file and line, inline contexts from outer to inner, an address fallback and explicit
  denominators. Use PDBs that match the capture, not the current build. `none_reported` does not prove
  there was no inlining. Each sample counts once: this is self attribution, not inclusive line timing.
  Source paths are build metadata only and the analyzer never reads source files
  (`sourceTextVerified=false`); read the matching source yourself.
- **Filters:** `startMs` (inclusive) and `endMs` (exclusive) are trace-relative; `threadId` is a
  recorded native TID; `subtree` is an exact physical function name or frame id and keeps the stacks
  that contain it (recursion counts once). Filters combine with AND and with `function`. Ambiguous
  names, absent selectors and reused TIDs fail.
- Read `filters` before interpreting filtered percentages: process samples = retained + excluded +
  indeterminate. Exclusion precedence is time, thread, subtree. Rankings use the retained samples. An
  empty window is a zero-sample result, not a failure. `filters.threads` lists the recorded TIDs.
- With `subtree`, hot paths start at the subtree frame (`framesAboveOmitted` counts the trimmed callers),
  the call tree is rooted at the subtree's caller contexts, and callers above it at 100% inclusive are
  dropped from the rankings (`analysis.subtreeScope`). `sections` (`functionsBySelf`,
  `functionsByInclusive`, `callTree`, `hotPaths`) limits the output; unselected sections are null. To fit
  a bounded response, prefer a subtree, then sections, then a smaller `top`.

## CLI equivalents

`bh_agent_debugger_cli` with no arguments prints its commands. Most take `--timeout-ms <n>`.

- doctor, targets, dte: `doctor`, `targets-list`, `dte-list`
- modules: `modules --target-id vs_<pid>`
- start: `start-debugging <hint> [--args "<command line>"]`
- attach: `attach-debugging <hint> --pid <n>`
- stop: `stop-debugging <hint>`
- chain: `start-debugging-chain <hint1> <hint2> ...`
- send: `send-target vs_<pid> "<command>"`
- open dump: `open-dump "<path.dmp>" --target-id vs_<pid>`
- profile: `profile start --pid <pid> [--seconds <n>] [--output <path>]`, `profile stop`,
  `profile status`, `profile report`, `profile open --target-id vs_<pid>`
- analyze: `profile analyze --input <path> --pid <pid> [--top <n>] [--symbols <dir>] [--function <name>]
  [--start-ms <ms>] [--end-ms <ms>] [--thread <tid>] [--subtree <name>] [--sections <a,b>]`

## Command list

- Help: `help [command]`
- Session: `status`, `modules`, `start debugging` (`start`), `stop debugging` (`stop`),
  `open dump "<absolute-path.dmp>"`
- Execution: `continue`, `async break` (`break`), `step into`, `step over`, `step out`,
  `poll break [timeoutMs] [intervalMs]`
- Breakpoints: `set bp <file>:<line>`, `list bp`, `remove bp <index>`, `clear bp`, `clear all bp`
- Traps: `set target pid <pid>`, `resolve symbol <symbol>`, `set trap <address>`,
  `set trap symbol <symbol>`, `trap add <address>`, `trap add symbol <symbol>`, `trap list`,
  `trap remove <id>`, `trap clear`, `trap resume`
- Queue: `queue continue`, `queue list`, `queue clear`, `queue run` (see `help <command>`)
- Inspection: `locals [count]` (`local variables [count]`), `eval [--side-effects] <expr>`,
  `list threads`, `select thread <tid>`, `where [frames]`, `read regs`, `read stack [bytes]`,
  `read mem <address> <size> [hex|ascii|u32|u64|ptr]`, `stack [value]` (legacy)
