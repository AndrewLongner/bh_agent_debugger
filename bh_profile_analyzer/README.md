# Offline CPU analysis (0.12.1+)

A separate .NET 10 Windows command-line helper using Microsoft's **TraceEvent 3.1.28** NuGet package (MIT). It does not load .NET into the native CLI, broker, or Visual Studio and does not create or control a trace session.

Build with repository `build.bat` (requires .NET 10 SDK; pinned dependency graph in `packages.lock.json`). The helper, runtime configuration and NuGet dependencies are placed beside the native CLI in `Temp/bh_agent_debugger_cli_x64_Debug`. These development outputs require a compatible .NET 10 runtime. The 0.12.1 MSI separately publishes and includes a self-contained win-x64 analyzer and runtime, so installed analysis does not require a separately installed .NET runtime. Do not copy only the helper EXE: its DLL, deps/runtimeconfig, TraceEvent and native dependency files are also required.

```powershell
.\Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe profile analyze --input "C:\captures\capture.diagsession" --pid 1234 --top 20 --symbols "C:\build\pdbs"
```

`profile report` supplies the saved capture path; analysis takes an explicit archive/ETL path and **recorded debuggee PID** so it also works after broker restart. It is offline: no `--pipe`, DTE selection, debugger attachment, or elevation. Archive must contain exactly one ETL; otherwise explicitly extract/select the intended ETL rather than guessing. ZIP entry paths are never used for extraction destinations. Originals are never modified and an adjacent stale ETLX is never reused.

## JSON contract (schemaVersion 1)

- stdout: one JSON result; `ok:false` and nonzero exit on handled errors. Native/runtime failures can additionally write stderr and exit without a JSON result; always check exit status.
- Process: PID, trace-local ProcessIndex, name, lifetime bounds. Missing PID, reused/ambiguous PID, and zero selected CPU samples fail closed.
- Metrics: sample count, TraceLog sample interval and estimated CPU milliseconds. Interval may be a library default; a constant interval is assumed. These are **statistical CPU estimates**, not elapsed time, per-call durations, or call counts. They can differ from WPA's per-event weighted timings.
- Functions: separate top lists ranked by self and inclusive samples. Recursive occurrences contribute once per sample to flat inclusive counts. IDs are trace-local, not stable identifiers for cross-capture comparisons.
- Call tree: bounded preorder rows with node/parent IDs and sample counts, hottest children first. Truncation counters distinguish missing output rows from complete totals. Recursive nodes remain separate by call path.
- Hot paths: root-to-leaf stacks ranked by exact-stack sample count (not summed parent+child totals).
- Quality: ETW lost events, missing/truncated stacks, unresolved leaf/frame samples, symbol lookup exceptions, and bounded diagnostic warnings. Missing stacks are retained as synthetic `[missing-stack]` samples. Percentages use **retained process samples after filters**, not only resolved samples. Without filters this is all process samples. `filters.processQuality` preserves whole-process quality; uncertain subtree nonmatches are reported separately as indeterminate, not treated as proven matches or hidden exclusions. ETW lost-event counts and conversion diagnostics apply to the full trace.
- Symbols: optional **one local PDB directory**, cache-only lookup. No implicit symbol-server downloads; inherited symbol-server settings are cleared. Without matching PDBs, module/address fallbacks remain explicit. Kernel/OS frames often remain unresolved with application-only PDBs.

Limits: 512 MiB input/inflated ETL, 4096 ZIP entries, 5 million selected samples, 128 physical frames/sample, bounded address/function/tree tables, `--top 1..50` (default 20), 1 MiB UTF-8 JSON. The native CLI runs its helper in a kill-on-close job with 1 GiB process memory and a 120-second deadline. Large/corrupt traces can fail rather than yield a partial success. Normal execution removes its unique `%TEMP%/bh-profile-analysis-*` scratch directory; forced termination/crash can leave it for manual cleanup. It never deletes source archives, PDBs, or unrelated scratch folders.

