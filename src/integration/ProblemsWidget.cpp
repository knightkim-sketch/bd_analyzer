#include "ProblemsWidget.h"

#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace bda::integration
{

namespace
{

enum Column
{
  ColSeverity,
  ColFrame,
  ColWhere,
  ColSource,
  ColMessage
};

// Data roles on the first column: where a row points.
constexpr int RoleFrame  = Qt::UserRole;
constexpr int RolePixelX = Qt::UserRole + 1;
constexpr int RolePixelY = Qt::UserRole + 2;

constexpr int    MiSize      = 4;   // the AV1 mode-info unit, as in StreamDiffWindow
constexpr size_t MaxSbRows   = 200; // a broken stream can differ in every SB of a frame
const QString    SourceDiff  = "Find diff";
const QString    SourceDecoder = "Decoder";

QString firstElements(const bda::diff::SyntaxDiffResult &result, std::size_t max)
{
  QStringList parts;
  for (std::size_t i = 0; i < result.diffs.size() && i < max; ++i)
  {
    const auto &d = result.diffs[i];
    parts.append(QString("%1: %2 vs %3")
                     .arg(QString::fromStdString(d.name),
                          d.valueA.empty() ? "-" : QString::fromStdString(d.valueA),
                          d.valueB.empty() ? "-" : QString::fromStdString(d.valueB)));
  }
  if (result.diffs.size() > max)
    parts.append(QString("... %1 more").arg(result.diffs.size() - max));
  return parts.join("; ");
}

} // namespace

ProblemsWidget::ProblemsWidget(QWidget *parent) : QWidget(parent)
{
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  this->summary = new QLabel(this);
  this->summary->setMargin(4);
  layout->addWidget(this->summary);
  this->list = new QTreeWidget(this);
  this->list->setObjectName("problemsList");
  this->list->setRootIsDecorated(false);
  this->list->setUniformRowHeights(true);
  this->list->setHeaderLabels({"Severity", "Frame", "Where", "Source", "Message"});
  this->list->header()->setStretchLastSection(true);
  this->list->setColumnWidth(ColSeverity, 70);
  this->list->setColumnWidth(ColFrame, 60);
  this->list->setColumnWidth(ColWhere, 110);
  this->list->setColumnWidth(ColSource, 80);
  layout->addWidget(this->list);

  connect(this->list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *row) {
    const auto frame = row->data(ColSeverity, RoleFrame).toInt();
    if (frame < 0)
      return;
    StreamDiffLocation where;
    where.frameIdx = frame;
    const auto x = row->data(ColSeverity, RolePixelX);
    if (x.isValid())
      where.pixel = QPoint(x.toInt(), row->data(ColSeverity, RolePixelY).toInt());
    emit locateRequested(where);
  });
  this->updateSummary();
}

int ProblemsWidget::count() const
{
  return this->list->topLevelItemCount();
}

QTreeWidgetItem *ProblemsWidget::addRow(const QString       &severity,
                                        int                  frameIdx,
                                        const QString       &where,
                                        const QString       &source,
                                        const QString       &message,
                                        std::optional<QPoint> pixel)
{
  auto *row = new QTreeWidgetItem(
      {severity, frameIdx >= 0 ? QString::number(frameIdx) : QString("-"), where, source, message});
  row->setData(ColSeverity, RoleFrame, frameIdx);
  if (pixel)
  {
    row->setData(ColSeverity, RolePixelX, pixel->x());
    row->setData(ColSeverity, RolePixelY, pixel->y());
  }
  row->setToolTip(ColMessage, message);
  this->list->addTopLevelItem(row);
  return row;
}

void ProblemsWidget::removeSource(const QString &source)
{
  for (int i = this->list->topLevelItemCount() - 1; i >= 0; --i)
    if (this->list->topLevelItem(i)->text(ColSource) == source)
      delete this->list->takeTopLevelItem(i);
}

