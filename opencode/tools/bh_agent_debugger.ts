import { tool } from "@opencode-ai/plugin"

const CLI_EXE = process.env.BH_AGENT_CLI_EXE || "bh_agent_debugger_cli.exe"

async function runCli(args: string[]): Promise<string> {
  const proc = Bun.spawn([CLI_EXE, ...args], {
    stdout: "pipe",
    stderr: "pipe",
  })

  const stdout = (await new Response(proc.stdout).text()).trim()
  const stderr = (await new Response(proc.stderr).text()).trim()
  const code = await proc.exited

  const body = stdout || stderr || "(no output)"

  if (code !== 0) {
    throw new Error(`bh_agent_debugger_cli failed (code=${code}): ${body}`)
  }

  let parsed: { ok?: unknown; result?: { text?: unknown }; error?: { message?: unknown } } | null = null
  try {
    parsed = JSON.parse(body)
  } catch {
    // Return raw output when response is not JSON.
    return body
  }

  // A broker reply with "ok": false is a tool error carrying error.message.
  if (parsed?.ok === false) {
    const message = parsed.error?.message
    throw new Error(typeof message === "string" && message.length > 0 ? message : body)
  }

  const text = parsed?.result?.text
  if (typeof text === "string" && text.length > 0) {
    return text
  }

  return body
}

export const doctor = tool({
  description: "Run bh_agent_debugger doctor health check",
  args: {
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 5000
    return runCli(["doctor", "--timeout-ms", String(timeoutMs)])
  },
})

export const targets = tool({
  description: "List debugger targets",
  args: {
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 5000
    return runCli(["targets-list", "--timeout-ms", String(timeoutMs)])
  },
})

export const dte = tool({
  description: "List DTE instances",
  args: {
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 5000
    return runCli(["dte-list", "--timeout-ms", String(timeoutMs)])
  },
})

export const modules = tool({
  description: "List native modules; one-line table, max 1000",
  args: {
    targetId: tool.schema.string().describe("Target id like vs_12345"),
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 5000
    return runCli(["modules", "--target-id", args.targetId, "--timeout-ms", String(timeoutMs)])
  },
})

export const profile = tool({
  description: "Capture VS CPU Usage into a diagsession; start/stop are asynchronous. Poll status until ready/failed. Analysis is in VS, not a text export.",
  args: {
    action: tool.schema.enum(["start", "stop", "status", "report", "open"]),
    processId: tool.schema.number().int().positive().max(4294967295).optional().describe("Debuggee PID; required only for start, never devenv PID"),
    seconds: tool.schema.number().int().min(1).max(60).optional().describe("Capture duration; start only, default 10"),
    targetId: tool.schema.string().optional().describe("vs_<pid>; required only for open. Refresh targets/DTE and select an idle VS instance."),
    output: tool.schema.string().min(3).max(200).optional().describe("Start only (0.12.3+): absolute local directory (unique capture_N.diagsession) or explicit .diagsession file; never overwrites; caller-owned"),
    timeoutMs: tool.schema.number().int().min(1).max(120000).optional(),
  },
  async execute(args) {
    if (args.action === "start" ? args.processId === undefined : args.processId !== undefined || args.seconds !== undefined || args.output !== undefined)
      throw new Error("processId is required only for start; seconds and output are start-only")
    if (args.action === "open" ? !args.targetId : args.targetId !== undefined)
      throw new Error("targetId is required only for open")
    const cliArgs = ["profile", args.action]
    if (args.processId !== undefined) cliArgs.push("--pid", String(args.processId))
    if (args.seconds !== undefined) cliArgs.push("--seconds", String(args.seconds))
    if (args.targetId !== undefined) cliArgs.push("--target-id", args.targetId)
    if (args.output !== undefined) cliArgs.push("--output", args.output)
    cliArgs.push("--timeout-ms", String(args.timeoutMs ?? 30000), "--no-auto-start-service")
    return runCli(cliArgs)
  },
})

