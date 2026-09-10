#!/usr/bin/env bash
# Launch the Claude CLI for the bd_analyzer assistant panel, read-only.
#
# The app spawns this rather than the CLI directly, so the whole confinement recipe sits in one
# reviewable file. Three layers, in order of how much they can be argued with:
#
#   1. Kernel      - bwrap mounts the filesystem read-only. Nothing the model or the CLI does can
#                    write, delete, or rename outside the few paths bound writable below. Verified:
#                    rm and overwrite both fail with EROFS, reads succeed.
#   2. CLI         - plan mode plus a tool allowlist that omits Write/Edit/ExitPlanMode, so the
#                    model cannot even ask to leave read-only.
#   3. Prompt      - system-prompt.md states the policy. Guidance, not a boundary; it is here so
#                    the model explains a refusal instead of fighting it.
#
# The tool allowlist is the safe direction: an unrecognised name in --tools is silently ignored, so
# a typo costs a capability rather than opening a hole. A denylist has the opposite failure mode,
# which is why permissions.json is defence in depth and not the boundary.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${BDA_ASSIST_WORKDIR:-$PWD}"
MODEL="${BDA_ASSIST_MODEL:-opus}"
BUDGET="${BDA_ASSIST_BUDGET_USD:-5.00}"
# The panel drives a streaming session; the confinement check in README.md runs one plain-text
# turn instead, so both formats are overridable rather than hard-coded.
OUT_FORMAT="${BDA_ASSIST_OUTPUT_FORMAT:-stream-json}"
IN_FORMAT="${BDA_ASSIST_INPUT_FORMAT:-stream-json}"

command -v claude >/dev/null 2>&1 || { echo "bd_analyzer: 'claude' not found in PATH" >&2; exit 127; }
command -v bwrap  >/dev/null 2>&1 || { echo "bd_analyzer: 'bwrap' not found - refusing to run unsandboxed" >&2; exit 127; }

# The CLI needs its own state directory writable: OAuth refresh, session files, plan scratch.
# Everything else stays read-only. This is the one hole in layer 1 and it is deliberate.
STATE="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"

# Token-level streaming and message acknowledgement only exist on the stream-json pair; passing
# them with --output-format text is rejected.
STREAM_ARGS=()
if [[ "$OUT_FORMAT" == "stream-json" ]]; then
    STREAM_ARGS+=(--include-partial-messages)
    [[ "$IN_FORMAT" == "stream-json" ]] && STREAM_ARGS+=(--replay-user-messages)
fi

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
    claude \
        -p \
        --output-format "$OUT_FORMAT" \
        --input-format "$IN_FORMAT" \
        "${STREAM_ARGS[@]}" \
        --permission-mode plan \
        --tools "Read,Bash" \
        --settings "$HERE/permissions.json" \
        --append-system-prompt "$(cat "$HERE/system-prompt.md")" \
        --model "$MODEL" \
        --max-budget-usd "$BUDGET" \
        "$@"
