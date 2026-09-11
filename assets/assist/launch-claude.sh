#!/usr/bin/env bash
# Launch the Claude CLI for the bd_analyzer assistant panel.
#
# The app spawns this rather than the CLI directly, so the whole recipe sits in one reviewable
# file. There are three modes; see the MODE block below for what each one is.
#
# In readonly and edit mode the confinement is three layers, in order of how much they can be
# argued with:
#
#   1. Kernel      - bwrap mounts the filesystem read-only, and in edit mode binds the working
#                    directory (and only that) writable on top. Verified: under the edit bind,
#                    writing outside the workspace and removing a system binary both fail.
#   2. CLI         - a tool allowlist and a permission ruleset.
#   3. Prompt      - the policy fragment. Guidance, not a boundary; it is here so the model
#                    explains a refusal instead of fighting it.
#
# **full mode has none of these.** No sandbox, every built-in tool, no permission rules. It is an
# ordinary shell's worth of access held by an agent, running as the user, and it is the default
# because the work this panel exists for is a chain that any single refusal breaks. Nothing here
# protects the machine in that mode - what does is the user watching what they asked for.
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

# readonly | edit | full. The panel sets this; full is the default.
#
# full is the default because the work this panel exists for is a chain - find a test sequence,
# download it, lay out a playlist, run an encoder sweep, measure BD-rate - and a single refusal
# anywhere in that chain stops all of it. The narrower modes are kept for when the assistant is
# only being asked to explain what is already on screen.
MODE="${BDA_ASSIST_MODE:-full}"
case "$MODE" in
    readonly|edit|full) ;;
    *) echo "bd_analyzer: BDA_ASSIST_MODE must be 'readonly', 'edit' or 'full', got '$MODE'" >&2
       exit 2 ;;
esac

command -v claude >/dev/null 2>&1 || { echo "bd_analyzer: 'claude' not found in PATH" >&2; exit 127; }
# bwrap is only needed by the confined modes. full mode does not sandbox at all, so requiring it
# there would refuse to start for no benefit.
if [[ "$MODE" != "full" ]]; then
    command -v bwrap >/dev/null 2>&1 || {
        echo "bd_analyzer: 'bwrap' not found - refusing to run $MODE mode unsandboxed" >&2
        exit 127
    }
fi

# The CLI needs its own state directory writable: OAuth refresh, session files, plan scratch.
# Everything else stays read-only. This is the one hole in layer 1 and it is deliberate.
#
# /tmp is a private tmpfs, not a bind of the host's. It used to be `--bind /tmp /tmp`, which
# quietly made every file under /tmp writable - measured: in edit mode the assistant wrote to a
# path under /tmp that was nowhere near the working directory, and correctly reported that the
# boundary had not held. A tmpfs gives the CLI the scratch space it needs and nothing else.
STATE="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"

# What each mode is.
#
#   readonly - read and search anywhere, change nothing.
#   edit     - additionally create and modify files inside the working directory, and only there.
#   full     - no sandbox, every built-in tool, no permission ruleset. Downloads, copies, deletes,
#              runs encoders and build scripts. This is an ordinary shell's worth of access, held
#              by an agent, with the user's own credentials.
#
# MODE_ARGS carries the per-mode CLI flags; POLICY names the prompt fragment that tells the model
# what it is allowed to do, so the prompt never claims a restriction the flags do not impose.
POLICY="$HERE/policy-$MODE.md"
case "$MODE" in
    full)
        WORKSPACE_BIND=()
        # No --tools (every built-in), no --settings (no permission rules), nothing refused.
        MODE_ARGS=(--permission-mode bypassPermissions)
        ;;
    edit)
        WORKSPACE_BIND=(--bind "$WORKDIR" "$WORKDIR")
        MODE_ARGS=(--permission-mode acceptEdits
                   --tools "Read,Bash,Write,Edit"
                   --settings "$HERE/permissions.json")
        ;;
    readonly)
        WORKSPACE_BIND=()
        MODE_ARGS=(--permission-mode plan
                   --tools "Read,Bash"
                   --settings "$HERE/permissions.json")
        ;;
esac

# The app reference is mode-neutral; the policy is not. Concatenating them keeps the prompt honest
# - in full mode it must not repeat a list of things that are in fact allowed.
SYSTEM_PROMPT="$(cat "$HERE/system-prompt.md")
$(cat "$POLICY")"

# --verbose is not optional here: `--print --output-format=stream-json` is rejected without it
# (measured - "requires --verbose"). Token-level streaming and message acknowledgement likewise
# only exist on the stream-json pair.
STREAM_ARGS=()
if [[ "$OUT_FORMAT" == "stream-json" ]]; then
    STREAM_ARGS+=(--verbose --include-partial-messages)
    [[ "$IN_FORMAT" == "stream-json" ]] && STREAM_ARGS+=(--replay-user-messages)
fi

CLAUDE_ARGS=(
    -p
    --output-format "$OUT_FORMAT"
    --input-format "$IN_FORMAT"
    "${STREAM_ARGS[@]}"
    "${MODE_ARGS[@]}"
    # Kept in every mode. It blocks nothing this panel is used for - the workflow needs web search,
    # downloads and encoders, none of which are MCP - while MCP tool definitions measured four
    # times the token cost for the same two answers. Drop it here if you want your MCP servers.
    --strict-mcp-config
    --setting-sources user
    --append-system-prompt "$SYSTEM_PROMPT"
    --model "$MODEL"
    --max-budget-usd "$BUDGET"
    "$@"
)

# full mode runs the CLI directly. Wrapping it in a sandbox that binds everything writable would
# be theatre, and it would still get in the way - a downloaded file has to survive the session.
if [[ "$MODE" == "full" ]]; then
    cd "$WORKDIR" || exit 1   # bwrap's --chdir does this for the other modes
    exec claude "${CLAUDE_ARGS[@]}"
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
    claude "${CLAUDE_ARGS[@]}"
