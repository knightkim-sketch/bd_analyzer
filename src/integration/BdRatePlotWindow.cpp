#include "BdRatePlotWindow.h"

#include <QCheckBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace bda::integration
{
namespace
{

/* One colour per group. Chosen to stay apart on both light and dark backgrounds, and to avoid the
 * red and blue the bitstream's own motion vectors already use elsewhere in the window.
 */
const std::vector<QColor> &groupColours()
{
  static const std::vector<QColor> colours{QColor(0, 150, 60),
                                           QColor(220, 130, 0),
                                           QColor(150, 60, 200),
                                           QColor(0, 140, 190),
                                           QColor(190, 60, 110),
                                           QColor(110, 120, 40)};
  return colours;
}

QString bdRateText(const std::optional<bdrate::BdRateResult> &result)
{
  if (!result)
    return "anchor";
  switch (result->status)
  {
  case bdrate::BdRateStatus::Ok:
    /* The degree is part of the number's meaning: the standard metric is a cubic over four points,
     * and a line over two is a slope comparison wearing the same units.
     */
    return QString("%1%2 %%3")
        .arg(result->percent >= 0.0 ? "+" : "")
        .arg(result->percent, 0, 'f', 2)
        .arg(result->degree < 3 ? QString(" (deg %1)").arg(result->degree) : QString());
  case bdrate::BdRateStatus::NoPsnrOverlap:
    return "no PSNR overlap";
  case bdrate::BdRateStatus::NotEnoughPoints:
    return "too few points";
  case bdrate::BdRateStatus::Degenerate:
    return "not computable";
  }
  return "-";
}

} // namespace

/* A round step for a linear axis: 1, 2 or 5 times a power of ten, whichever lands nearest the
 * requested number of divisions. Ticks at arbitrary fractions of the data range are readable only
 * by accident - 42.7, 45.1, 47.5 tells you less than 42, 44, 46 does.
 */
double niceAxisStep(double range, int divisions)
{
  if (!(range > 0.0) || divisions < 1)
    return 1.0;
  const auto raw        = range / divisions;
  const auto magnitude  = std::pow(10.0, std::floor(std::log10(raw)));
  const auto normalised = raw / magnitude;

  /* Rounded to the *nearest* round number, not up to the next one. Rounding up looks harmless and
   * is not: a 12 dB PSNR span asks for 2.4 and gets 5, which is two labels on the whole axis.
   * Measured on the render before this was fixed - 40 and 45, and nothing else.
   */
  double step = 10.0;
  if (normalised < 1.5)
    step = 1.0;
  else if (normalised < 3.5)
    step = 2.0;
  else if (normalised < 7.5)
    step = 5.0;
  return step * magnitude;
}

/* Tick positions for the rate axis, which is logarithmic.
 *
 * Stepping linearly in log space would label 10^3.4 and 10^3.7 - numbers nobody reads. These are
 * the round values a reader expects on a log scale (1, 2, 5, 10, 20, 50, ...) that fall inside the
 * visible range. Over a narrow range those three mantissas can yield too few ticks, so a second
 * pass fills in.
 */
std::vector<double> logAxisTicks(double logMin, double logMax)
{
  const auto collect = [logMin, logMax](const std::vector<double> &mantissas) {
    std::vector<double> ticks;
    const int           first = int(std::floor(logMin));
    const int           last  = int(std::ceil(logMax));
    for (int decade = first; decade <= last; ++decade)
      for (const auto mantissa : mantissas)
      {
        const auto value = mantissa * std::pow(10.0, decade);
        const auto position = std::log10(value);
        if (position >= logMin && position <= logMax)
          ticks.push_back(value);
      }
    return ticks;
  };

  auto ticks = collect({1.0, 2.0, 5.0});
  if (ticks.size() >= 3)
    return ticks;

  if (auto filled = collect({1.0, 1.5, 2.0, 3.0, 5.0, 7.0}); filled.size() >= 3)
    return filled;

  /* Still too few. That happens when the whole visible span sits between two round values -
   * 2100..4800 bits contains no 1/2/5 mantissa at all and would draw a single tick. Over a span
   * that narrow the axis is effectively linear, so round values *in bits* are both correct and
   * what a reader expects.
   */
  const auto low   = std::pow(10.0, logMin);
  const auto high  = std::pow(10.0, logMax);
  const auto step  = niceAxisStep(high - low, 4);
  ticks.clear();
  for (double value = std::ceil(low / step) * step; value <= high; value += step)
    ticks.push_back(value);
  return ticks;
}

//!< Short enough for a narrow panel: 6435 -> "6.44k", 1200000 -> "1.2M".
QString formatAxisRate(double value)
{
  if (value >= 1e6)
    return QString::number(value / 1e6, 'g', 3) + "M";
  if (value >= 1e3)
    return QString::number(value / 1e3, 'g', 3) + "k";
  return QString::number(value, 'g', 3);
}

// ---------------------------------------------------------------------------------------------
// BdRateCurvePanel
// ---------------------------------------------------------------------------------------------

BdRateCurvePanel::BdRateCurvePanel(const QString &title, QWidget *parent)
    : QWidget(parent), title(title)
{
  this->setMinimumSize(260, 220);
}

void BdRateCurvePanel::setCurves(std::vector<Curve> curves)
{
  this->curves = std::move(curves);
  this->message.clear();
  this->update();
}

void BdRateCurvePanel::setMessage(const QString &message)
{
  this->message = message;
  this->curves.clear();
  this->update();
}

void BdRateCurvePanel::paintEvent(QPaintEvent *)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.fillRect(this->rect(), this->palette().base());

  const auto pen = this->palette().text().color();
  painter.setPen(pen);
  painter.drawText(QRect(0, 2, this->width(), 16), Qt::AlignCenter, this->title);

  if (!this->message.isEmpty() || this->curves.empty())
  {
    painter.drawText(this->rect().adjusted(8, 20, -8, -8),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     this->message.isEmpty() ? "Nothing to draw." : this->message);
    return;
  }

  // Data range over every curve, so the panels can be read against each other.
  double xMin = 1e300, xMax = -1e300, yMin = 1e300, yMax = -1e300;
  for (const auto &curve : this->curves)
    for (const auto &point : curve.points)
    {
      const auto x = std::log10(point.rate);
      xMin         = std::min(xMin, x);
      xMax         = std::max(xMax, x);
      yMin         = std::min(yMin, point.psnr);
      yMax         = std::max(yMax, point.psnr);
    }
  if (!(xMax > xMin))
  {
    xMin -= 0.5;
    xMax += 0.5;
  }
  if (!(yMax > yMin))
  {
    yMin -= 1.0;
    yMax += 1.0;
  }
  // A margin so the outermost points are not drawn on the axis itself.
  const auto xPad = (xMax - xMin) * 0.08;
  const auto yPad = (yMax - yMin) * 0.08;
  xMin -= xPad;
  xMax += xPad;
  yMin -= yPad;
  yMax += yPad;

  const QRect plot(52, 22, this->width() - 64, this->height() - 64);

  const auto toPixel = [&](double rate, double psnr) {
    const auto x = (std::log10(rate) - xMin) / (xMax - xMin);
    const auto y = (psnr - yMin) / (yMax - yMin);
    return QPointF(plot.left() + x * plot.width(), plot.bottom() - y * plot.height());
  };

  /* Ticks before the curves, so the grid sits under the data rather than over it.
   *
   * Both axes are labelled in the reader's units, not the plotting ones: the rate axis is drawn in
   * log space but labelled with bits, because "3.8" on a log axis is not a number anyone can use.
   */
  auto gridColour = pen;
  gridColour.setAlpha(40);
  const QFontMetrics metrics(painter.font());
  constexpr int      kTick = 4;

  // --- rate, logarithmic ---------------------------------------------------------------------
  painter.setPen(pen);
  int lastLabelRight = plot.left() - 1000;
  for (const auto rate : logAxisTicks(xMin, xMax))
  {
    const auto x = toPixel(rate, yMin).x();

    painter.setPen(gridColour);
    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    painter.setPen(pen);
    painter.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + kTick));

    /* Skip a label that would collide with the one before it. A narrow panel can hold more ticks
     * than legible numbers, and overlapping text is worse than a bare tick.
     */
    const auto text  = formatAxisRate(rate);
    const auto width = metrics.horizontalAdvance(text);
    const auto left  = int(x) - width / 2;
    if (left > lastLabelRight + 6)
    {
      painter.drawText(QRect(left, plot.bottom() + kTick + 1, width, 12),
                       Qt::AlignHCenter | Qt::AlignTop,
                       text);
      lastLabelRight = left + width;
    }
  }

  // --- PSNR, linear ---------------------------------------------------------------------------
  const auto psnrStep = niceAxisStep(yMax - yMin, 5);
  for (double value = std::ceil(yMin / psnrStep) * psnrStep; value <= yMax; value += psnrStep)
  {
    const auto y = toPixel(std::pow(10.0, xMin), value).y();

    painter.setPen(gridColour);
    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    painter.setPen(pen);
    painter.drawLine(QPointF(plot.left() - kTick, y), QPointF(plot.left(), y));

    // One decimal only when the step needs it - "44" beats "44.0" in a 52 pixel margin.
    const auto text = QString::number(value, 'f', psnrStep < 1.0 ? 1 : 0);
    painter.drawText(QRect(0, int(y) - 7, plot.left() - kTick - 2, 14),
                     Qt::AlignRight | Qt::AlignVCenter,
                     text);
  }

  painter.setPen(pen);
  painter.drawLine(plot.bottomLeft(), plot.bottomRight());
  painter.drawLine(plot.topLeft(), plot.bottomLeft());
  // Below the tick labels, not level with them - at +16 the title sat in the same row and the
  // axis read "3k 5k bits + 1 (log) 7k 10k".
  painter.drawText(QRect(plot.left(), plot.bottom() + 20, plot.width(), 14),
                   Qt::AlignCenter,
                   "bits + 1 (log)");
  painter.save();
  painter.translate(11, plot.center().y());
  painter.rotate(-90);
  painter.drawText(QRect(-50, -10, 100, 14), Qt::AlignCenter, "PSNR (dB)");
  painter.restore();

  int legendRow = 0;
  for (std::size_t index = 0; index < this->curves.size(); ++index)
  {
    const auto &curve  = this->curves[index];
    const auto  colour = groupColours()[index % groupColours().size()];
    painter.setPen(QPen(colour, curve.isAnchor ? 2.4 : 1.4));

    /* The fitted polynomial, sampled - the same fit the BD-rate integrates. With fewer than two
     * points there is no fit, and the marker alone says what there is.
     */
    if (curve.points.size() >= 2)
    {
      auto sorted = curve.points;
      std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return a.psnr < b.psnr;
      });
      std::vector<double> psnrs, logRates;
      for (const auto &point : sorted)
      {
        psnrs.push_back(point.psnr);
        logRates.push_back(std::log10(point.rate));
      }
      const auto degree = std::min<int>(3, int(sorted.size()) - 1);
      const auto fit    = bdrate::polyFit(psnrs, logRates, degree);

      QPainterPath path;
      const int    steps = 48;
      for (int step = 0; step <= steps; ++step)
      {
        const auto psnr = sorted.front().psnr +
                          (sorted.back().psnr - sorted.front().psnr) * step / double(steps);
        const auto logRate = fit.empty() ? std::log10(sorted.front().rate)
                                         : bdrate::polyEval(fit, psnr);
        const auto pixel   = toPixel(std::pow(10.0, logRate), psnr);
        if (step == 0)
          path.moveTo(pixel);
        else
          path.lineTo(pixel);
      }
      painter.drawPath(path);
    }

    painter.setBrush(colour);
    for (const auto &point : curve.points)
      painter.drawEllipse(toPixel(point.rate, point.psnr), 3.0, 3.0);
    painter.setBrush(Qt::NoBrush);

    // Legend, bottom right, where the curves are least likely to be.
    const auto label = curve.name + "  " + bdRateText(curve.bdRate);
    painter.drawText(QRect(plot.left(), plot.top() + 2 + legendRow * 13, plot.width() - 4, 13),
                     Qt::AlignRight,
                     label);
    ++legendRow;
  }
}