void ProblemsWidget::setDiffReport(const StreamDiffReport &report)
{
  this->removeSource(SourceDiff);

  // The first divergence first: headers, then payloads, then blocks and reconstruction.
  if (const auto *section = report.headers.firstDiffering())
  {
    const auto message = section->onlyInA   ? QString("only in A")
                         : section->onlyInB ? QString("only in B")
                                            : firstElements(section->result, 3);
    this->addRow("Diff",
                 -1,
                 QString::fromStdString(section->label),
                 SourceDiff,
                 QString("Header differs (%1 differences in all): %2")
                     .arg(report.headers.totalDiffs)
                     .arg(message),
                 std::nullopt);
  }
  if (report.payloadCompared && !report.payload.identical())
    this->addRow("Diff",
                 -1,
                 "OBU payload",
                 SourceDiff,
                 QString("%1 of %2 OBUs differ in their payload bytes")
                     .arg(report.payload.diffs.size())
                     .arg(report.payload.comparedObus),
                 std::nullopt);

  if (report.blockCompared)
  {
    const auto &step = report.blocks;
    std::size_t shown = 0;
    for (const auto &sb : step.differing)
    {
      if (!sb.differs())
        continue;
      if (++shown > MaxSbRows)
        break;
      QString               message;
      std::optional<QPoint> pixel;
      if (sb.hasBlockDiff())
      {
        const auto &blk = sb.blocks.front();
        pixel           = QPoint(blk.miCol * MiSize, blk.miRow * MiSize);
        message         = QString("%1 MI positions differ. MI(%2, %3): %4")
                      .arg(sb.differingPositions)
                      .arg(blk.miRow)
                      .arg(blk.miCol)
                      .arg(firstElements(blk.syntax, 3));
      }
      else
        message = "Superblock values differ: " + firstElements(sb.superblock, 3);
      if (!pixel && step.sbSize > 0)
        pixel = QPoint(sb.sbCol * step.sbSize, sb.sbRow * step.sbSize);
      this->addRow("Diff",
                   step.frameIdx,
                   QString("SB(%1, %2)").arg(sb.sbRow).arg(sb.sbCol),
                   SourceDiff,
                   message,
                   pixel);
    }
    if (shown > MaxSbRows)
      this->addRow("Info",
                   step.frameIdx,
                   "",
                   SourceDiff,
                   QString("Only the first %1 differing superblocks are listed").arg(MaxSbRows),
                   std::nullopt);
  }

  if (report.reconCompared)
    for (const auto &frame : report.recon.frames)
    {
      if (frame.diff.identical() || frame.diff.planes.empty())
        continue;
      const auto           &luma = frame.diff.planes.front();
      std::optional<QPoint> pixel;
      if (luma.firstX && luma.firstY)
        pixel = QPoint(*luma.firstX, *luma.firstY);
      std::uint64_t differing = 0;
      for (const auto &plane : frame.diff.planes)
        differing += plane.differingSamples;
      this->addRow("Diff",
                   frame.frameIdx,
                   pixel ? QString("pixel (x, y) = (%1, %2)").arg(pixel->x()).arg(pixel->y())
                         : QString("chroma"),
                   SourceDiff,
                   QString("Reconstruction differs in %1 samples").arg(differing),
                   pixel);
    }

  this->updateSummary();
}

void ProblemsWidget::setDecoderError(const void *item, const QString &itemName, const QString &error)
{
  const auto known = this->decoderErrors.find(item);
  if (known != this->decoderErrors.end() ? known->second == error : error.isEmpty())
    return;
  if (const auto row = this->decoderRows.find(item); row != this->decoderRows.end())
  {
    delete row->second;
    this->decoderRows.erase(row);
  }
  this->decoderErrors[item] = error;
  if (!error.isEmpty())
    this->decoderRows[item] =
        this->addRow("Error", -1, itemName, SourceDecoder, error.trimmed(), std::nullopt);
  this->updateSummary();
}

void ProblemsWidget::keepDecoderErrorsOf(const std::vector<const void *> &items)
{
  for (auto it = this->decoderErrors.begin(); it != this->decoderErrors.end();)
  {
    if (std::find(items.begin(), items.end(), it->first) != items.end())
    {
      ++it;
      continue;
    }
    if (const auto row = this->decoderRows.find(it->first); row != this->decoderRows.end())
    {
      delete row->second;
      this->decoderRows.erase(row);
    }
    it = this->decoderErrors.erase(it);
  }
  this->updateSummary();
}

void ProblemsWidget::updateSummary()
{
  const auto n = this->list->topLevelItemCount();
  this->summary->setText(n == 0 ? QString("No problems. Find diff results and decoder errors are "
                                          "listed here; double-click a row to go there.")
                                : QString("%1 entries - double-click a row to go there").arg(n));
}

} // namespace bda::integration