No graphical heat-map UI, cache-miss metrics, cross-capture diff, remote symbols, or arbitrary .diagsession resource selection is implemented.

## Source-line/inline drill-down (0.12.2+)

Append `--function <exact-name-or-trace-local-id>` to analyze. Optional OpenCode typed argument: `function`. An exact name shared by multiple physical functions fails rather than guessing; an absent/unresolved name returns `sourceDrilldown.status=function_unresolved_or_absent`, with the process quality counters intact. IDs can also select explicit unknown buckets such as `[missing-stack]`.

The additive schemaVersion 1 `sourceDrilldown` object contains disjoint self-sample rows for the selected **physical containing function**. It does not filter/change the existing process function/tree metrics or count physical callees as self samples. Every selected sample contributes once to the deepest unambiguous inline source location. Each row includes PDB file/line/column, containing/inline contexts (outer-to-inner; parent location represents call site), address fallback (four addresses, explicit omitted count), percent of function self samples, and percent of ALL process samples, and percent of retained samples after filters. `--top` bounds the rows; `rowsOmitted` and `samplesInOmittedRows` preserve omitted mass. Coverage is `resolvedLineSamples + unresolvedLineSamples = selectedSelfSamples`; shown rows plus omitted-row samples also equal the selected self count. No inclusive line totals are claimed.

`bh_profile_dia.dll` is a small x64 C++ C-ABI helper loaded **only inside the already isolated .NET analyzer**. `build.bat` builds it using the VS DIA SDK without changing a solution/vcxproj. It uses the existing bundled `amd64/msdia140.dll`, not registered COM or an arbitrary PATH DLL. It opens only explicitly supplied local PDB-directory files and validates the trace's PDB GUID AND age before RVA lookup. Native module load/unload time and image range must identify the RVA uniquely. No source server, SourceLink, srcsrv command execution, or .NET NGEN symbol generation is used.

Inline normalization uses DIA lexical parents, not enumeration order. Ambiguous chains, incomplete/unsupported queries, truncated chains/strings and ambiguous line records are explicit unresolved buckets rather than invented deepest locations. Inline depth is bounded to 16 plus the physical function. `inlineStatus=none_reported` does not prove code was not inlined: the compiler/PDB may omit inline records. PDB line attribution can differ from a human's apparent expression costs in optimized code. `sourceTextVerified=false`: file paths are build-time metadata; source contents/checksums are not read or verified. Use the matching source snapshot when interpreting rows.

Build output and the 0.12.2+ MSI include the native helper. Building the MSI does not update installed analyzer binaries. Native bh_agent's separate tool registry also needs its new argument forwarded.

## Time/thread/call-subtree filters (0.12.2+)

Combine any of these with `profile analyze` (and optionally `--function`):

```text
--start-ms 5000 --end-ms 15000 --thread 56268 --subtree physics_step
```

