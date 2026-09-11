// The assistant panel's event parsing and its read-only guard.
//
// Every JSON line below was captured from a real `claude -p --output-format stream-json --verbose`
// session, not written from documentation - the schema is not documented anywhere we control, and
// the first version of the launcher was wrong about it twice (stream-json needs --verbose; the CLI
// reports argument complaints as bare text on the same stream).
//
// The case that earns this test its place is the tool guard. A read-only session is read-only
// because of what --tools grants, but --tools only constrains the built-in set: MCP servers from
// the user's own configuration attach separately, asynchronously, and bring write-capable tools.
// Measured before --strict-mcp-config was added, a two-turn session's second init arrived holding
// Confluence page-creation and Google Drive file-creation tools. The first turn looked clean.
#include <QCoreApplication>
#include <iostream>
#include <string>

#include "integration/AssistBackend.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

template <typename T> void checkEqual(T got, T want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (mismatch)";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

using namespace bda::integration;
} // namespace

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  std::cout << "assist event parsing" << std::endl;

  // --- session init -----------------------------------------------------------------------
  {
    const QByteArray line =
        R"({"type":"system","subtype":"init","session_id":"9a7720e7-aaaa","model":"claude-sonnet-5",)"
        R"("permissionMode":"plan","tools":["Bash","Read"],"cwd":"/tmp"})";
    const auto event = parseAssistEvent(line);
    check(event.kind == AssistEvent::Kind::SessionStarted, "init is a session start");
    check(event.permissionMode == "plan", "permission mode is read out");
    check(event.model == "claude-sonnet-5", "model is read out");
    check(event.sessionId.startsWith("9a7720e7"), "session id is read out");
    check(event.tools.size() == 2, "both tools are read out");
    check(unexpectedTools(event.tools, AssistMode::ReadOnly).isEmpty(),
          "Bash+Read is an expected read-only session");
  }

  // --- the MCP hole, in both modes ---------------------------------------------------------
  {
    const QStringList withMcp{"Bash",
                              "Read",
                              "mcp__claude_ai_Atlassian__createConfluencePage",
                              "mcp__claude_ai_Google_Drive__create_file"};
    for (const auto mode : {AssistMode::ReadOnly, AssistMode::Edit})
    {
      const auto unexpected = unexpectedTools(withMcp, mode);
      check(unexpected.size() == 2, "MCP write tools are unexpected in " +
                                        assistModeName(mode).toStdString() + " mode");
      check(unexpected.contains("mcp__claude_ai_Google_Drive__create_file"),
            "and the Drive file-creation tool is named");
    }
  }

  // --- the modes differ by exactly two names ----------------------------------------------
  {
    check(!unexpectedTools({"Bash", "Read", "Write"}, AssistMode::ReadOnly).isEmpty(),
          "Write is unexpected in read-only mode");
    check(!unexpectedTools({"Bash", "Read", "Edit"}, AssistMode::ReadOnly).isEmpty(),
          "Edit is unexpected in read-only mode");

    check(unexpectedTools({"Bash", "Read", "Write", "Edit"}, AssistMode::Edit).isEmpty(),
          "Write and Edit are expected in edit mode");

    /* Edit mode widens the list, it does not switch the guard off. Deletion and shell escapes are
     * not granted by "the assistant may edit files", and an extra built-in arriving unannounced
     * is exactly the MCP failure in a different coat.
     */
    check(!unexpectedTools({"Bash", "Read", "Write", "Edit", "NotebookEdit"}, AssistMode::Edit)
               .isEmpty(),
          "an extra built-in is still a finding in edit mode");
    check(!unexpectedTools({"Bash", "Read", "Write", "Edit", "WebFetch"}, AssistMode::Edit)
               .isEmpty(),
          "so is a network tool");

    checkEqual(toolAllowlist(AssistMode::Edit).size(),
               toolAllowlist(AssistMode::ReadOnly).size() + 2,
               "edit mode adds exactly two tools");

    for (const auto mode : {AssistMode::ReadOnly, AssistMode::Edit})
      check(unexpectedTools({}, mode).isEmpty(), "an empty tool set is not a finding");
  }
  {
    checkEqual(assistModeName(AssistMode::Edit), QString("edit"), "the edit mode name matches the launcher");
    checkEqual(assistModeName(AssistMode::ReadOnly), QString("readonly"),
               "and so does the read-only one");
  }

  // --- streamed text ----------------------------------------------------------------------
  {
    const QByteArray line =
        R"({"type":"stream_event","session_id":"x","event":{"type":"content_block_delta",)"
        R"("delta":{"type":"text_delta","text":"ello"}}})";
    const auto event = parseAssistEvent(line);
    check(event.kind == AssistEvent::Kind::TextDelta, "a text delta is a text delta");
    check(event.text == "ello", "the fragment survives verbatim");
  }
  {
    // Structure we do not draw must not become a visible event.
    for (const QByteArray line :
         {QByteArray(R"({"type":"stream_event","event":{"type":"message_start"}})"),
          QByteArray(R"({"type":"stream_event","event":{"type":"content_block_stop"}})"),
          QByteArray(R"({"type":"rate_limit_event","session_id":"x"})"),
          QByteArray(R"({"type":"user","message":{"role":"user","content":[]}})"),
          QByteArray("")})
      check(parseAssistEvent(line).kind == AssistEvent::Kind::Ignored,
            "housekeeping is ignored: " + std::string(line.left(46).constData()));
  }

  // --- turn result ------------------------------------------------------------------------
  {
    const QByteArray line =
        R"({"type":"result","subtype":"success","is_error":false,"result":"hello",)"
        R"("total_cost_usd":0.13195300000000001,"num_turns":1,"session_id":"x"})";
    const auto event = parseAssistEvent(line);
    check(event.kind == AssistEvent::Kind::TurnFinished, "a successful result finishes the turn");
    check(event.text == "hello", "the final answer is carried");
    check(event.costUsd > 0.13 && event.costUsd < 0.14, "the reported cost is carried");
  }
  {
    const QByteArray line =
        R"({"type":"result","subtype":"error_during_execution","is_error":true,"result":"boom"})";
    check(parseAssistEvent(line).kind == AssistEvent::Kind::Failed, "an error result fails");
  }

  // --- non-JSON is a failure, not noise ---------------------------------------------------
  {
    /* This exact line is what the CLI printed when the launcher was missing --verbose. Treating it
     * as something to skip left the panel waiting on a session that had already refused to start.
     */
    const QByteArray line = "Error: When using --print, --output-format=stream-json requires "
                            "--verbose";
    const auto       event = parseAssistEvent(line);
    check(event.kind == AssistEvent::Kind::Failed, "a bare error line is surfaced, not skipped");
    check(event.text.contains("requires --verbose"), "and its text is kept for the user");
  }

  // --- the input envelope -----------------------------------------------------------------
  {
    /* Verified against a live session: two messages in this shape, written one per line, were both
     * answered inside a single process with the same session id.
     */
    const auto encoded = encodeUserMessage("why is this superblock expensive?");
    check(encoded.endsWith('\n'), "a message is newline terminated or the turn never starts");
    check(encoded.contains("\"type\":\"user\""), "the envelope is a user message");
    check(encoded.contains("\"role\":\"user\""), "with a user role inside");
    check(encoded.contains("\"type\":\"text\""), "and a text content block");
    check(encoded.contains("why is this superblock expensive?"), "carrying the question");
    check(!encoded.left(encoded.size() - 1).contains('\n'),
          "and no embedded newline, which would split one turn into two");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