export const profile_analyze = tool({
  description: "Offline CPU analysis of an ETL/diagsession into bounded JSON using TraceEvent. No capture, elevation, broker, or symbol downloads.",
  args: {
    input: tool.schema.string().describe("Absolute path to saved local ETL or single-ETL diagsession"),
    processId: tool.schema.number().int().positive().max(2147483647).describe("Exact recorded PID; ambiguous reused PIDs are rejected"),
    top: tool.schema.number().int().min(1).max(50).optional(),
    symbols: tool.schema.string().optional().describe("One local PDB directory; no symbol servers"),
    function: tool.schema.string().min(1).max(1024).optional().describe("Exact containing-function name or trace-local id for inline-aware self source-line drill-down"),
    startMs: tool.schema.number().min(0).max(1e12).optional().describe("Inclusive trace-relative time in milliseconds"),
    endMs: tool.schema.number().min(0).max(1e12).optional().describe("Exclusive trace-relative time; must exceed startMs (default 0)"),
    threadId: tool.schema.number().int().positive().max(2147483647).optional().describe("Recorded native thread ID, not list index"),
    subtree: tool.schema.string().min(1).max(1024).optional().describe("Exact physical root function name or frame id; keeps samples containing it; unresolved nonmatches are reported as indeterminate"),
    sections: tool.schema.array(tool.schema.enum(["functionsBySelf", "functionsByInclusive", "callTree", "hotPaths"])).min(1).max(4).optional().describe("0.12.4+: analysis sections to include (default all); omitted sections are null and listed in sectionsOmitted"),
  },
  async execute(args) {
    const cliArgs = ["profile", "analyze", "--input", args.input, "--pid", String(args.processId), "--top", String(args.top ?? 20)]
    if (args.symbols !== undefined) cliArgs.push("--symbols", args.symbols)
    if (args.function !== undefined) cliArgs.push("--function", args.function)
    if (args.startMs !== undefined) cliArgs.push("--start-ms", String(args.startMs))
    if (args.endMs !== undefined) cliArgs.push("--end-ms", String(args.endMs))
    if (args.threadId !== undefined) cliArgs.push("--thread", String(args.threadId))
    if (args.subtree !== undefined) cliArgs.push("--subtree", args.subtree)
    if (args.sections !== undefined) cliArgs.push("--sections", args.sections.join(","))
    return runCli(cliArgs)
  },
})

export const open_dump = tool({
  description: "Open a native x86/x64 Windows dump in an explicit VS target; refuses active sessions",
  args: {
    targetId: tool.schema.string().describe("Target id like vs_12345; refresh targets and DTE first"),
    path: tool.schema.string().describe("Absolute local path to a .dmp file (read-only)"),
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds; default 30000"),
  },
  async execute(args) {
    return runCli(["open-dump", args.path, "--target-id", args.targetId, "--timeout-ms", String(args.timeoutMs ?? 30000)])
  },
})

export const start = tool({
  description: "Start debugging by solution hint, optionally overriding startup-project arguments",
  args: {
    hint: tool.schema.string().describe("Solution hint, eg bh_agent_debugger"),
    arguments: tool.schema.string().optional().describe("Raw debugger command-line arguments"),
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 15000
    const cliArgs = ["start-debugging", args.hint]
    if (args.arguments !== undefined) cliArgs.push("--args", args.arguments)
    cliArgs.push("--timeout-ms", String(timeoutMs))
    return runCli(cliArgs)
  },
})

export const attach = tool({
  description: "Attach Visual Studio debugging to a process by PID",
  args: {
    hint: tool.schema.string().describe("Solution hint used to select Visual Studio"),
    processId: tool.schema.number().int().positive().describe("Target process ID"),
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 15000
    return runCli(["attach-debugging", args.hint, "--pid", String(args.processId), "--timeout-ms", String(timeoutMs)])
  },
})

export const stop = tool({
  description: "Stop debugging by solution hint",
  args: {
    hint: tool.schema.string().describe("Solution hint, eg bh_agent_debugger"),
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 15000
    return runCli(["stop-debugging", args.hint, "--timeout-ms", String(timeoutMs)])
  },
})

export const chain = tool({
  description: "Run start-debugging-chain with solution hints",
  args: {
    hints: tool.schema.array(tool.schema.string()).min(1).describe("Ordered solution hints"),
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 30000
    return runCli(["start-debugging-chain", ...args.hints, "--timeout-ms", String(timeoutMs)])
  },
})

export const send = tool({
  description: "Send target command (status/break/bp/trap/modules or open dump with a quoted absolute path)",
  args: {
    targetId: tool.schema.string().describe("Target id like vs_12345"),
    command: tool.schema.string().describe("Raw command text, eg status, modules, list bp, remove bp <index>, local variables [count], trap add symbol <symbol>"),
    timeoutMs: tool.schema.number().optional().describe("Timeout in milliseconds"),
  },
  async execute(args) {
    const timeoutMs = args.timeoutMs ?? 15000
    return runCli(["send-target", args.targetId, args.command, "--timeout-ms", String(timeoutMs)])
  },
})
