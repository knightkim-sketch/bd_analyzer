#include "integration/Mp4ContainerWidget.h"

#include <algorithm>

#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace bda::integration
{

namespace
{

/* Extensions that are ISO base media files. `.mkv` and `.webm` are Matroska and have nothing to do
 * with boxes, so they are deliberately absent - showing an empty box tree for them would read as a
 * parser failure rather than as "wrong container".
 */
bool looksLikeIsoBmff(const QString &path)
{
  static const QStringList extensions{"mp4", "m4v", "mov", "m4a", "3gp", "3g2", "mj2", "f4v"};
  return extensions.contains(QFileInfo(path).suffix().toLower());
}

/* Filling a row per sample is fine for a one second clip and not fine for a feature film, so the
 * list is capped and says that it is. The parser still located every sample; this is only what is
 * drawn.
 */
constexpr int kMaxSampleRows = 5000;

QString describeBox(const container::Mp4Box &box)
{
  QStringList notes;
  if (box.headerSize == 16)
    notes << QStringLiteral("64-bit size");
  if (box.extendsToEnd)
    notes << QStringLiteral("extends to end of file");
  if (!box.children.empty())
    notes << QStringLiteral("%1 children").arg(box.children.size());
  return notes.join(QStringLiteral(", "));
}

} // namespace

Mp4ContainerWidget::Mp4ContainerWidget(QWidget *parent) : QWidget(parent)
{
  auto *outer = new QVBoxLayout(this);

  this->summary = new QLabel(this);
  this->summary->setWordWrap(true);
  this->summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  outer->addWidget(this->summary);

  this->boxTree = new QTreeWidget(this);
  this->boxTree->setColumnCount(4);
  this->boxTree->setHeaderLabels({tr("Box"), tr("Offset"), tr("Size"), tr("Notes")});
  this->boxTree->setColumnWidth(0, 220);
  this->boxTree->setAlternatingRowColors(true);
  outer->addWidget(this->boxTree, 3);

  this->sampleTree = new QTreeWidget(this);
  this->sampleTree->setColumnCount(5);
  this->sampleTree->setHeaderLabels(
      {tr("Sample"), tr("Offset"), tr("Size"), tr("Sync"), tr("Decode time")});
  this->sampleTree->setColumnWidth(0, 90);
  this->sampleTree->setAlternatingRowColors(true);
  this->sampleTree->setRootIsDecorated(false);
  outer->addWidget(this->sampleTree, 2);

  this->clearFile();
}

void Mp4ContainerWidget::clearFile()
{
  this->shownPath.clear();
  this->boxTree->clear();
  this->sampleTree->clear();
  this->summary->setText(tr("Select an MP4 (or MOV / M4V) item to see its box structure. Other "
                            "containers are not ISO base media files and have no boxes."));
}

void Mp4ContainerWidget::setFile(const QString &path)
{
  if (path == this->shownPath)
    return;

  this->boxTree->clear();
  this->sampleTree->clear();
  this->shownPath = path;

  if (path.isEmpty())
  {
    this->clearFile();
    return;
  }
  if (!looksLikeIsoBmff(path))
  {
    this->summary->setText(
        tr("%1 is not an ISO base media file, so it has no box structure.").arg(path));
    return;
  }

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    this->summary->setText(tr("Could not open %1: %2").arg(path, file.errorString()));
    return;
  }

  /* Mapped, not read: mdat is the entire coded video and the box walk never touches it. Reading
   * the file would turn looking at headers into a copy of the whole clip.
   */
  const auto  fileSize = file.size();
  const auto *mapped   = file.map(0, fileSize);
  if (mapped == nullptr)
  {
    this->summary->setText(tr("Could not map %1 into memory (%2 bytes): %3")
                               .arg(path)
                               .arg(fileSize)
                               .arg(file.errorString()));
    return;
  }

  const auto parsed =
      container::parseMp4Boxes(mapped, static_cast<std::size_t>(fileSize));
  const auto tracks =
      container::readMp4Tracks(mapped, static_cast<std::size_t>(fileSize), parsed);

  this->fillBoxTree(parsed.boxes, nullptr);
  this->boxTree->expandToDepth(2);
  this->fillSamples(tracks);

  QStringList lines;
  lines << tr("%1 — %2 bytes, %3 top level boxes, %4 track(s)")
               .arg(QFileInfo(path).fileName())
               .arg(fileSize)
               .arg(parsed.boxes.size())
               .arg(tracks.size());

  for (const auto &track : tracks)
  {
    const auto syncs = std::count_if(track.samples.begin(),
                                     track.samples.end(),
                                     [](const container::Mp4Sample &s) { return s.sync; });
    lines << tr("Track %1: %2 %3, %4x%5, %6 samples, %7 sync, av1C config %8 bytes")
                 .arg(track.id)
                 .arg(QString::fromStdString(track.handlerType),
                      QString::fromStdString(track.sampleFormat))
                 .arg(track.width)
                 .arg(track.height)
                 .arg(track.samples.size())
                 .arg(syncs)
                 .arg(track.av1ConfigObus.size());
  }

  /* Errors go last and are not swallowed. A truncated capture is the common case here and its
   * moov is still worth reading, so the tree stays on screen next to the reason.
   */
  if (!parsed.ok())
    lines << tr("Box structure: %1%2")
                 .arg(QString::fromStdString(parsed.error),
                      parsed.truncated ? tr(" (file appears truncated)") : QString());
  for (const auto &track : tracks)
    if (!track.error.empty())
      lines << tr("Track %1: %2").arg(track.id).arg(QString::fromStdString(track.error));

  this->summary->setText(lines.join(QStringLiteral("\n")));
  file.unmap(const_cast<uchar *>(mapped));
}

void Mp4ContainerWidget::fillBoxTree(const std::vector<container::Mp4Box> &boxes,
                                     QTreeWidgetItem                     *parent)
{
  for (const auto &box : boxes)
  {
    const QStringList columns{QString::fromStdString(box.type),
                              QString::number(box.offset),
                              QString::number(box.size),
                              describeBox(box)};

    auto *item = parent == nullptr ? new QTreeWidgetItem(this->boxTree, columns)
                                   : new QTreeWidgetItem(parent, columns);
    this->fillBoxTree(box.children, item);
  }
}

void Mp4ContainerWidget::fillSamples(const std::vector<container::Mp4Track> &tracks)
{
  for (const auto &track : tracks)
  {
    int drawn = 0;
    for (std::size_t index = 0; index < track.samples.size(); ++index)
    {
      if (drawn >= kMaxSampleRows)
      {
        new QTreeWidgetItem(this->sampleTree,
                            QStringList{tr("..."),
                                        tr("%1 more samples not listed")
                                            .arg(track.samples.size() - std::size_t(drawn)),
                                        {},
                                        {},
                                        {}});
        break;
      }

      const auto &sample = track.samples[index];
      new QTreeWidgetItem(this->sampleTree,
                          QStringList{QString::number(index + 1),
                                      QString::number(sample.offset),
                                      QString::number(sample.size),
                                      sample.sync ? tr("yes") : QString(),
                                      QString::number(sample.decodeTime)});
      ++drawn;
    }
  }
}

} // namespace bda::integration
