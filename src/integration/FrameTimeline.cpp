#include "FrameTimeline.h"

#include <QAbstractItemModel>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "playlistitem/playlistItem.h"

namespace bda::integration
{

namespace
{

constexpr int TextHeight = 16; // the line for the range summary above the bars

QColor typeColor(int type)
{
  switch (type)
  {
  case FrameTimeline::Key:
    return QColor(220, 50, 50);
  case FrameTimeline::IntraOnly:
    return QColor(240, 140, 30);
  case FrameTimeline::Inter:
    return QColor(60, 110, 220);
  case FrameTimeline::Switch:
    return QColor(150, 60, 200);
  case FrameTimeline::BiPred:
    return QColor(40, 170, 80);
  case FrameTimeline::ShowExisting:
    return QColor(120, 120, 120);
  }
  return QColor(150, 150, 150);
}

QString typeLabel(int type)
{
  switch (type)
  {
  case FrameTimeline::Key:
    return "KEY/I";
  case FrameTimeline::IntraOnly:
    return "INTRA_ONLY";
  case FrameTimeline::Inter:
    return "INTER/P";
  case FrameTimeline::Switch:
    return "SWITCH";
  case FrameTimeline::BiPred:
    return "B";
  case FrameTimeline::ShowExisting:
    return "show_existing";
  }
  return "?";
}

QString bytesText(qint64 bytes)
{
  if (bytes >= 10 * 1024 * 1024)
    return QString("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
  if (bytes >= 10 * 1024)
    return QString("%1 KB").arg(double(bytes) / 1024, 0, 'f', 1);
  return QString("%1 B").arg(bytes);
}

} // namespace

FrameTimeline::FrameTimeline(QWidget *parent) : QWidget(parent)
{
  this->setObjectName("frameTimeline");
  this->setMinimumHeight(70);
  this->setMouseTracking(true);
  this->typeRefresh.setInterval(1000);
  connect(&this->typeRefresh, &QTimer::timeout, this, &FrameTimeline::refreshTypes);
}

std::vector<FrameTimeline::Bar> FrameTimeline::barsOf(playlistItem *item) const
{
  std::vector<Bar> bars;
  if (!item)
    return bars;
  for (const auto &frame : item->getCodedFrames())
  {
    Bar bar;
    bar.bytes = frame.bytes;
    bar.type  = frame.key ? Key : Inter;
    bars.push_back(bar);
  }
  /* Bars must line up with the frame index playback uses. Sizes that do not (a cancelled scan,
   * frames the item does not list) are worse than none.
   */
  const auto range = item->properties().startEndRange;
  if (!bars.empty() && range.second >= 0 && int(bars.size()) != range.second + 1)
    bars.clear();
  if (bars.empty())
  {
    // Raw video: no sizes, but the frames are still there to click.
    for (int i = 0; range.second >= 0 && i <= range.second; ++i)
      bars.push_back({});
  }
  return bars;
}

void FrameTimeline::currentSelectedItemsChanged(playlistItem *item1, playlistItem *item2)
{
  if (item1 != this->itemA || item2 != this->itemB)
  {
    this->selection.reset();
    this->current = -1;
  }
  if (item1 != this->itemA)
  {
    // The parser's frame types belonged to the old item; the new model arrives on its own.
    this->obuIndex.reset();
    this->avcIndex.reset();
  }
  this->itemA = item1;
  this->itemB = item2;
  this->reload();
}

void FrameTimeline::reload()
{
  this->barsA = this->barsOf(this->itemA);
  this->barsB = this->barsOf(this->itemB);
  this->refreshTypes();
  this->update();
}

void FrameTimeline::setBitstreamModel(QAbstractItemModel *newModel)
{
  this->model = newModel;
  this->obuIndex.reset();
  this->avcIndex.reset();
  if (this->model)
    this->typeRefresh.start();
  else
    this->typeRefresh.stop();
  this->refreshTypes();
}

/* Frame types from the bitstream parser, as far as it has got. The model belongs to the first
 * selected item (BitstreamAnalysisWidget parses only that one), so only its bars are upgraded.
 */
void FrameTimeline::refreshTypes()
{
  if (!this->model)
  {
    this->typeRefresh.stop();
    return;
  }
  this->obuIndex.update(*this->model);
  this->avcIndex.update(*this->model);
  bool changed = false;
  for (int i = 0; i < int(this->barsA.size()); ++i)
  {
    auto &bar  = this->barsA[i];
    int   type = bar.type;
    if (const auto *obu = this->obuIndex.frame(i))
    {
      if (obu->showExisting)
        type = ShowExisting;
      else if (obu->frameType >= 0 && obu->frameType <= 3)
        type = std::array<int, 4>{Key, Inter, IntraOnly, Switch}[obu->frameType];
    }
    else if (const auto *avc = this->avcIndex.frame(i))
    {
      const auto &slices = avc->sliceTypes;
      const auto  has    = [&](const char *t) {
        return std::find(slices.begin(), slices.end(), t) != slices.end();
      };
      type = (avc->idr || (has("I") && !has("P") && !has("B"))) ? Key : has("B") ? BiPred : Inter;
    }
    changed |= type != bar.type;
    bar.type = type;
  }
  if (this->obuIndex.parsedFrames() >= int(this->barsA.size()) ||
      this->avcIndex.parsedFrames() >= int(this->barsA.size()))
    this->typeRefresh.stop();
  if (changed)
    this->update();
}

void FrameTimeline::setCurrentFrame(int frameIdx)
{
  if (frameIdx == this->current)
    return;
  this->current = frameIdx;
  this->update();
}

void FrameTimeline::selectRange(int first, int last)
{
  if (this->barsA.empty())
    return;
  first = std::clamp(first, 0, int(this->barsA.size()) - 1);
  last  = std::clamp(last, 0, int(this->barsA.size()) - 1);
  this->selection = std::make_pair(std::min(first, last), std::max(first, last));
  this->update();
}

void FrameTimeline::clearSelection()
{
  this->selection.reset();
  this->update();
}

void FrameTimeline::setLogScale(bool on)
{
  this->log = on;
  this->update();
}

std::optional<FrameTimeline::Summary> FrameTimeline::selectionSummary() const
{
  if (!this->selection || this->barsA.empty())
    return std::nullopt;
  Summary s;
  s.first = this->selection->first;
  s.last  = std::min(this->selection->second, int(this->barsA.size()) - 1);
  std::map<int, std::pair<int, qint64>> perType; // count, bytes
  qint64 bytesB = 0;
  bool   haveB  = !this->barsB.empty();
  for (int i = s.first; i <= s.last; ++i)
  {
    const auto &bar = this->barsA[i];
    if (bar.bytes < 0)
      continue;
    s.bytes += bar.bytes;
    perType[bar.type].first++;
    perType[bar.type].second += bar.bytes;
    if (s.largest < 0 || bar.bytes > this->barsA[s.largest].bytes)
      s.largest = i;
    if (s.smallest < 0 || bar.bytes < this->barsA[s.smallest].bytes)
      s.smallest = i;
    if (haveB && i < int(this->barsB.size()) && this->barsB[i].bytes >= 0)
      bytesB += this->barsB[i].bytes;
    else
      haveB = false;
  }
  // Bitrate over the frames on screen: a temporal unit is one displayed frame.
  const double fps    = this->itemA ? this->itemA->properties().frameRate : 0.0;
  const int    frames = s.last - s.first + 1;
  if (fps > 0 && frames > 0)
    s.kbps = double(s.bytes) * 8.0 * fps / frames / 1000.0;
  QStringList types;
  for (const auto &[type, countBytes] : perType)
    types.append(QString("%1 %2 (%3)")
                     .arg(typeLabel(type))
                     .arg(countBytes.first)
                     .arg(bytesText(countBytes.second)));
  s.perType = types.join(", ");
  if (haveB && s.bytes > 0)
    s.diffPercentB = 100.0 * double(bytesB - s.bytes) / double(s.bytes);
  return s;
}

QRect FrameTimeline::barArea() const
{
  return this->rect().adjusted(2, TextHeight + 2, -2, -2);
}

int FrameTimeline::frameAt(int x) const
{
  if (this->barsA.empty())
    return -1;
  const auto area = this->barArea();
  const int  n    = int(this->barsA.size());
  return std::clamp(int(double(x - area.left()) * n / std::max(1, area.width())), 0, n - 1);
}

void FrameTimeline::paintEvent(QPaintEvent *)
{
  QPainter painter(this);
  painter.fillRect(this->rect(), this->palette().base());
  const auto area = this->barArea();
  const int  n    = int(this->barsA.size());
  painter.setPen(this->palette().text().color());
  if (n == 0)
  {
    painter.drawText(area, Qt::AlignCenter, "Open an item to see its frames here");
    return;
  }

  qint64 maxBytes = 1;
  for (const auto *bars : {&this->barsA, &this->barsB})
    for (const auto &bar : *bars)
      maxBytes = std::max(maxBytes, bar.bytes);
  const auto height = [&](qint64 bytes) {
    if (bytes < 0)
      return area.height() / 2;
    const double f = this->log ? std::log1p(double(bytes)) / std::log1p(double(maxBytes))
                               : double(bytes) / double(maxBytes);
    return std::max(1, int(f * area.height()));
  };
  const double w = double(area.width()) / n;

  if (this->selection)
  {
    const int x0 = area.left() + int(this->selection->first * w);
    const int x1 = area.left() + int((this->selection->second + 1) * w);
    painter.fillRect(QRect(x0, area.top(), std::max(1, x1 - x0), area.height()),
                     this->palette().highlight().color().lighter(160));
  }
  for (int i = 0; i < n; ++i)
  {
    const auto &bar = this->barsA[i];
    const int   x   = area.left() + int(i * w);
    const int   bw  = std::max(1, int((i + 1) * w) - int(i * w) - (w > 4 ? 1 : 0));
    const int   h   = bar.type == ShowExisting ? std::max(2, area.height() / 10) : height(bar.bytes);
    painter.fillRect(QRect(x, area.bottom() - h + 1, bw, h), typeColor(bar.type));
  }
  if (!this->barsB.empty())
  {
    painter.setPen(QPen(this->palette().text().color(), 1));
    painter.setBrush(Qt::NoBrush);
    for (int i = 0; i < int(this->barsB.size()) && i < n; ++i)
    {
      const int h = height(this->barsB[i].bytes);
      painter.drawRect(QRect(area.left() + int(i * w), area.bottom() - h + 1, std::max(1, int(w) - 1), h));
    }
  }
  if (this->current >= 0 && this->current < n)
  {
    const int x = area.left() + int((this->current + 0.5) * w);
    painter.setPen(QPen(QColor(255, 20, 147), 2)); // the block selection's pink
    painter.drawLine(x, area.top(), x, area.bottom());
  }

  painter.setPen(this->palette().text().color());
  const QRect textRect(4, 0, this->width() - 8, TextHeight);
  if (const auto s = this->selectionSummary())
  {
    auto text = QString("Frames %1-%2: %3").arg(s->first).arg(s->last).arg(bytesText(s->bytes));
    if (s->kbps > 0)
      text += QString(", %1 kbps").arg(s->kbps, 0, 'f', 1);
    if (s->largest >= 0)
      text += QString(" | max #%1 %2, min #%3 %4")
                  .arg(s->largest)
                  .arg(bytesText(this->barsA[s->largest].bytes))
                  .arg(s->smallest)
                  .arg(bytesText(this->barsA[s->smallest].bytes));
    if (s->diffPercentB)
      text += QString(" | B %1%2%").arg(*s->diffPercentB >= 0 ? "+" : "").arg(*s->diffPercentB, 0, 'f', 1);
    text += " | " + s->perType;
    painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
  }
  else
    painter.drawText(textRect,
                     Qt::AlignVCenter | Qt::AlignLeft,
                     QString("%1 frames%2 - click to go to a frame, Shift+drag to add up a range")
                         .arg(n)
                         .arg(this->log ? " (log scale)" : ""));
}

void FrameTimeline::mousePressEvent(QMouseEvent *event)
{
  const int frame = this->frameAt(int(event->position().x()));
  if (frame < 0 || event->button() != Qt::LeftButton)
    return QWidget::mousePressEvent(event);
  if (event->modifiers() & Qt::ShiftModifier)
  {
    this->dragStart = frame;
    this->selectRange(frame, frame);
    return;
  }
  emit seekRequested(frame);
}

void FrameTimeline::mouseMoveEvent(QMouseEvent *event)
{
  if (this->dragStart >= 0 && (event->buttons() & Qt::LeftButton))
    this->selectRange(this->dragStart, this->frameAt(int(event->position().x())));
}

void FrameTimeline::mouseReleaseEvent(QMouseEvent *event)
{
  this->dragStart = -1;
  QWidget::mouseReleaseEvent(event);
}

void FrameTimeline::contextMenuEvent(QContextMenuEvent *event)
{
  QMenu menu(this);
  auto *logAction = menu.addAction("Log Scale");
  logAction->setCheckable(true);
  logAction->setChecked(this->log);
  connect(logAction, &QAction::toggled, this, &FrameTimeline::setLogScale);
  auto *clear = menu.addAction("Clear Range Selection");
  clear->setEnabled(this->selection.has_value());
  connect(clear, &QAction::triggered, this, &FrameTimeline::clearSelection);
  menu.exec(event->globalPos());
}

bool FrameTimeline::event(QEvent *event)
{
  if (event->type() == QEvent::ToolTip)
  {
    auto      *help  = static_cast<QHelpEvent *>(event);
    const int  frame = this->frameAt(help->pos().x());
    if (frame >= 0)
    {
      const auto &bar  = this->barsA[frame];
      auto        text = QString("Frame %1: %2").arg(frame).arg(typeLabel(bar.type));
      if (bar.bytes >= 0)
        text += ", " + bytesText(bar.bytes);
      if (frame < int(this->barsB.size()) && this->barsB[frame].bytes >= 0)
        text += QString("\nB: %1").arg(bytesText(this->barsB[frame].bytes));
      QToolTip::showText(help->globalPos(), text, this);
    }
    return true;
  }
  return QWidget::event(event);
}

} // namespace bda::integration
