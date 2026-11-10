# Service + CLI Cutover Plan

This plan tracks migration from `bh_agent_debugger.ps1` to `agentd` + `agentctl`.

## Current state

- `agentd` service pipe is implemented in `bh_agent_debugger` console app.
- `agentctl` CLI is implemented in the same executable.
- Routing rule: CLI talks only to service; service talks to package endpoint pipe.
- Deterministic routing baseline is implemented with broker-managed `targetId` entries.

## Implemented commands

- Transport and targeting
  - `agentctl send`
  - `agentctl send-target`
  - `agentctl send-json`
  - `agentctl targets-list`
- Core wrappers (broker send wrappers)
  - `agentctl status`
  - `agentctl list-solutions`
  - `agentctl diag-session`
  - `agentctl set-trap <symbol>`
  - `agentctl read-regs`
  - `agentctl read-stack [bytes]`
- Orchestration wrappers (currently delegated to existing PowerShell script)
  - `agentctl start-main`
  - `agentctl stop-main`
  - `agentctl start-exp`
  - `agentctl stop-exp`

## Remaining work before full cutover

1. Service-side discovery integration
   - Continue hardening automatic target discovery table from running VS instances.
2. Routing maturity
   - Add `suffix` and `pid` selectors in broker resolution path (not only `targetId`).
3. Reliability hardening
   - Structured logging, retry policy, heartbeat, stale target cleanup.
4. Script compatibility mode
   - Keep wrapper mode in `bh_agent_debugger.ps1` that forwards to `agentctl` for supported commands.
5. Deprecation and cleanup
   - Mark direct script transport paths deprecated after parity and remove old command paths in a final cleanup phase.

## Recommended transition policy

- Phase A (now): keep script as primary for production workflows; use `agentctl` for iterative testing.
- Phase B: move team workflows to `agentctl` commands while keeping script fallback.
- Phase C: default to `agentctl`; keep script as legacy wrapper only.
- Phase D: remove legacy direct-pipe script transport after stabilization window.
