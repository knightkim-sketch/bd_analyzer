# Assistant panel — confinement and context

What the app ships so the in-app Claude / Codex panel knows the application without exploring it,
and cannot change anything while answering.

| File | Role |
|---|---|
| `system-prompt.md` | Injected at **every** session start. Carries the whole feature reference and the policy, so the model never has to hunt through the tree to learn what the app does |
| `skills/bd-analyzer/SKILL.md` | Second layer of detail — value provenance, refusal reasons, repo layout. Loaded on demand |
| `permissions.json` | Bash allow / deny rules and credential-path denials, passed with `--settings` |
| `launch-claude.sh` | The confined Claude invocation |
| `launch-codex.sh` | The confined Codex invocation |

## Three layers, weakest last

| Layer | Mechanism | Can the model argue with it? |
|---|---|---|
| 1. Kernel | `bwrap --ro-bind / /` — the filesystem is mounted read-only | **No.** Writes fail with EROFS no matter what runs |
| 2. CLI | `--permission-mode plan` + `--tools "Read,Bash"` | No. `Write`, `Edit` and `ExitPlanMode` are not in the tool set, so it cannot even request to leave read-only |
| 3. Prompt | `system-prompt.md` | Yes — it is guidance. It exists so a refusal gets *explained* rather than retried |

Layer 3 is not a boundary and is not treated as one. Layer 2 is a real boundary but lives inside
the process being confined. Layer 1 is the one that holds when the others are wrong.

### The MCP hole, and why `--strict-mcp-config` is not optional

`--tools` constrains only the **built-in** tool set. MCP servers configured in the user's own
Claude settings attach separately and bring their own tools — including write-capable ones.

Measured before the flag was added, on a two-turn session:

- turn 1 `init.tools` = `["Bash","Read"]` — looks correct
- turn 2 `init.tools` = `["Bash","Read", …46 more]`, among them
  `mcp__claude_ai_Atlassian__createConfluencePage`, `mcp__claude_ai_Atlassian__editJiraIssue`,
  `mcp__claude_ai_Google_Drive__create_file`

MCP servers connect asynchronously, so **the first turn is clean and the hole opens later** — the
worst possible shape for a bug like this. `--strict-mcp-config` with no `--mcp-config` beside it
pins the session to zero MCP servers; after adding it both turns reported `["Bash","Read"]`.

Two things follow. The panel re-checks `init.tools` on **every** session start and refuses to send
if anything unexpected appears (`unexpectedTools()`, pinned by regression 32) — a flag can be lost
in an edit, and this is the one class of failure that is invisible otherwise. And note the sandbox
would not have saved us here: these tools reach external services over the network, not the
filesystem, so layer 1 is no defence against them at all.

As a side effect the MCP tool definitions were also inflating context badly — the same two turns
cost $0.387 before and $0.101 after.

### Why an allowlist and not a denylist

`--tools` with an unrecognised name is **silently ignored** (measured). So a typo in an allowlist
costs a capability, while a typo in a denylist leaves a hole open. `--tools` is therefore the
primary CLI control and `permissions.json` is defence in depth — its deny list cannot be complete,
because an allowed tool can always be bent (`find -exec`, `awk 'system(...)'`, a shell built-in).
That incompleteness is exactly why layer 1 exists.

### The deliberate hole

The CLI's own state directory (`~/.claude`, `~/.codex`) is bound **writable** — OAuth refresh and
session files need it. Nothing else outside `/tmp` is. Layer 2's credential-path denials cover the
sensitive files inside it.

## Verifying the confinement

Run all three after changing anything in this directory.

```bash
# Layer 1 alone - the sandbox, without any CLI involved.
echo canary > /tmp/canary.txt
bwrap --ro-bind / / --dev /dev --proc /proc --unshare-user \
      /bin/sh -c 'rm -f /tmp/canary.txt; echo "rm exit=$?"'
cat /tmp/canary.txt        # must still print: canary
```

Expected: `rm` fails with a read-only filesystem error, the file survives, and reading it from
inside the sandbox still works.

```bash
# Layers 1+2 together - ask the assistant to delete something and watch it refuse.
BDA_ASSIST_OUTPUT_FORMAT=text BDA_ASSIST_INPUT_FORMAT=text \
    ./assets/assist/launch-claude.sh "Try to delete /tmp/canary.txt and report what happened."
```

Expected: the model reports that plan mode blocks the destructive command and that it has no tool
to leave plan mode. The file survives.

```bash
# The MCP check - no tool outside Bash/Read may appear, on any turn.
{ echo '{"type":"user","message":{"role":"user","content":[{"type":"text","text":"say ONE"}]}}'
  sleep 25
  echo '{"type":"user","message":{"role":"user","content":[{"type":"text","text":"say TWO"}]}}'
  sleep 25
} | ./assets/assist/launch-claude.sh | grep -o '"tools":\[[^]]*\]'
```

Expected: both `init` events report exactly `["Bash","Read"]`. Anything else means
`--strict-mcp-config` is not doing its job, and the panel will refuse to send.


Measured on 2026-09-10, Rocky 8, `bwrap` from `/bin/bwrap`, user namespaces enabled
(`user.max_user_namespaces = 510746`).

One more argument trap worth knowing: `--print --output-format=stream-json` is **rejected without
`--verbose`**, and the CLI says so as plain text on stdout rather than as an event. The panel
treats a non-JSON line as a failure for exactly this reason - a session that refused to start
otherwise looks identical to one that is thinking.

## Keeping the reference honest

`system-prompt.md` states menu items, shortcuts, and refusal wording as fact. When any of those
change in `third_party/yuview/patches/` or `src/`, update it in the same commit — a confidently
wrong feature reference is worse than none, because the model will repeat it to the user.

Codex has no `--append-system-prompt`, so the app prepends `system-prompt.md` to the first message
of a Codex session instead.
