# Security Policy

## Supported versions

Security fixes are made for the latest release only.

## Reporting a vulnerability

Report vulnerabilities privately through GitHub's private vulnerability reporting: open this
repository's **Security** tab and choose **Report a vulnerability**. Please do not open a public issue
for a vulnerability that is not fixed yet.

Include the version (`bh_agent_debugger_cli --version`), your Visual Studio version, the steps to
reproduce and the impact you observed. Follow-up happens in the private advisory.

Vulnerabilities in Visual Studio, the Concord debug engine or the .NET runtime belong to Microsoft;
report them to the [Microsoft Security Response Center](https://msrc.microsoft.com).

## Security model

BH Agent Debugger exists to let software, usually an AI coding agent, drive the Visual Studio
debugger. Read this section before connecting an agent.

### A connected agent can run code as you

- Starting or attaching a debugger, launching with arguments, opening dumps and evaluating
  expressions run with the rights of the signed-in user, and with Visual Studio's rights when it runs
  elevated. A debuggee that an agent starts or resumes runs arbitrary code by design.
- Treat any agent connected to the CLI, the OpenCode tool or the MCP server as having your user
  account's access. Keep your agent's per-tool approval prompts on for the tools that are not
  read-only (see [docs/mcp.md](docs/mcp.md#tools)).
- Debuggee output, variable values, source text, symbols, module names and dump contents reach the
  agent as text. Treat them as untrusted input: they can contain instructions aimed at the agent
  (prompt injection). Review what an agent proposes to do with them.

### Local transport only

- The CLI, the broker service and the Visual Studio package talk over local named pipes. Each pipe
  is owned by, and only accessible to, the user account that created it; network clients are
  rejected; and clients refuse a pipe that another account owns. Both sides fail closed.
- Nothing listens on the network. The MCP server uses stdio only.
- Processes running as your own account can use the pipes. The tool does not protect you from code
  that already runs as you.

### Defaults that limit side effects

- `eval <expr>` blocks side effects and function evaluation. `eval --side-effects <expr>` must be
  requested explicitly, and dump sessions refuse it.
- Crash dumps are opened read-only; execution, breakpoint changes and traps are rejected in dump
  sessions.
- Offline profile analysis reads only the capture and one local PDB directory you name. It uses no
  symbol servers, source servers or SourceLink. (Visual Studio itself follows its own symbol
  settings while debugging.)
- The broker and profiling never request elevation. Do not run the broker or Visual Studio elevated
  unless you need to debug an elevated process.
- The MSI installs per user only and never needs administrator rights. Its scheduled task starts
  the broker in your interactive session with limited rights.

### Out of scope

- Actions an agent performs with tools you allowed it to use.
- Code that runs in a debuggee you chose to start, attach to or resume.
- Issues in Visual Studio, the debug engine or the .NET runtime (report those to Microsoft).
