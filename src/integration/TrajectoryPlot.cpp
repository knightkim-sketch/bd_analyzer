#include "TrajectoryPlot.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace bda::integration
{

TrajectoryPlot::TrajectoryPlot(QWidget *parent) : QWidget(parent)
{
  this->setObjectName("trajectoryPlot");
  this->setToolTip("Camera trajectory from the global model: raw (grey) and smoothed (colour). The "
                   "correction applied to a frame is the gap between the two.");
}

void TrajectoryPlot::setTrajectory(int firstFrame, std::vector<filter::StabilizerFrame> trajectory)
{
  this->first  = firstFrame;
  this->frames = std::move(trajectory);
  this->update();
}

void TrajectoryPlot::setCurrentFrame(int frameIdx)
{
  if (frameIdx == this->current)
    return;
  this->current = frameIdx;
  this->update();
}

void TrajectoryPlot::paintEvent(QPaintEvent *)
{
  QPainter p(this);
  p.fillRect(this->rect(), this->palette().base());
  p.setRenderHint(QPainter::Antialiasing, true);
  if (this->frames.size() < 2)
  {
    p.setPen(this->palette().color(QPalette::PlaceholderText));
    p.drawText(this->rect(), Qt::AlignCenter, "No trajectory - run with a model, then pick a method.");
    return;
  }

  struct Band
  {
    const char *name;
    double (*get)(const filter::TrajectoryPoint &);
    QColor colour;
  };
  const Band bands[] = {
      {"dx (px)", [](const filter::TrajectoryPoint &t) { return t.dx; }, QColor(30, 120, 220)},
      {"dy (px)", [](const filter::TrajectoryPoint &t) { return t.dy; }, QColor(220, 120, 30)},
      {"angle (deg)", [](const filter::TrajectoryPoint &t) { return t.angle; }, QColor(40, 160, 70)},
  };
  const int    left = 70, right = 8, top = 4;
  const int    bandH = (this->height() - top - 16) / 3;
  const int    n     = int(this->frames.size());
  const double xs    = double(this->width() - left - right) / std::max(1, n - 1);
  const QColor axis  = this->palette().color(QPalette::Mid);

  for (int b = 0; b < 3; ++b)
  {
    const auto &band = bands[b];
    const int   y0   = top + b * bandH;
    double      lo = 0, hi = 0;
    for (const auto &f : this->frames)
      for (double v : {band.get(f.raw), band.get(f.smoothed)})
        lo = std::min(lo, v), hi = std::max(hi, v);
    if (hi - lo < 1e-9)
      hi = lo + 1;
    const double pad = (hi - lo) * 0.08;
    lo -= pad, hi += pad;
    const auto px = [&](int i) { return left + i * xs; };
    const auto py = [&](double v) { return y0 + (bandH - 6) * (1 - (v - lo) / (hi - lo)) + 3; };

    p.setPen(axis);
    p.drawRect(QRectF(left, y0 + 2, this->width() - left - right, bandH - 4));
    p.setPen(this->palette().color(QPalette::Text));
    p.drawText(QRectF(2, y0, left - 6, bandH / 2), Qt::AlignRight | Qt::AlignVCenter, band.name);
    p.drawText(QRectF(2, y0 + bandH / 2, left - 6, bandH / 2), Qt::AlignRight | Qt::AlignTop,
               QString("%1 .. %2").arg(lo + pad, 0, 'f', 1).arg(hi - pad, 0, 'f', 1));
    if (lo < 0 && hi > 0)
    {
      p.setPen(QPen(axis, 1, Qt::DotLine));
      p.drawLine(QPointF(left, py(0)), QPointF(this->width() - right, py(0)));
    }

    for (const bool smoothed : {false, true})
    {
      QPainterPath path;
      for (int i = 0; i < n; ++i)
      {
        const auto &f = this->frames[std::size_t(i)];
        const QPointF pt(px(i), py(band.get(smoothed ? f.smoothed : f.raw)));
        if (i == 0)
          path.moveTo(pt);
        else
          path.lineTo(pt);
      }
      p.setPen(QPen(smoothed ? band.colour : QColor(150, 150, 150), smoothed ? 2.0 : 1.0));
      p.drawPath(path);
    }
  }

  const int ci = this->current - this->first;
  if (ci >= 0 && ci < n)
  {
    p.setPen(QPen(QColor(220, 40, 40), 1, Qt::DashLine));
    p.drawLine(QPointF(left + ci * xs, top), QPointF(left + ci * xs, top + 3 * bandH));
  }
  p.setPen(this->palette().color(QPalette::Text));
  p.drawText(QRectF(left, top + 3 * bandH, this->width() - left - right, 14), Qt::AlignLeft,
             QString("frame %1").arg(this->first));
  p.drawText(QRectF(left, top + 3 * bandH, this->width() - left - right, 14), Qt::AlignRight,
             QString("frame %1").arg(this->first + n - 1));
}

} // namespace bda::integration
