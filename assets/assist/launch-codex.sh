#!/usr/bin/env bash
# Launch the Codex CLI for the bd_analyzer assistant panel, read-only.
#
# Same three layers as launch-claude.sh. Codex differs in two ways that matter here:
#
#   - It has its own OS-level sandbox (--sandbox read-only), so layer 1 is doubled: bwrap outside,
#     the Codex sandbox inside. Both are kept - they fail independently.
#   - `codex exec` is one turn per invocation. Continuity comes from `exec resume`, so the app
#     re-runs this script per turn instead of holding one process open.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${BDA_ASSIST_WORKDIR:-$PWD}"

command -v codex >/dev/null 2>&1 || { echo "bd_analyzer: 'codex' not found in PATH" >&2; exit 127; }
command -v bwrap >/dev/null 2>&1 || { echo "bd_analyzer: 'bwrap' not found - refusing to run unsandboxed" >&2; exit 127; }

STATE="${CODEX_HOME:-$HOME/.codex}"
[[ -d "$STATE" ]] || STATE="$HOME"

exec bwrap \
    --ro-bind / / \
    --dev /dev \
    --proc /proc \
    --tmpfs /run \
    --tmpfs /var/tmp \
    --bind "$STATE" "$STATE" \
    --bind /tmp /tmp \
    --unshare-pid --unshare-ipc --unshare-uts \
    --die-with-parent \
    --chdir "$WORKDIR" \
    codex exec \
        --json \
        --sandbox read-only \
        --skip-git-repo-check \
        -C "$WORKDIR" \
        "$@"
