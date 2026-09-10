#include "integration/AssistPanelWidget.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

#include "integration/ClaudeCliBackend.h"

namespace bda::integration
{

namespace
{
//!< Small enough that the answer keeps most of the panel, large enough for a real question.
constexpr int kInputHeight   = 70;
constexpr int kPreviewHeight = 150;

std::string toStd(const QString &text)
{
  return text.toStdString();
}
} // namespace

AssistPanelWidget::AssistPanelWidget(QWidget *parent) : QWidget(parent)
{
  auto *outer = new QVBoxLayout(this);

  this->status = new QLabel(this);
  this->status->setWordWrap(true);
  outer->addWidget(this->status);

  this->log = new QPlainTextEdit(this);
  this->log->setReadOnly(true);
  outer->addWidget(this->log, 3);

  /* Collapsed by default: it is a check the user reaches for when an answer looks wrong, not
   * something to read every turn.
   */
  this->sentBox = new QGroupBox(tr("What was sent"), this);
  this->sentBox->setCheckable(true);
  this->sentBox->setChecked(false);
  auto *sentLayout  = new QVBoxLayout(this->sentBox);
  this->sentPreview = new QPlainTextEdit(this->sentBox);
  this->sentPreview->setReadOnly(true);
  this->sentPreview->setMaximumHeight(kPreviewHeight);
  sentLayout->addWidget(this->sentPreview);
  this->sentPreview->setVisible(false);
  connect(this->sentBox, &QGroupBox::toggled, this->sentPreview, &QWidget::setVisible);
  outer->addWidget(this->sentBox);

  auto *attachRow         = new QHBoxLayout();
  this->attachStream      = new QCheckBox(tr("Stream"), this);
  this->attachFrame       = new QCheckBox(tr("Frame"), this);
  this->attachSuperblock  = new QCheckBox(tr("Superblock"), this);
  for (auto *box : {this->attachStream, this->attachFrame, this->attachSuperblock})
  {
    box->setChecked(true);
    attachRow->addWidget(box);
    connect(box, &QCheckBox::toggled, this, [this]() { this->refreshSentPreview(); });
  }
  attachRow->addStretch();
  outer->addLayout(attachRow);

  this->input = new QPlainTextEdit(this);
  this->input->setPlaceholderText(tr("Ask about the stream, the frame, or the clicked superblock."));
  this->input->setMaximumHeight(kInputHeight);
  outer->addWidget(this->input);

  auto *buttonRow  = new QHBoxLayout();
  this->sendButton = new QPushButton(tr("Send"), this);
  this->stopButton = new QPushButton(tr("End session"), this);
  this->stopButton->setEnabled(false);
  buttonRow->addWidget(this->sendButton);
  buttonRow->addWidget(this->stopButton);
  buttonRow->addStretch();
  outer->addLayout(buttonRow);

  this->backend = new ClaudeCliBackend(this);
  connect(this->backend, &AssistBackend::assistEvent, this, &AssistPanelWidget::handleEvent);
  connect(this->backend, &AssistBackend::finished, this, [this](int) {
    this->streaming = false;
    this->stopButton->setEnabled(false);
    this->appendLog(tr("--- session ended ---"));
  });

  connect(this->sendButton, &QPushButton::clicked, this, &AssistPanelWidget::sendQuestion);
  connect(this->stopButton, &QPushButton::clicked, this, [this]() { this->backend->cancel(); });

  /* Say up front why the panel cannot work, the way the ME panel does when no original is
   * attached. A disabled Send with no explanation is the thing this repository keeps not doing.
   */
  if (const auto reason = this->backend->unavailableReason(); !reason.isEmpty())
  {
    this->status->setText(reason);
    this->sendButton->setEnabled(false);
    this->input->setEnabled(false);
  }
  else
  {
    this->status->setText(tr("Read-only. It can read and search files, but cannot change "
                             "anything. Ask a question to start a session."));
  }

  this->refreshSentPreview();
}

void AssistPanelWidget::setStream(const QString &path,
                                  int            width,
                                  int            height,
                                  int            superblockSize,
                                  const QString &decoder,
                                  const QString &originalPath)
{
  if (path.isEmpty())
  {
    this->context.stream.reset();
    this->superblockGrid = 0;
  }
  else
  {
    assist::StreamInfo stream;
    stream.path           = toStd(path);
    stream.width          = width;
    stream.height         = height;
    stream.superblockSize = superblockSize;
    stream.decoder        = toStd(decoder);
    stream.originalPath   = toStd(originalPath);
    this->context.stream  = stream;
    this->superblockGrid  = superblockSize;
  }
  this->refreshSentPreview();
}

void AssistPanelWidget::setCurrentFrame(int frameIdx)
{
  this->context.frameIdx = frameIdx;
  this->refreshSentPreview();
}

void AssistPanelWidget::setSelectedPosition(const QPoint &pixelPos, bool valid)
{
  this->havePosition  = valid;
  this->selectedPixel = pixelPos;
  this->refreshSentPreview();
}

assist::AssistContext AssistPanelWidget::attachedContext() const
{
  assist::AssistContext attached;

  if (this->attachStream->isChecked())
    attached.stream = this->context.stream;
  if (this->attachFrame->isChecked())
    attached.frameIdx = this->context.frameIdx;

  /* The grid position is derived here rather than carried in, because the superblock size is a
   * property of the stream and folding the pixel onto the grid with a stale size would name the
   * wrong block - the one mistake in this panel that produces a plausible answer about something
   * the user did not click.
   */
  if (this->attachSuperblock->isChecked() && this->havePosition && this->superblockGrid > 0)
  {
    assist::SuperblockInfo superblock;
    superblock.column  = this->selectedPixel.x() / this->superblockGrid;
    superblock.row     = this->selectedPixel.y() / this->superblockGrid;
    superblock.pixelX  = this->selectedPixel.x();
    superblock.pixelY  = this->selectedPixel.y();
    attached.superblock = superblock;
  }

  return attached;
}

QString AssistPanelWidget::composeMessage(const QString &question) const
{
  const auto rendered = QString::fromStdString(assist::renderContext(this->attachedContext()));
  return rendered + "\n## Question\n\n" + question + "\n";
}

void AssistPanelWidget::refreshSentPreview()
{
  this->sentPreview->setPlainText(this->composeMessage(this->input->toPlainText().isEmpty()
                                                           ? tr("<your question>")
                                                           : this->input->toPlainText()));
}

void AssistPanelWidget::sendQuestion()
{
  if (!this->toolWarning.isEmpty())
    return;

  const auto question = this->input->toPlainText().trimmed();
  if (question.isEmpty())
    return;

  const auto message = this->composeMessage(question);
  this->sentPreview->setPlainText(message);

  this->appendLog(QStringLiteral("> ") + question);
  this->log->appendPlainText(QString());
  this->input->clear();
  this->streaming = true;
  this->stopButton->setEnabled(true);
  this->backend->send(message);
}

void AssistPanelWidget::appendLog(const QString &text)
{
  this->log->appendPlainText(text);
}

void AssistPanelWidget::handleEvent(const AssistEvent &event)
{
  switch (event.kind)
  {
  case AssistEvent::Kind::SessionStarted:
  {
    this->sessionId = event.sessionId;
    if (const auto unexpected = unexpectedTools(event.tools); !unexpected.isEmpty())
    {
      /* Refuse rather than warn and continue. This fires when something outside our flags put a
       * tool in the session - the measured case is MCP servers from the user's configuration
       * arriving on a later turn with write access to external services.
       */
      this->toolWarning = tr("This session was granted tools it should not have: %1. Sending is "
                             "disabled. Check assets/assist/launch-claude.sh.")
                              .arg(unexpected.join(QStringLiteral(", ")));
      this->status->setText(this->toolWarning);
      this->sendButton->setEnabled(false);
      this->backend->cancel();
      return;
    }
    this->status->setText(tr("Session %1 - %2, %3 mode, tools: %4")
                              .arg(event.sessionId.left(8),
                                   event.model,
                                   event.permissionMode,
                                   event.tools.join(QStringLiteral(", "))));
    return;
  }
  case AssistEvent::Kind::TextDelta:
    /* Appended without a newline so the answer builds up as one paragraph; appendPlainText would
     * put every fragment on its own line.
     */
    this->log->moveCursor(QTextCursor::End);
    this->log->insertPlainText(event.text);
    return;

  case AssistEvent::Kind::TurnFinished:
    this->streaming   = false;
    this->lastCostUsd = event.costUsd;
    this->log->appendPlainText(QString());
    /* "usage" and not "spent": what this figure is depends on how the CLI is authenticated. On a
     * subscription login (apiKeySource "none", which is what a signed-in user has) nothing is
     * billed per question - the tokens draw down a rolling usage window, and the CLI reports their
     * list-price equivalent. With ANTHROPIC_API_KEY set it is a real charge. The panel cannot tell
     * the difference from here, so it must not claim one.
     */
    this->status->setText(tr("Session %1 - done. Token usage so far: $%2 equivalent "
                             "(billed only if this CLI uses an API key).")
                              .arg(this->sessionId.left(8))
                              .arg(event.costUsd, 0, 'f', 4));
    return;

  case AssistEvent::Kind::Failed:
    this->streaming = false;
    this->appendLog(tr("[error] ") + event.text);
    return;

  case AssistEvent::Kind::Ignored:
    return;
  }
}

} // namespace bda::integration