// ---------------------------------------------------------------------------------------------
// BdRatePlotWindow
// ---------------------------------------------------------------------------------------------

BdRatePlotWindow::BdRatePlotWindow(QWidget *parent) : QDialog(parent)
{
  this->setWindowTitle("SB BD-rate");
  this->resize(1100, 720);
  // Non-modal: the point is to click superblocks in the view while this is open.
  this->setModal(false);

  auto *outer = new QVBoxLayout(this);

  auto *switches = new QHBoxLayout();
  this->showSb       = new QCheckBox("SB", this);
  this->showFrame    = new QCheckBox("Frame", this);
  this->showSequence = new QCheckBox("Sequence", this);
  this->showSb->setChecked(true);
  this->showFrame->setChecked(true);
  this->showSb->setToolTip("Rate-distortion of the clicked superblock, on the displayed frame.");
  this->showFrame->setToolTip("Rate-distortion of the whole displayed frame.");
  this->showSequence->setToolTip(
      "Rate-distortion accumulated over every frame of every stream. Checking this starts the "
      "sweep; unchecking cancels it.");
  for (auto *box : {this->showSb, this->showFrame, this->showSequence})
    switches->addWidget(box);

  this->sweepProgress = new QProgressBar(this);
  this->sweepProgress->setVisible(false);
  this->sweepProgress->setMaximumWidth(240);
  switches->addWidget(this->sweepProgress);
  this->sweepCancel = new QPushButton("Cancel sweep", this);
  this->sweepCancel->setVisible(false);
  switches->addWidget(this->sweepCancel);
  switches->addStretch();
  outer->addLayout(switches);

  this->panelRow      = new QHBoxLayout();
  this->sbPanel       = new BdRateCurvePanel("Superblock", this);
  this->framePanel    = new BdRateCurvePanel("Frame", this);
  this->sequencePanel = new BdRateCurvePanel("Sequence", this);
  this->sequencePanel->setMessage("Check \"Sequence\" to sweep every frame.");
  for (auto *panel : {this->sbPanel, this->framePanel, this->sequencePanel})
    this->panelRow->addWidget(panel, 1);
  outer->addLayout(this->panelRow, 3);

  this->status = new QLabel(this);
  this->status->setWordWrap(true);
  outer->addWidget(this->status);

  this->groupTable = new QTableWidget(0, 4, this);
  this->groupTable->setHorizontalHeaderLabels({"Anchor", "Group", "Points", "Original"});
  this->groupTable->horizontalHeader()->setStretchLastSection(true);
  this->groupTable->verticalHeader()->setVisible(false);
  this->groupTable->setMaximumHeight(150);
  outer->addWidget(this->groupTable, 1);

  this->valueTable = new QTableWidget(0, 5, this);
  this->valueTable->setHorizontalHeaderLabels(
      {"Group", "Stream", "Scope", "bits", "PSNR (dB)"});
  this->valueTable->horizontalHeader()->setStretchLastSection(true);
  this->valueTable->verticalHeader()->setVisible(false);
  outer->addWidget(this->valueTable, 2);

  this->sweeper = new BdRateSequenceSweeper(this);
  connect(this->sweeper, &BdRateSequenceSweeper::progressed, this, [this]() {
    this->updatePanels();
  });
  connect(this->sweeper, &BdRateSequenceSweeper::finished, this, [this]() {
    /* The frame and superblock panels describe the frame on screen, and the sweep just walked
     * every stream past it. Re-collect so those two do not keep showing whatever the last swept
     * frame happened to leave behind.
     */
    this->refresh();
  });

  for (auto *box : {this->showSb, this->showFrame})
    connect(box, &QCheckBox::toggled, this, [this]() { this->updatePanels(); });

  /* Checking Sequence is what starts the sweep - the user asked for the checkbox to be the switch.
   * Unchecking cancels it, which is the only way to stop a minute of work that is no longer
   * wanted.
   */
  connect(this->showSequence, &QCheckBox::toggled, this, [this](bool on) {
    if (on)
    {
      if (!this->groups.empty())
        this->sweeper->start(this->groups);
    }
    else
      this->sweeper->cancel();
    this->updatePanels();
  });
  connect(this->sweepCancel, &QPushButton::clicked, this, [this]() {
    this->sweeper->cancel();
    this->updatePanels();
  });

  // The group name is editable in place; anything else in that table is read only.
  connect(this->groupTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
    if (this->fillingTable || !item || item->column() != 1)
      return;
    const auto row = item->row();
    if (row >= 0 && row < int(this->groups.size()))
      this->groups[std::size_t(row)].name = item->text();
    this->updatePanels();
  });

  /* 200 ms is well under what reads as a stall and far above the cost of asking again: the retry
   * re-reads statistics that are already decoded, and only the missing SSE is waited on.
   */
  this->retryTimer.setInterval(200);
  this->retryTimer.setSingleShot(true);
  connect(&this->retryTimer, &QTimer::timeout, this, [this]() {
    if (this->retriesLeft > 0)
      --this->retriesLeft;
    this->collect();
    this->updatePanels();
    if (this->collected.pending && this->retriesLeft > 0)
      this->retryTimer.start();
  });

  this->updatePanels();
}

