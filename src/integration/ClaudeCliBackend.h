// Drives the Claude CLI as one long lived process, taking turns on its stdin.
//
// The confinement recipe is not built here - it lives in assets/assist/launch-claude.sh, which
// this only locates and runs. Keeping it in a script means the sandbox flags can be reviewed and
// re-verified without a rebuild, and that the panel cannot accidentally run the CLI unconfined.
#pragma once

#include <QByteArray>
#include <QProcess>
#include <QString>

#include "integration/AssistBackend.h"

namespace bda::integration

{

class ClaudeCliBackend : public AssistBackend
{
  Q_OBJECT

public:
  explicit ClaudeCliBackend(QObject *parent = nullptr);
  ~ClaudeCliBackend() override;

  void    start() override;
  void    send(const QString &message) override;
  void    cancel() override;
  bool    running() const override;
  QString unavailableReason() const override;

  //!< Directory the session treats as its working root. Defaults to the process's own.
  void setWorkingDirectory(const QString &path) { this->workingDirectory = path; }

  /* Read-only, or allowed to edit inside the working directory. Takes effect on the next session:
   * the mode decides how the sandbox is built, so an already running process cannot change it.
   */
  void       setMode(AssistMode newMode) { this->mode = newMode; }
  AssistMode currentMode() const { return this->mode; }

  /* Where launch-claude.sh is. Searched rather than fixed, because the deployed bundle puts it
   * next to the binary while a development build runs out of build/YUViewApp with the assets
   * still in the source tree. BDA_ASSIST_DIR overrides both.
   */
  static QString locateLauncher();

private:
  void drainStdout();
  void drainStderr();

  QProcess  *process{};
  QByteArray pending; //!< Partial line left over between reads; the CLI writes one event per line.
  QString    workingDirectory;
  QString    launcher;
  AssistMode mode{AssistMode::Full};
};

} // namespace bda::integration
