// The assistant dock: a question box, the answer as it streams in, and - deliberately - the exact
// text that was sent.
//
// Showing what was sent is not a debug affordance. The model answers from numbers this panel
// hands it, so a wrong number produces a confident wrong answer with nothing on screen to
// contradict it. The sent block is the only thing that lets a user catch that.
#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

#include "assist/AssistContext.h"
#include "integration/AssistBackend.h"

class QCheckBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace bda::integration
{

class AssistPanelWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AssistPanelWidget(QWidget *parent = nullptr);

public slots:
  //!< The item on screen. Empty path clears it, which is how "nothing open" is expressed.
  void setStream(const QString &path,
                 int            width,
                 int            height,
                 int            superblockSize,
                 const QString &decoder,
                 const QString &originalPath);
  void setCurrentFrame(int frameIdx);
  //!< The superblock the user clicked, in pixels. `valid` false clears the selection.
  void setSelectedPosition(const QPoint &pixelPos, bool valid);

private:
  void        sendQuestion();
  void        handleEvent(const AssistEvent &event);
  void        appendLog(const QString &text);
  void        refreshSentPreview();
  QString     composeMessage(const QString &question) const;
  assist::AssistContext attachedContext() const;

  AssistBackend *backend{};

  QLabel         *status{};
  QPlainTextEdit *log{};
  QGroupBox      *sentBox{};
  QPlainTextEdit *sentPreview{};
  QCheckBox      *attachStream{};
  QCheckBox      *attachFrame{};
  QCheckBox      *attachSuperblock{};
  QPlainTextEdit *input{};
  QPushButton    *sendButton{};
  QPushButton    *stopButton{};

  assist::AssistContext context;
  int                   superblockGrid{};  //!< Cached so pixel -> grid needs no item lookup.
  bool                  havePosition{};
  QPoint                selectedPixel;

  /* Set when a session came up holding a tool it should not have. Sending is refused while it is
   * set, because the interesting failure here is silent: MCP servers attach asynchronously, so a
   * session can look read-only on its first turn and not be on its second.
   */
  QString toolWarning;

  bool    streaming{};
  double  lastCostUsd{};
  QString sessionId;
};

} // namespace bda::integration

/* Same reason as MotionEstimationWidget: uic writes the member declaration for a promoted widget
 * using the bare class name, so the form generated from mainwindow.ui says `AssistPanelWidget *`.
 * The alias keeps the class in its namespace instead of flattening it to global scope.
 */
using AssistPanelWidget = bda::integration::AssistPanelWidget;
