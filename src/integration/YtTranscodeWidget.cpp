#include "integration/YtTranscodeWidget.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>

namespace bda::integration
{

namespace
{

//!< The script's -e name, and what to show for it. Order is the order the box offers.
struct EncoderChoice
{
  const char *scriptName;
  const char *label;
  const char *ffmpegEncoder; //!< What must exist in ffmpeg for this to work. Empty = always.
};

const std::vector<EncoderChoice> &encoderChoices()
{
  static const std::vector<EncoderChoice> choices{
      {"av1", "AV1 (libaom)", "libaom-av1"},
      {"svtav1", "AV1 (SVT-AV1)", "libsvtav1"},
      {"x265", "H.265 (x265)", "libx265"},
      {"x264", "H.264 (x264)", "libx264"},
      {"nvenc", "H.264 (NVENC)", "h264_nvenc"},
      {"copy", "Copy - no re-encode", ""},
  };
  return choices;
}

} // namespace

QString YtTranscodeWidget::locateScript()
{
  const auto name = QStringLiteral("yt-transcode.sh");

  if (const auto override = qEnvironmentVariable("BDA_YT_SCRIPT"); !override.isEmpty())
    return override;

  const auto        appDir = QCoreApplication::applicationDirPath();
  const QStringList candidates{
      QDir(appDir).filePath("yt-transcode/" + name),                 // deployed bundle
      QDir(appDir).filePath("../../assets/yt-transcode/" + name),    // build/YUViewApp -> repo
      QDir(appDir).filePath("../../../assets/yt-transcode/" + name),
  };
  for (const auto &candidate : candidates)
    if (QFileInfo::exists(candidate))
      return QFileInfo(candidate).canonicalFilePath();

  return {};
}

QStringList YtTranscodeWidget::availableEncoders()
{
  /* Ask ffmpeg rather than assume. This build has libaom-av1 and libx265 but no libsvtav1, and
   * offering an encoder that is not there costs the user a whole download before it fails.
   */
  QStringList present;

  QProcess ffmpeg;
  ffmpeg.start(QStringLiteral("ffmpeg"), {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
  if (!ffmpeg.waitForFinished(5000))
    return present;

  const auto output = QString::fromUtf8(ffmpeg.readAllStandardOutput()) +
                      QString::fromUtf8(ffmpeg.readAllStandardError());
  for (const auto &choice : encoderChoices())
  {
    const QString needed = QString::fromLatin1(choice.ffmpegEncoder);
    if (needed.isEmpty() || output.contains(needed))
      present << QString::fromLatin1(choice.scriptName);
  }
  return present;
}

YtTranscodeWidget::YtTranscodeWidget(QWidget *parent) : QWidget(parent)
{
  auto *outer = new QVBoxLayout(this);

  // --- the list file ------------------------------------------------------------------------
  auto *fileRow = new QHBoxLayout();
  fileRow->addWidget(new QLabel(tr("Link list:"), this));
  this->linkFileEdit = new QLineEdit(this);
  this->linkFileEdit->setPlaceholderText(tr("links.txt"));
  fileRow->addWidget(this->linkFileEdit, 1);
  auto *browse = new QPushButton(tr("Open..."), this);
  auto *save   = new QPushButton(tr("Save"), this);
  fileRow->addWidget(browse);
  fileRow->addWidget(save);
  outer->addLayout(fileRow);

  this->linkList = new QListWidget(this);
  this->linkList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  // The links differ only at the end, exactly like the playlist item names, so keep the tail.
  this->linkList->setTextElideMode(Qt::ElideMiddle);
  outer->addWidget(this->linkList, 3);

  auto *addRow      = new QHBoxLayout();
  this->newLinkEdit = new QLineEdit(this);
  this->newLinkEdit->setPlaceholderText(tr("Paste a YouTube URL or an 11 character video ID"));
  addRow->addWidget(this->newLinkEdit, 1);
  auto *add    = new QPushButton(tr("Add"), this);
  auto *remove = new QPushButton(tr("Remove"), this);
  addRow->addWidget(add);
  addRow->addWidget(remove);
  outer->addLayout(addRow);

  // --- encoding settings --------------------------------------------------------------------
  auto *settings = new QGridLayout();
  settings->addWidget(new QLabel(tr("Codec:"), this), 0, 0);
  this->encoderBox = new QComboBox(this);
  settings->addWidget(this->encoderBox, 0, 1);

  settings->addWidget(new QLabel(tr("CRF:"), this), 0, 2);
  this->crfBox = new QSpinBox(this);
  this->crfBox->setRange(0, 63); // AV1 goes to 63; H.264 stops at 51 and clamps in the encoder.
  this->crfBox->setValue(23);
  settings->addWidget(this->crfBox, 0, 3);

  settings->addWidget(new QLabel(tr("Max height:"), this), 0, 4);
  this->heightBox = new QComboBox(this);
  this->heightBox->addItems({"2160", "1440", "1080", "720", "480", "360"});
  this->heightBox->setCurrentText("1080");
  settings->addWidget(this->heightBox, 0, 5);

  settings->addWidget(new QLabel(tr("Output:"), this), 1, 0);
  this->outDirEdit = new QLineEdit(this);
  this->outDirEdit->setText(QDir::currentPath());
  settings->addWidget(this->outDirEdit, 1, 1, 1, 4);
  auto *browseOut = new QPushButton(tr("..."), this);
  settings->addWidget(browseOut, 1, 5);
  outer->addLayout(settings);

  // --- run ------------------------------------------------------------------------------------
  auto *runRow      = new QHBoxLayout();
  this->startButton = new QPushButton(tr("Download and encode"), this);
  this->stopButton  = new QPushButton(tr("Stop"), this);
  this->stopButton->setEnabled(false);
  this->progress = new QProgressBar(this);
  this->progress->setVisible(false);
  runRow->addWidget(this->startButton);
  runRow->addWidget(this->stopButton);
  runRow->addWidget(this->progress, 1);
  outer->addLayout(runRow);

  this->status = new QLabel(this);
  this->status->setWordWrap(true);
  outer->addWidget(this->status);

  this->log = new QPlainTextEdit(this);
  this->log->setReadOnly(true);
  outer->addWidget(this->log, 2);

  // --- wiring -----------------------------------------------------------------------------------
  connect(browse, &QPushButton::clicked, this, [this]() {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Open link list"), this->linkFilePath, tr("Text files (*.txt);;All files (*)"));
    if (!path.isEmpty())
      this->loadLinkFile(path);
  });
  connect(save, &QPushButton::clicked, this, &YtTranscodeWidget::saveLinkFile);
  connect(add, &QPushButton::clicked, this, &YtTranscodeWidget::addLink);
  connect(this->newLinkEdit, &QLineEdit::returnPressed, this, &YtTranscodeWidget::addLink);
  connect(remove, &QPushButton::clicked, this, &YtTranscodeWidget::removeSelectedLinks);
  connect(browseOut, &QPushButton::clicked, this, [this]() {
    const auto path =
        QFileDialog::getExistingDirectory(this, tr("Output folder"), this->outDirEdit->text());
    if (!path.isEmpty())
      this->outDirEdit->setText(path);
  });
  connect(this->startButton, &QPushButton::clicked, this, &YtTranscodeWidget::startTranscode);
  connect(this->stopButton, &QPushButton::clicked, this, &YtTranscodeWidget::stopTranscode);

  // --- what this machine can actually do -------------------------------------------------------
  for (const auto &choice : encoderChoices())
    if (availableEncoders().contains(QString::fromLatin1(choice.scriptName)))
      this->encoderBox->addItem(QString::fromLatin1(choice.label),
                                QString::fromLatin1(choice.scriptName));

  if (const auto reason = this->unavailableReason(); !reason.isEmpty())
  {
    this->status->setText(reason);
    this->startButton->setEnabled(false);
  }
  else
  {
    this->status->setText(tr("Ready. Each link is downloaded with yt-dlp and encoded with ffmpeg."));
  }

  // The list shipped beside the script is the example; load it if the user has not chosen one.
  if (const auto script = locateScript(); !script.isEmpty())
  {
    const auto beside = QDir(QFileInfo(script).absolutePath()).filePath("links.txt");
    if (QFileInfo::exists(beside))
      this->loadLinkFile(beside);
  }
}

YtTranscodeWidget::~YtTranscodeWidget()
{
  // A download and encode outlives the panel being closed unless it is stopped here.
  if (this->process && this->process->state() != QProcess::NotRunning)
  {
    this->process->terminate();
    if (!this->process->waitForFinished(3000))
      this->process->kill();
  }
}

QString YtTranscodeWidget::unavailableReason() const
{
  if (locateScript().isEmpty())
    return tr("yt-transcode.sh was not found next to the application. Set BDA_YT_SCRIPT to its "
              "path.");
  if (QStandardPaths::findExecutable(QStringLiteral("yt-dlp")).isEmpty())
    return tr("'yt-dlp' is not installed. It is what fetches the video; the encode cannot start "
              "without it.");
  if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
    return tr("'ffmpeg' is not on PATH.");
  if (this->encoderBox->count() == 0)
    return tr("This ffmpeg has none of the encoders the pane offers.");
  return {};
}

void YtTranscodeWidget::loadLinkFile(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    this->appendLog(tr("Could not open %1: %2").arg(path, file.errorString()));
    return;
  }

  this->entries      = yt::parseLinkList(QString::fromUtf8(file.readAll()).toStdString());
  this->linkFilePath = path;
  this->linkFileEdit->setText(path);
  this->refreshLinkList();
}

void YtTranscodeWidget::saveLinkFile()
{
  if (this->linkFilePath.isEmpty())
  {
    this->linkFilePath = QFileDialog::getSaveFileName(this, tr("Save link list"), "links.txt",
                                                      tr("Text files (*.txt)"));
    if (this->linkFilePath.isEmpty())
      return;
    this->linkFileEdit->setText(this->linkFilePath);
  }

  QFile file(this->linkFilePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
  {
    this->appendLog(tr("Could not write %1: %2").arg(this->linkFilePath, file.errorString()));
    return;
  }
  file.write(QString::fromStdString(yt::renderLinkList(this->entries)).toUtf8());
  this->appendLog(tr("Saved %1").arg(this->linkFilePath));
}

void YtTranscodeWidget::refreshLinkList()
{
  this->linkList->clear();
  for (const auto &link : yt::linksOnly(this->entries))
  {
    auto *item = new QListWidgetItem(QString::fromStdString(link), this->linkList);
    item->setToolTip(QString::fromStdString(link));
  }
  this->status->setText(tr("%1 link(s) in %2")
                            .arg(this->linkList->count())
                            .arg(QFileInfo(this->linkFilePath).fileName()));
}

void YtTranscodeWidget::addLink()
{
  const auto candidate = this->newLinkEdit->text().trimmed();
  if (candidate.isEmpty())
    return;

  /* Refused here rather than passed through, because a malformed id is handed to the script as an
   * id and fails several minutes later inside yt-dlp, by which point it is not obvious which line
   * was wrong.
   */
  if (!yt::looksLikeVideoLink(candidate.toStdString()))
  {
    this->appendLog(tr("Not a YouTube URL or an 11 character video ID: %1").arg(candidate));
    return;
  }

  yt::LinkEntry entry;
  entry.link = candidate.toStdString();
  this->entries.push_back(entry);
  this->newLinkEdit->clear();
  this->refreshLinkList();
}

void YtTranscodeWidget::removeSelectedLinks()
{
  const auto selected = this->linkList->selectedItems();
  if (selected.isEmpty())
    return;

  QStringList doomed;
  for (const auto *item : selected)
    doomed << item->text();

  /* Removed from the entry list by value, not by row: the rows show links only, while `entries`
   * holds the comments and blank lines too, so the indices do not line up.
   */
  this->entries.erase(std::remove_if(this->entries.begin(),
                                     this->entries.end(),
                                     [&doomed](const yt::LinkEntry &entry) {
                                       return entry.isLink() &&
                                              doomed.contains(QString::fromStdString(entry.link));
                                     }),
                      this->entries.end());
  this->refreshLinkList();
}

void YtTranscodeWidget::startTranscode()
{
  if (this->process && this->process->state() != QProcess::NotRunning)
    return;

  const auto links = yt::linksOnly(this->entries);
  if (links.empty())
  {
    this->appendLog(tr("The list has no links."));
    return;
  }

  /* The script is handed a list file, not one link at a time: it already knows how to skip an
   * existing output, keep going after a failure, and report per video. Writing the current list
   * to a temporary file is what lets the pane's unsaved edits be the thing that runs.
   */
  const auto listPath = QDir::temp().filePath(QStringLiteral("bd_analyzer_yt_links.txt"));
  QFile      listFile(listPath);
  if (!listFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
  {
    this->appendLog(tr("Could not write the temporary list %1").arg(listPath));
    return;
  }
  listFile.write(QString::fromStdString(yt::renderLinkList(this->entries)).toUtf8());
  listFile.close();

  const QStringList arguments{
      QStringLiteral("-l"),         listPath,
      QStringLiteral("-d"),         this->outDirEdit->text(),
      QStringLiteral("-e"),         this->encoderBox->currentData().toString(),
      QStringLiteral("-q"),         QString::number(this->crfBox->value()),
      QStringLiteral("-H"),         this->heightBox->currentText(),
      QStringLiteral("--skip-existing"),
  };

  this->queued = int(links.size());
  this->progress->setRange(0, 0); // The script reports per video; until then, show it is alive.
  this->progress->setVisible(true);
  this->startButton->setEnabled(false);
  this->stopButton->setEnabled(true);

  this->appendLog(tr("--- %1 link(s), encoder %2, crf %3, max %4p ---")
                      .arg(this->queued)
                      .arg(this->encoderBox->currentText())
                      .arg(this->crfBox->value())
                      .arg(this->heightBox->currentText()));

  this->process = new QProcess(this);
  this->process->setProcessChannelMode(QProcess::MergedChannels);
  connect(this->process, &QProcess::readyRead, this, [this]() {
    this->appendLog(QString::fromUtf8(this->process->readAll()).trimmed());
  });
  connect(this->process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
    this->progress->setVisible(false);
    this->startButton->setEnabled(true);
    this->stopButton->setEnabled(false);
    this->appendLog(code == 0 ? tr("--- finished ---")
                              : tr("--- finished with exit code %1 ---").arg(code));
  });

  this->process->start(locateScript(), arguments);
}

void YtTranscodeWidget::stopTranscode()
{
  if (!this->process || this->process->state() == QProcess::NotRunning)
    return;

  /* terminate first: the script traps it and stops between videos, which leaves the output of the
   * one already finished intact instead of a half written file.
   */
  this->appendLog(tr("Stopping after the current video..."));
  this->process->terminate();
  if (!this->process->waitForFinished(5000))
    this->process->kill();
}

void YtTranscodeWidget::appendLog(const QString &text)
{
  if (text.isEmpty())
    return;
  this->log->appendPlainText(text);
}

} // namespace bda::integration