BdRateGroupResult BdRatePlotWindow::addGroup(const QList<playlistItem *> &selection)
{
  auto result = makeBdRateGroup(selection, this->groups);
  if (!result.ok())
    return result;

  this->groups.push_back(result.group);
  if (this->groups.size() == 1)
    this->anchorIndex = 0; // the first group is the anchor until the user says otherwise
  this->rebuildGroupTable();
  this->refresh();
  return result;
}

void BdRatePlotWindow::setCurrentFrame(int frameIdx)
{
  if (this->frameIdx == frameIdx)
    return;
  this->frameIdx = frameIdx;
  this->refresh();
}

void BdRatePlotWindow::setSelectedPosition(const QPoint &pixelPos, bool valid)
{
  if (valid)
    this->selectedPos = pixelPos;
  else
    this->selectedPos.reset();
  this->updatePanels();
}

void BdRatePlotWindow::rebuildGroupTable()
{
  this->fillingTable = true;
  this->groupTable->setRowCount(int(this->groups.size()));
  for (int row = 0; row < int(this->groups.size()); ++row)
  {
    const auto &group = this->groups[std::size_t(row)];

    auto *anchor = new QRadioButton(this->groupTable);
    anchor->setChecked(row == this->anchorIndex);
    /* Autoexclusive would only group buttons with the same parent widget, and these each sit in
     * their own cell. The exclusivity is done by hand below instead.
     */
    anchor->setAutoExclusive(false);
    connect(anchor, &QRadioButton::clicked, this, [this, row]() {
      this->anchorIndex = row;
      this->rebuildGroupTable();
      this->updatePanels();
    });
    this->groupTable->setCellWidget(row, 0, anchor);

    auto *name = new QTableWidgetItem(group.name);
    this->groupTable->setItem(row, 1, name);

    auto *points = new QTableWidgetItem(QString::number(group.points.size()));
    points->setFlags(points->flags() & ~Qt::ItemIsEditable);
    this->groupTable->setItem(row, 2, points);

    auto *original = new QTableWidgetItem(group.originalPath);
    original->setFlags(original->flags() & ~Qt::ItemIsEditable);
    this->groupTable->setItem(row, 3, original);
  }
  this->fillingTable = false;
}

