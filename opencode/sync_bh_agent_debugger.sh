#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

SRC_TOOL="$SCRIPT_DIR/tools/bh_agent_debugger.ts"
SRC_SKILL="$SCRIPT_DIR/skills/bh-agent-debugger/SKILL.md"

TARGET_ROOTS=("$HOME/.config/opencode")

if command -v cmd.exe >/dev/null 2>&1; then
  WIN_USERPROFILE_RAW="$(cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d '\r' | sed -e 's/^\s*//' -e 's/\s*$//')"
  if [[ -n "$WIN_USERPROFILE_RAW" && "$WIN_USERPROFILE_RAW" == [A-Za-z]:\\* ]]; then
    drive_letter="${WIN_USERPROFILE_RAW:0:1}"
    drive_letter="$(printf '%s' "$drive_letter" | tr '[:upper:]' '[:lower:]')"
    win_rest="${WIN_USERPROFILE_RAW:2}"
    win_rest="${win_rest//\\//}"
    TARGET_ROOTS+=("/mnt/${drive_letter}${win_rest}/.config/opencode")
  fi
fi

if [[ ! -f "$SRC_TOOL" ]]; then
  echo "Missing source tool file: $SRC_TOOL" >&2
  exit 1
fi

if [[ ! -f "$SRC_SKILL" ]]; then
  echo "Missing source skill file: $SRC_SKILL" >&2
  exit 1
fi

for root in "${TARGET_ROOTS[@]}"; do
  tool_dir="$root/tools"
  skill_dir="$root/skills/bh-agent-debugger"
  dest_tool="$tool_dir/bh_agent_debugger.ts"
  dest_skill="$skill_dir/SKILL.md"

  mkdir -p "$tool_dir" "$skill_dir"

  src_tool_real="$(readlink -f "$SRC_TOOL")"
  src_skill_real="$(readlink -f "$SRC_SKILL")"
  dest_tool_real="$(readlink -f "$dest_tool" 2>/dev/null || true)"
  dest_skill_real="$(readlink -f "$dest_skill" 2>/dev/null || true)"

  if [[ "$src_tool_real" != "$dest_tool_real" ]]; then
    cp "$SRC_TOOL" "$dest_tool"
  fi

  if [[ "$src_skill_real" != "$dest_skill_real" ]]; then
    cp "$SRC_SKILL" "$dest_skill"
  fi

  echo "Synced to $root"
done

echo "Done."
