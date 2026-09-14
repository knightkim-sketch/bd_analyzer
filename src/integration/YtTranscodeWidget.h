// The YouTube transcode pane: a list of links, an encoder, and a run of ffmpeg per link.
//
// The download and the encode are not done here. They are done by assets/yt-transcode/
// yt-transcode.sh, which this only drives - that script already solves the part that is easy to
// get wrong: yt-dlp extracts stream URLs using a spoofed client, googlevideo checks the
// User-Agent against the client that signed the URL, and anything else gets a 403. Reimplementing
// that in C++ would be reimplementing a bug.
#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

#include "yt/LinkList.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace bda::integration
{

class YtTranscodeWidget : public QWidget
{
  Q_OBJECT

public:
  explicit YtTranscodeWidget(QWidget *parent = nullptr);
  ~YtTranscodeWidget() override;

  /* Where yt-transcode.sh is. Searched the way the assistant launcher is: next to the binary in a
   * deployed bundle, in the source tree for a development build, overridable by environment.
   */
  static QString locateScript();

  //!< The encoders this ffmpeg actually has, as the script's -e names. Offering one it lacks only
  //!< produces a failure several minutes into a download.
  static QStringList availableEncoders();

private:
  void loadLinkFile(const QString &path);
  void saveLinkFile();
  void refreshLinkList();
  void addLink();
  void removeSelectedLinks();
  void startTranscode();
  void stopTranscode();
  void appendLog(const QString &text);
  QString unavailableReason() const;

  QLineEdit      *linkFileEdit{};
  QListWidget    *linkList{};
  QLineEdit      *newLinkEdit{};
  QComboBox      *encoderBox{};
  QSpinBox       *crfBox{};
  QComboBox      *heightBox{};
  QLineEdit      *outDirEdit{};
  QPushButton    *startButton{};
  QPushButton    *stopButton{};
  QProgressBar   *progress{};
  QLabel         *status{};
  QPlainTextEdit *log{};

  /* The whole file, not just the links: comments and blank lines are kept so that saving after an
   * edit does not throw away the documentation the shipped list is mostly made of.
   */
  std::vector<yt::LinkEntry> entries;
  QString                    linkFilePath;

  QProcess *process{};
  int       queued{};
};

} // namespace bda::integration

/* uic writes the member declaration for a promoted widget using the bare class name - same reason
 * as MotionEstimationWidget and AssistPanelWidget.
 */
using YtTranscodeWidget = bda::integration::YtTranscodeWidget;
