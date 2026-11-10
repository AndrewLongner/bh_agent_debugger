#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST_DIR="${HOME}/.config/opencode"

mkdir -p "${DEST_DIR}/tools"
mkdir -p "${DEST_DIR}/skills/bh-agent-debugger"

cp -f "${SCRIPT_DIR}/tools/bh_agent_debugger.ts" "${DEST_DIR}/tools/bh_agent_debugger.ts"
cp -f "${SCRIPT_DIR}/skills/bh-agent-debugger/SKILL.md" "${DEST_DIR}/skills/bh-agent-debugger/SKILL.md"

echo "OpenCode WSL tool/skill synced to ${DEST_DIR}"