- `--start-ms` inclusive / `--end-ms` exclusive: trace-relative milliseconds, NOT process-relative or wall-clock timestamps. Missing start means 0; missing end means unbounded. Finite nonnegative decimal/exponent notation, maximum 1e12. End must exceed start. Half-open intervals partition a trace without duplicating boundary samples.
- `--thread`: one recorded native TID, not a thread-list index. See bounded `filters.threads` discovery rows (process/retained sample counts). A TID with no process samples, unknown lifetime, or multiple sampled lifetimes fails closed; no merging reused TIDs even across a time window.
- `--subtree`: exact physical function name or trace-local frame ID, across all caller contexts. Retains samples whose physical stack contains that frame, preserving their full stacks for context. Recursive matches count once. This is not an inline-only selector or a specific call-tree node/path. Ambiguous names fail, even if another filter would hide the ambiguity. An absent/unresolved selector fails rather than returning a misleading zero-cost result; supply local PDBs or an exact frame ID.
- All filters compose with AND. Accounting uses disjoint precedence **time -> thread -> subtree**. Unresolved/missing/truncated stacks without a proven subtree match are `indeterminateSubtreeSamples`; fully resolved nonmatches are excluded. Known matches with unresolved other frames are retained. Missing stacks are retained normally when no subtree filter is used. These rules are deliberately conservative; system frames without PDBs can make many nonmatches indeterminate.
- `filters.processSamples = retainedSamples + excludedSamples + indeterminateSubtreeSamples`. `excludedSamples` is broken into `excludedByTime`, `excludedByThread`, `excludedBySubtree`. Original and indeterminate quality counters remain visible. `metrics`, function/tree/path counts, CPU estimates and their percentage denominator cover retained samples only. Empty valid windows return zero samples, null sample timestamps, empty rankings and explicit `empty_selection` status; zero confirmed matches with uncertainty have their own status.
- Source drill-down runs within the retained set, preserving both whole-process and retained counts. Its function-self denominator is scoped; `percentOfProcessSamples` continues to use the whole-process count, while `percentOfRetainedSamples` reports the filtered denominator. Its original process-level missing/unresolved counts stay intact.
- Repository OpenCode optional arguments: `startMs`, `endMs`, `threadId`, `subtree`. Native bh_agent must independently add/forward them in its schema/dispatcher. No installed tool/skill sync or MSI build is implied. Durable JSON/CSV file export remains a follow-up.

## Subtree-scoped output and section selection (0.12.4+)

- With `--subtree`, `analysis.callTreeScope` is `subtree`. Hot paths start at the root-most subtree frame; `framesAboveOmitted` counts the trimmed callers (it is 0 without a subtree). The call tree starts at each root-most caller context of the subtree frame (those nodes have `parentId: null`); recursive inner matches stay inside their outer context.
- Frames above the subtree in **every** retained sample and never inside it (startup/harness/`ntdll` callers at 100% inclusive) are left out of `functionsBySelf`/`functionsByInclusive`. `totalFunctions` still counts them. `analysis.subtreeScope` reports `frameId`, `callerContexts`, `samplesUnderCallerContexts` (equals retained samples), `callerContextsShownInCallTree`, and `alwaysAboveSubtreeFramesOmittedFromRankings`. Partial callers (above the subtree in only some samples) stay ranked.
- `--sections <comma-list>` selects any nonempty, duplicate-free subset of `functionsBySelf,functionsByInclusive,callTree,hotPaths` (default all). Unselected sections are `null` and listed in `analysis.sectionsOmitted`. Metrics, quality, filters and source drill-down are always emitted.
- Example: a game capture analyzed with `--top 30 --subtree <function>` is about 40 KB (it was over 60 KiB before).
- Without `--subtree` or `--sections`, the output is unchanged apart from the additive `callTreeScope`, `framesAboveOmitted`, `subtreeScope: null` and `sectionsOmitted: []` fields. `schemaVersion` stays 1.

## Validation

- `build.bat`
- `tests/test_profile_analysis.ps1` (deterministic aggregation/archive/parser/error/inline-normalization tests)
- `tests/test_profile_filters.ps1 -InputFile <capture> -RecordedPid <pid> -Symbols <pdb-directory> -Subtree <exact-function>` (time partitions, native TID, inclusive-count equivalence, combined/source scope, empty windows, invalid arguments, hash preservation)
- `tests/test_profile_dia.bat` (compiler-produced `/O2 /Ob2 /Zo /Zi` nested inline fixture; physical/inline call-site lines and GUID/age mismatch)
- `tests/test_profile_analysis.ps1 -InputFile <saved capture> -RecordedPid <pid> -Symbols <pdb directory> [-ExpectedSamples <independent reference>] [-Function <name>] [-ExpectedFunctionSamples <count>]`

The archived-capture test validates native CLI forwarding, symbols, JSON bounds, original-file hash preservation, absent PID, no-symbol mode, and Unicode/space paths. `-ExpectedSamples` compares against an independent reference (e.g. WPA). No new capture or active debugger session is required.
