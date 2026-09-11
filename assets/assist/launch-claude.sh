#!/usr/bin/env bash
# Launch the Claude CLI for the bd_analyzer assistant panel.
#
# The app spawns this rather than the CLI directly, so the whole confinement recipe sits in one
# reviewable file. Three layers, in order of how much they can be argued with:
#
#   1. Kernel      - bwrap mounts the filesystem read-only, and in edit mode binds the working
#                    directory (and only that) writable on top. Nothing the model or the CLI does
#                    can write or delete anywhere else. Verified: under the edit bind, writing
#                    outside the workspace and removing a system binary both fail with EROFS.
#   2. CLI         - a tool allowlist. In readonly mode it contains no writer at all and plan mode
#                    blocks the rest; in edit mode Write and Edit join it, and layer 1 is what
#                    keeps them inside the working directory.
#   3. Prompt      - system-prompt.md states the policy. Guidance, not a boundary; it is here so
#                    the model explains a refusal instead of fighting it.
#
# The tool allowlist is the safe direction: an unrecognised name in --tools is silently ignored, so
# a typo costs a capability rather than opening a hole. A denylist has the opposite failure mode,
# which is why permissions.json is defence in depth and not the boundary.
#
# --strict-mcp-config is not optional. --tools constrains only the built-in set; MCP servers from
# the user's own configuration attach separately and bring write-capable tools with them (measured:
# a second turn arrived with Confluence page-creation and Drive file-creation tools in the session).
# They connect asynchronously, so the first turn looks clean and the hole opens later. With no
# --mcp-config alongside it, this pins the session to zero MCP servers.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${BDA_ASSIST_WORKDIR:-$PWD}"
MODEL="${BDA_ASSIST_MODEL:-opus}"
BUDGET="${BDA_ASSIST_BUDGET_USD:-5.00}"
# The panel drives a streaming session; the confinement check in README.md runs one plain-text
# turn instead, so both formats are overridable rather than hard-coded.
OUT_FORMAT="${BDA_ASSIST_OUTPUT_FORMAT:-stream-json}"
IN_FORMAT="${BDA_ASSIST_INPUT_FORMAT:-stream-json}"

# readonly | edit. The panel sets this; edit is the default because reading a stream and then not
# being able to fix the code that produced it is the shape of the job here.
MODE="${BDA_ASSIST_MODE:-edit}"
case "$MODE" in
    readonly|edit) ;;
    *) echo "bd_analyzer: BDA_ASSIST_MODE must be 'readonly' or 'edit', got '$MODE'" >&2; exit 2 ;;
esac

command -v claude >/dev/null 2>&1 || { echo "bd_analyzer: 'claude' not found in PATH" >&2; exit 127; }
command -v bwrap  >/dev/null 2>&1 || { echo "bd_analyzer: 'bwrap' not found - refusing to run unsandboxed" >&2; exit 127; }

# The CLI needs its own state directory writable: OAuth refresh, session files, plan scratch.
# Everything else stays read-only. This is the one hole in layer 1 and it is deliberate.
#
# /tmp is a private tmpfs, not a bind of the host's. It used to be `--bind /tmp /tmp`, which
# quietly made every file under /tmp writable - measured: in edit mode the assistant wrote to a
# path under /tmp that was nowhere near the working directory, and correctly reported that the
# boundary had not held. A tmpfs gives the CLI the scratch space it needs and nothing else.
STATE="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"

# What separates the two modes, and what does not.
#
# Changes: whether the working directory is writable, and whether Write/Edit exist.
#
# Does not change: everything outside the working directory stays read-only in both modes, so
# /etc, /usr and the user's other projects are untouchable either way. Deletion and system
# administration also stay denied by permissions.json in both - "the assistant may edit the files
# it is working on" is a different request from "the assistant may run rm", and only the first was
# made.
if [[ "$MODE" == "edit" ]]; then
    WORKSPACE_BIND=(--bind "$WORKDIR" "$WORKDIR")
    TOOLS="Read,Bash,Write,Edit"
    PERMISSION_MODE="acceptEdits"
else
    WORKSPACE_BIND=()
    TOOLS="Read,Bash"
    PERMISSION_MODE="plan"
fi

# --verbose is not optional here: `--print --output-format=stream-json` is rejected without it
# (measured - "requires --verbose"). Token-level streaming and message acknowledgement likewise
# only exist on the stream-json pair.
STREAM_ARGS=()
if [[ "$OUT_FORMAT" == "stream-json" ]]; then
    STREAM_ARGS+=(--verbose --include-partial-messages)
    [[ "$IN_FORMAT" == "stream-json" ]] && STREAM_ARGS+=(--replay-user-messages)
fi

exec bwrap \
    --ro-bind / / \
    --dev /dev \
    --proc /proc \
    --tmpfs /run \
    --tmpfs /var/tmp \
    --bind "$STATE" "$STATE" \
    --tmpfs /tmp \
    "${WORKSPACE_BIND[@]}" \
    --unshare-pid --unshare-ipc --unshare-uts \
    --die-with-parent \
    --chdir "$WORKDIR" \
    claude \
        -p \
        --output-format "$OUT_FORMAT" \
        --input-format "$IN_FORMAT" \
        "${STREAM_ARGS[@]}" \
        --permission-mode "$PERMISSION_MODE" \
        --tools "$TOOLS" \
        --strict-mcp-config \
        --setting-sources user \
        --settings "$HERE/permissions.json" \
        --append-system-prompt "$(cat "$HERE/system-prompt.md")" \
        --model "$MODEL" \
        --max-budget-usd "$BUDGET" \
        "$@"
