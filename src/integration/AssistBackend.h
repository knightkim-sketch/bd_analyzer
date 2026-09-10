// The assistant panel's view of a CLI session: the events it can receive, and the guard that
// checks a session actually came up read-only.
//
// The event shapes here are not guessed - they were captured from `claude -p --output-format
// stream-json --verbose` and are pinned by regression 32. Anything the panel does not need is
// folded into Kind::Ignored rather than modelled, so an added event type cannot break parsing.
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

namespace bda::integration
{

//!< One thing that happened in a session, already reduced to what the panel draws.
struct AssistEvent
{
  enum class Kind
  {
    Ignored,        //!< Housekeeping we do not draw (rate limits, block starts, replayed input).
    SessionStarted, //!< `system`/`init`. Carries what the session was actually granted.
    TextDelta,      //!< A fragment of the answer, for typing-as-it-arrives.
    TurnFinished,   //!< `result`. The whole answer plus what the turn cost.
    Failed,         //!< The CLI reported an error, or wrote something that is not an event.
  };

  Kind        kind{Kind::Ignored};
  QString     text;           //!< Delta text, final answer, or the failure message.
  QStringList tools;          //!< SessionStarted: the tool names the session really has.
  QString     sessionId;
  QString     permissionMode; //!< SessionStarted: expected to be "plan".
  QString     model;
  double      costUsd{};      //!< TurnFinished: cumulative session cost as the CLI reports it.
};

/* Reduce one line of the CLI's JSONL output to an event.
 *
 * A line that is not JSON is a failure rather than something to skip: the CLI writes plain text
 * when it rejects its own arguments ("requires --verbose"), and swallowing that leaves the panel
 * waiting forever on a session that never started.
 */
AssistEvent parseAssistEvent(const QByteArray &line);

/* The tools a read-only session is allowed to have. Everything else is a finding.
 *
 * An allowlist rather than a denylist, and the difference is not theoretical: `--tools` constrains
 * only the built-in set, so MCP servers from the user's own configuration attach separately and
 * bring write-capable tools with them. Measured on a two-turn session before --strict-mcp-config
 * was added: the second turn arrived holding Confluence page-creation and Drive file-creation
 * tools. They connect asynchronously, so the first turn looked clean.
 */
QStringList readOnlyToolAllowlist();

//!< Whatever in `tools` is not on the allowlist. Empty means the session came up as intended.
QStringList unexpectedTools(const QStringList &tools);

//!< A user turn in the shape the CLI's stream-json input expects, newline terminated.
QByteArray encodeUserMessage(const QString &text);

/* A session with one assistant CLI.
 *
 * Deliberately not "one process = one session": Claude holds a process open and takes turns on
 * stdin, while `codex exec` is one process per turn and resumes by id. Both have to fit behind
 * this, so the panel never learns which shape it is talking to.
 */
class AssistBackend : public QObject
{
  Q_OBJECT

public:
  using QObject::QObject;
  ~AssistBackend() override = default;

  virtual void start()                        = 0;
  virtual void send(const QString &message)   = 0;
  virtual void cancel()                       = 0;
  virtual bool running() const                = 0;
  //!< Why the backend cannot run, for the panel to show instead of failing silently. Empty if ok.
  virtual QString unavailableReason() const = 0;

signals:
  void assistEvent(const bda::integration::AssistEvent &event);
  void finished(int exitCode);
};

} // namespace bda::integration