void BdRatePlotWindow::collect()
{
  this->collected = collectBdRateFrame(this->groups, this->frameIdx);
}

void BdRatePlotWindow::refresh()
{
  // A fresh budget: this is a new question (new frame, new group), not a continuation of the last.
  this->retriesLeft = 40; // 8 seconds, which is far more than an SSE takes even at 4K
  this->collect();
  this->updatePanels();
  if (this->collected.pending)
    this->retryTimer.start();
  else
    this->retryTimer.stop();
}

void BdRatePlotWindow::updatePanels()
{
  this->sbPanel->setVisible(this->showSb->isChecked());
  this->framePanel->setVisible(this->showFrame->isChecked());
  this->sequencePanel->setVisible(this->showSequence->isChecked());

  if (this->groups.empty())
  {
    this->status->setText("No groups yet. Select the streams of one curve, plus the original YUV "
                          "or Y4M, and press SB BD-rate.");
    this->sbPanel->setMessage("No groups.");
    this->framePanel->setMessage("No groups.");
    this->valueTable->setRowCount(0);
    return;
  }

  if (!this->collected.error.isEmpty())
  {
    this->status->setText(this->collected.error);
    this->sbPanel->setMessage(this->collected.error);
    this->framePanel->setMessage(this->collected.error);
    return;
  }

  const auto anchor = std::size_t(std::clamp(this->anchorIndex, 0, int(this->groups.size()) - 1));

  /* Build the curves of one region, with each group's BD-rate against the anchor. `samplesOf`
   * picks the region out of the collected frame, so the two panels differ only in that.
   */
  const auto buildCurves =
      [this, anchor](const std::vector<std::vector<BdRateSample>> &perGroup) {
        std::vector<BdRateCurvePanel::Curve> curves;
        std::vector<bdrate::RatePoint>       anchorCurve;
        if (anchor < perGroup.size())
          anchorCurve = curveFor(perGroup[anchor]);

        for (std::size_t g = 0; g < this->groups.size() && g < perGroup.size(); ++g)
        {
          BdRateCurvePanel::Curve curve;
          curve.name     = this->groups[g].name;
          curve.points   = curveFor(perGroup[g]);
          curve.isAnchor = (g == anchor);
          if (!curve.isAnchor && !anchorCurve.empty())
            curve.bdRate = bdrate::bdRate(anchorCurve, curve.points);
          curves.push_back(std::move(curve));
        }
        return curves;
      };

  // --- Frame ------------------------------------------------------------------------------------
  const auto frameCurves = buildCurves(this->collected.frameTotals);
  if (this->collected.frameTotals.empty())
    this->framePanel->setMessage("Nothing collected for this frame yet.");
  else
    this->framePanel->setCurves(frameCurves);

  // --- SB ---------------------------------------------------------------------------------------
  std::vector<BdRateCurvePanel::Curve> sbCurves;
  BdRateSbKey                          sbKey{-1, -1};
  if (!this->selectedPos)
    this->sbPanel->setMessage("Click a superblock in the video view.");
  else
  {
    const auto grid = int(this->groups.front().superblockSize);
    sbKey           = {this->selectedPos->x() / grid, this->selectedPos->y() / grid};
    const auto it   = this->collected.perSuperblock.find(sbKey);
    if (it == this->collected.perSuperblock.end())
      this->sbPanel->setMessage(QString("No data for the superblock at (%1, %2).")
                                    .arg(sbKey.first)
                                    .arg(sbKey.second));
    else
    {
      sbCurves = buildCurves(it->second);
      this->sbPanel->setCurves(sbCurves);
    }
  }

  // --- Sequence ---------------------------------------------------------------------------------
  const auto &sequence = this->sweeper->data();
  const bool  sweeping = this->sweeper->running();
  this->sweepProgress->setVisible(sweeping);
  this->sweepCancel->setVisible(sweeping);
  if (sweeping)
    this->sweepProgress->setValue(int(this->sweeper->progress() * 100.0));

  std::vector<BdRateCurvePanel::Curve> sequenceCurves;
  std::vector<std::vector<BdRateSample>> sequenceSb;
  if (!this->showSequence->isChecked())
    this->sequencePanel->setMessage("Check \"Sequence\" to sweep every frame.");
  else if (!sequence.error.isEmpty())
    this->sequencePanel->setMessage(sequence.error);
  else if (!sequence.usable())
    this->sequencePanel->setMessage("Sweeping...");
  else
  {
    /* Partial results are drawn while the sweep runs. The curve moves as frames accumulate, which
     * is more useful than an empty box for a minute - and the status line says it is not final.
     */
    sequenceCurves = buildCurves(sequence.totals);
    this->sequencePanel->setCurves(sequenceCurves);
  }

  // --- status -----------------------------------------------------------------------------------
  QString text = QString("Frame %1").arg(this->frameIdx);
  if (this->selectedPos)
    text += QString(", superblock (%1, %2)").arg(sbKey.first).arg(sbKey.second);
  text += QString(", anchor \"%1\"").arg(this->groups[anchor].name);
  if (this->collected.pending)
    text += this->retriesLeft > 0
                ? ". Still computing - the SSE for this frame is on its way."
                : ". Gave up waiting for the SSE of this frame. Check that the original is still "
                  "there, or move to another frame and back.";
  /* Said out loud rather than left to be inferred from a gap: on a single superblock the two
   * curves often do not share a PSNR range at all, and a superblock that coded losslessly has no
   * finite PSNR to place on the axis.
   */
  int undefined = 0;
  for (const auto &curve : sbCurves)
    if (curve.bdRate && !curve.bdRate->ok())
      ++undefined;
  if (undefined > 0)
    text += QString(" %1 of the superblock's curves have no BD-rate (too few points, or no "
                    "overlapping PSNR range).")
                .arg(undefined);
  if (this->showSequence->isChecked())
    text += "  |  " + this->sweeper->statusText() +
            (sweeping ? " (the sequence curve is still filling in)" : "");
  this->status->setText(text);

  // --- numbers ----------------------------------------------------------------------------------
  this->valueTable->setRowCount(0);
  const auto addRows = [this](const QString                              &scope,
                              const std::vector<std::vector<BdRateSample>> &perGroup) {
    for (std::size_t g = 0; g < this->groups.size() && g < perGroup.size(); ++g)
      for (std::size_t p = 0; p < this->groups[g].points.size() && p < perGroup[g].size(); ++p)
      {
        const auto &sample = perGroup[g][p];
        const auto  row    = this->valueTable->rowCount();
        this->valueTable->insertRow(row);
        this->valueTable->setItem(row, 0, new QTableWidgetItem(this->groups[g].name));
        this->valueTable->setItem(row, 1, new QTableWidgetItem(this->groups[g].points[p].label));
        this->valueTable->setItem(row, 2, new QTableWidgetItem(scope));
        this->valueTable->setItem(row, 3, new QTableWidgetItem(QString::number(sample.bits)));
        const auto psnr = sample.psnr();
        this->valueTable->setItem(
            row,
            4,
            new QTableWidgetItem(psnr ? QString::number(*psnr, 'f', 3)
                                      : (sample.sse == 0.0 ? "lossless" : "-")));
      }
  };
  if (this->showFrame->isChecked())
    addRows("frame", this->collected.frameTotals);
  if (this->showSb->isChecked() && this->selectedPos)
    if (const auto it = this->collected.perSuperblock.find(sbKey);
        it != this->collected.perSuperblock.end())
      addRows(QString("SB (%1,%2)").arg(sbKey.first).arg(sbKey.second), it->second);
  if (this->showSequence->isChecked() && sequence.usable())
  {
    addRows(QString("seq %1-%2").arg(sequence.firstFrame).arg(sequence.lastFrame), sequence.totals);
    if (this->selectedPos)
      if (const auto it = sequence.perSuperblock.find(sbKey); it != sequence.perSuperblock.end())
        addRows(QString("seq SB (%1,%2)").arg(sbKey.first).arg(sbKey.second), it->second);
  }
}

} // namespace bda::integration
