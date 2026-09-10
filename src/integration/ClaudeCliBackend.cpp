#include "integration/ClaudeCliBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace bda::integration
{

namespace
{
constexpr int kTerminateGraceMs = 2000;
}

ClaudeCliBackend::ClaudeCliBackend(QObject *parent) : AssistBackend(parent)
{
  this->launcher = locateLauncher();
}

ClaudeCliBackend::~ClaudeCliBackend()
{
  /* The panel can be destroyed with a turn in flight, and this repository has no other place that
   * spawns a child process - so nothing else would reap it. Close stdin first: the CLI exits on
   * its own when its input ends, which is a cleaner shutdown than a signal.
   */
  if (this->process && this->process->state() != QProcess::NotRunning)
  {
    this->process->closeWriteChannel();
    if (!this->process->waitForFinished(kTerminateGraceMs))
    {
      this->process->terminate();
      if (!this->process->waitForFinished(kTerminateGraceMs))
        this->process->kill();
    }
  }
}

QString ClaudeCliBackend::locateLauncher()
{
  const auto name = QStringLiteral("launch-claude.sh");

  if (const auto override = qEnvironmentVariable("BDA_ASSIST_DIR"); !override.isEmpty())
    return QDir(override).filePath(name);

  const auto appDir = QCoreApplication::applicationDirPath();
  const QStringList candidates{
      QDir(appDir).filePath("assist/" + name),                  // deployed bundle
      QDir(appDir).filePath("../../assets/assist/" + name),      // build/YUViewApp -> repo root
      QDir(appDir).filePath("../../../assets/assist/" + name),   // one level deeper
  };
  for (const auto &candidate : candidates)
    if (QFileInfo::exists(candidate))
      return QFileInfo(candidate).canonicalFilePath();

  return {};
}

QString ClaudeCliBackend::unavailableReason() const
{
  if (this->launcher.isEmpty())
    return QStringLiteral("The assistant launcher (assist/launch-claude.sh) was not found next to "
                          "the application. Set BDA_ASSIST_DIR to its directory.");
  if (!QFileInfo(this->launcher).isExecutable())
    return QStringLiteral("%1 is not executable.").arg(this->launcher);
  if (QStandardPaths::findExecutable(QStringLiteral("claude")).isEmpty())
    return QStringLiteral("The 'claude' CLI is not on PATH. Install it and sign in; the panel "
                          "cannot provide it.");
  /* Refuse rather than run unconfined. The tool allowlist and plan mode both live inside the
   * process being confined, so without the sandbox there is no layer left that the model cannot
   * argue with.
   */
  if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
    return QStringLiteral("'bwrap' (bubblewrap) is not installed. The assistant runs read-only "
                          "inside it, so the panel will not start without it.");
  return {};
}

bool ClaudeCliBackend::running() const
{
  return this->process && this->process->state() != QProcess::NotRunning;
}

void ClaudeCliBackend::start()
{
  if (this->running())
    return;

  if (const auto reason = this->unavailableReason(); !reason.isEmpty())
  {
    AssistEvent event;
    event.kind = AssistEvent::Kind::Failed;
    event.text = reason;
    emit this->assistEvent(event);
    return;
  }

  this->pending.clear();
  this->process = new QProcess(this);
  this->process->setProgram(this->launcher);

  auto environment = QProcessEnvironment::systemEnvironment();
  if (!this->workingDirectory.isEmpty())
    environment.insert(QStringLiteral("BDA_ASSIST_WORKDIR"), this->workingDirectory);
  this->process->setProcessEnvironment(environment);
  if (!this->workingDirectory.isEmpty())
    this->process->setWorkingDirectory(this->workingDirectory);

  connect(this->process, &QProcess::readyReadStandardOutput, this, [this]() {
    this->drainStdout();
  });
  connect(this->process, &QProcess::readyReadStandardError, this, [this]() {
    this->drainStderr();
  });
  connect(this->process,
          &QProcess::finished,
          this,
          [this](int exitCode, QProcess::ExitStatus) { emit this->finished(exitCode); });
  connect(this->process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error != QProcess::Crashed || this->running())
    {
      AssistEvent event;
      event.kind = AssistEvent::Kind::Failed;
      event.text = QStringLiteral("The assistant process failed: %1")
                       .arg(this->process ? this->process->errorString() : QString());
      emit this->assistEvent(event);
    }
  });

  this->process->start();
}

void ClaudeCliBackend::send(const QString &message)
{
  if (!this->running())
    this->start();
  if (!this->running())
    return;
  this->process->write(encodeUserMessage(message));
}

void ClaudeCliBackend::cancel()
{
  if (!this->running())
    return;
  /* Closing stdin ends the session rather than the turn - the CLI has no mid-turn interrupt on
   * this interface. The panel says so, so the user is not told a turn was cancelled when what
   * actually happened is that the conversation ended.
   */
  this->process->closeWriteChannel();
  if (!this->process->waitForFinished(kTerminateGraceMs))
    this->process->terminate();
}

void ClaudeCliBackend::drainStdout()
{
  this->pending.append(this->process->readAllStandardOutput());

  /* One event per line, but a read can land mid-line, so only complete lines are parsed and the
   * remainder is carried to the next read. Splitting the buffer eagerly would corrupt any event
   * large enough to arrive in two chunks - which is most of them once a whole answer is in flight.
   */
  int newline = 0;
  while ((newline = this->pending.indexOf('\n')) >= 0)
  {
    const auto line = this->pending.left(newline);
    this->pending.remove(0, newline + 1);
    if (const auto event = parseAssistEvent(line); event.kind != AssistEvent::Kind::Ignored)
      emit this->assistEvent(event);
  }
}

void ClaudeCliBackend::drainStderr()
{
  const auto text = QString::fromUtf8(this->process->readAllStandardError()).trimmed();
  if (text.isEmpty())
    return;
  AssistEvent event;
  event.kind = AssistEvent::Kind::Failed;
  event.text = text;
  emit this->assistEvent(event);
}

} // namespace bda::integration
