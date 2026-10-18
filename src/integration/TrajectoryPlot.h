// The camera trajectory of a stabilisation run, drawn for the Global motion window: dx and dy in
// pixels and the angle in degrees, each in its own band, the raw path in grey and the smoothed one
// in colour, with a line at the frame on screen.
#pragma once

#include <QWidget>

#include <vector>

#include "filter/Stabilizer.h"

namespace bda::integration
{

class TrajectoryPlot : public QWidget
{
  Q_OBJECT

public:
  explicit TrajectoryPlot(QWidget *parent = nullptr);

  //!< `frames[i]` belongs to frame `firstFrame + i`.
  void setTrajectory(int firstFrame, std::vector<filter::StabilizerFrame> frames);
  void setCurrentFrame(int frameIdx);

  QSize minimumSizeHint() const override { return {320, 180}; }

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  int                                  first{};
  std::vector<filter::StabilizerFrame> frames;
  int                                  current{-1};
};

} // namespace bda::integration
