# Service + CLI JSON IPC Protocol (Step 2)

This document defines the v1 JSON protocol between:

- `agentctl` (CLI) and `agentd` (service broker)
- `agentd` and per-instance package endpoints

`agentctl` to package direct transport is out of scope by design.

## Transport

- Local named pipes.
- UTF-8 JSON, one message per line (NDJSON framing).
- Request/response model with correlation id.
- Default request timeout: `5000` ms unless overridden by `timeoutMs`.

## Envelope

All requests and responses use a common envelope.

### Request

```json
{
  "protocolVersion": 1,
  "id": "req-0001",
  "method": "send",
  "target": {
    "targetId": "t-exp-target",
    "pid": 21784,
    "suffix": "ExpTarget",
    "solution": "<repo-root>\\TargetApp\\TargetApp.sln"
  },
  "timeoutMs": 5000,
  "params": {
    "message": "set trap symbol target_work"
  }
}
```

### Success response

```json
{
  "protocolVersion": 1,
  "id": "req-0001",
  "ok": true,
  "result": {
    "text": "ok: trap symbol set"
  },
  "meta": {
    "targetId": "t-exp-target",
    "pid": 21784,
    "elapsedMs": 41
  }
}
```

### Error response

```json
{
  "protocolVersion": 1,
  "id": "req-0001",
  "ok": false,
  "error": {
    "code": "ambiguous_target",
    "message": "Multiple targets matched suffix 'ExpTarget'",
    "details": {
      "matches": ["t-exp-target-a", "t-exp-target-b"]
    }
  },
  "meta": {
    "elapsedMs": 2
  }
}
```

## Methods (v1)

- `targets.list`
  - Params: optional filters (`suffix`, `pid`, `solutionContains`).
  - Result: array of targets.

- `targets.register` (bootstrap helper)
  - Params: `targetId`, `pipeName`.
  - Adds or updates a broker target entry.

- `targets.unregister` (bootstrap helper)
  - Params: `targetId`.
  - Removes a dynamic broker target entry.

- `targets.heartbeat` (bootstrap helper)
  - Params: `targetId`.
  - Refreshes target liveness timestamp.

- `targets.show`
  - Params: target selector.
  - Result: single target or `target_not_found`.

- `send`
  - Params: `message` (string).
  - Requires target selection unless exactly one target exists.
  - Step 4 bootstrap: service also accepts `target.pipeName` for explicit package endpoint override during migration.
  - Step 5 bootstrap: service accepts `targetId` and rejects unresolved multi-target sends with `ambiguous_target`.

- `diag.session`
  - Params: none.
  - Requires target selection.

- `debug.startMain`
  - Params: `solution`, `startupProject`, `configuration`, `platform`.
  - Current implementation is native broker DTE command execution (`Debug.Start`).
  - Includes readiness gate: waits for package endpoint transport availability.

- `debug.startHint`
  - Params: `solutionHint`, optional `arguments`, optional `timeoutMs`.
  - Resolves an open DTE instance by solution hint (exact solution-name match first, then path contains match).
  - When `arguments` is present, temporarily updates the active startup project's debugger arguments for one launch and restores the previous value after Visual Studio enters run/break mode or the request fails.
  - Returns `already_debugging` without changing arguments when Visual Studio is already in run or break mode.
  - Executes `Debug.Start` on that instance and waits for debugger mode `run`/`break`.
  - Registers inferred `targetId = vs_<pid>` and performs endpoint readiness probe.

- `debug.attachPid`
  - Params: `solutionHint`, `processId`, optional `timeoutMs`.
  - Resolves the Visual Studio instance by solution hint and attaches its debugger to the local process.
  - Registers inferred `targetId = vs_<pid>` and waits for debugger mode `run`/`break`.

- `debug.stopMain`
  - Params: none.
  - Current implementation is native broker DTE command execution (`Debug.StopDebugging`).

- `debug.startExp`
  - Params: `suffix`, `solution`, `startupProject`, `configuration`, `platform`, `forceLaunch`.
  - Current implementation launches missing Exp instance when `forceLaunch=true`, then runs `Debug.Start` via DTE.
  - Includes readiness gate: waits for package endpoint transport availability.

- `debug.stopExp`
  - Params: `suffix`, optional `kill`.
  - Current implementation runs `Debug.StopDebugging` via DTE on the suffix-matched instance.

- `chain.run`
  - Params: `steps` array.
  - Each step is a request fragment with optional `continueOnError`.

## Target Resolution Rules

Resolution precedence:

1. `target.targetId`
2. `target.pid`
3. `target.suffix` (+ optional `target.solution`)

Rules:

- If no match: `target_not_found`
- If multiple matches: `ambiguous_target`
- Service must not auto-pick among multiple matches.

## Error Codes

Standard error codes for v1:

- `invalid_request`
- `unsupported_method`
- `target_not_found`
- `ambiguous_target`
- `timeout`
- `transport_error`
- `backend_error`
- `access_denied`
- `internal_error`

## Timeouts and Retries

- CLI sends desired `timeoutMs`.
- Service may retry transient transport failures internally.
- Service returns a single response for each request id.
- On timeout, service returns `ok=false` with `code=timeout`.
- Current bootstrap implementation retries pipe send up to 3 attempts.

## Liveness and stale cleanup

- Broker tracks `last_seen` for targets.
- The package unregisters its target (`targets.unregister`) when Visual Studio closes. Dynamic targets that
  were not unregistered, for example after a crash, are pruned when stale (current bootstrap threshold: 120 seconds).
- Only package-registered targets are listed or probed. There is no synthetic default target.
- Target-less send/probe requests require exactly one registered target; zero targets return `target_not_found` and multiple targets return `ambiguous_target`.

## Versioning

- `protocolVersion` is required.
- v1 service accepts `protocolVersion = 1` only.
- Unknown version returns `invalid_request`.

## Logging and Correlation

- Service logs request `id`, resolved `targetId`, method, and elapsed ms.
- Error logs include `error.code` and compact message.
- Sensitive payload fields should be redacted where needed.
- Current bootstrap writes service and CLI logs to `%LOCALAPPDATA%\bh_agent_debugger\bh_agent_debugger.log`.
- `dte.list`
  - Result: broker-side DTE/ROT discovered instances with pid, mode, root, solution.

- `health.summary`
  - Result: aggregated broker health text including targets, DTE list, and per-target pipe probe status.
