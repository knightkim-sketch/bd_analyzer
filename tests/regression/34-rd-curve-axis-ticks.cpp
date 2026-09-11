// Regression: the tick values on the RD curve axes, and that the panel draws them.
//
// A wrong tick is invisible. 42.7 / 45.1 / 47.5 draws exactly as convincingly as 42 / 44 / 46, and
// a rate axis labelled one decade out reads as a perfectly plausible number - so the values are
// pinned here rather than judged by looking at the picture.
//
// The rate axis is the one that actually needs thought: it is drawn in log space, so stepping the
// pixels evenly would label 10^3.4 and 10^3.7. The ticks have to be round in *bits*.
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <cmath>
#include <iostream>
#include <string>

#include "integration/BdRatePlotWindow.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

bool near(double got, double want) { return std::abs(got - want) < 1e-9; }

bool hasTick(const std::vector<double> &ticks, double want)
{
  for (const auto tick : ticks)
    if (near(tick, want))
      return true;
  return false;
}

using namespace bda::integration;
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  std::cout << "rd curve axis ticks" << std::endl;

  // --- linear steps are round numbers ------------------------------------------------------
  {
    check(near(niceAxisStep(15.0, 5), 2.0), "a 15 dB range over 5 divisions steps by 2");
    check(near(niceAxisStep(50.0, 5), 10.0), "a 50 dB range steps by 10");
    check(near(niceAxisStep(4.0, 5), 1.0), "a 4 dB range steps by 1");
    check(near(niceAxisStep(2.0, 5), 0.5), "a 2 dB range steps by 0.5");

    /* Every step must be 1, 2 or 5 times a power of ten, over the whole range of PSNR spans this
     * panel sees - from a couple of dB on one superblock to tens across a sequence.
     */
    bool allRound = true;
    for (double range = 0.2; range < 60.0; range += 0.1)
    {
      const auto step      = niceAxisStep(range, 5);
      const auto magnitude = std::pow(10.0, std::floor(std::log10(step)));
      const auto mantissa  = step / magnitude;
      allRound = allRound && (near(mantissa, 1.0) || near(mantissa, 2.0) || near(mantissa, 5.0));
    }
    check(allRound, "every step across the plausible range is 1, 2 or 5 x 10^k");
  }
  {
    // Degenerate input must not produce a zero or negative step - that is an infinite tick loop.
    check(niceAxisStep(0.0, 5) > 0.0, "a zero range still yields a positive step");
    check(niceAxisStep(-3.0, 5) > 0.0, "so does a negative one");
    check(niceAxisStep(10.0, 0) > 0.0, "and zero divisions");
  }

  // --- log ticks are round in bits, not in log space ---------------------------------------
  {
    // Roughly the frame totals in the design doc's measured example: 1071..6435 bits.
    const auto ticks = logAxisTicks(std::log10(900.0), std::log10(7000.0));
    check(hasTick(ticks, 1000.0), "1000 is a tick");
    check(hasTick(ticks, 2000.0), "2000 is a tick");
    check(hasTick(ticks, 5000.0), "5000 is a tick");
    check(!hasTick(ticks, 900.0), "the range edge itself is not forced to be a tick");

    bool inRange = true;
    for (const auto tick : ticks)
      inRange = inRange && tick >= 900.0 && tick <= 7000.0;
    check(inRange, "no tick falls outside the visible range");

    bool allRound = true;
    for (const auto tick : ticks)
    {
      const auto magnitude = std::pow(10.0, std::floor(std::log10(tick)));
      const auto mantissa  = tick / magnitude;
      allRound             = allRound && (near(mantissa, 1.0) || near(mantissa, 2.0) ||
                              near(mantissa, 5.0) || near(mantissa, 1.5) ||
                              near(mantissa, 3.0) || near(mantissa, 7.0));
    }
    check(allRound, "every tick is a round value in bits");
  }
  {
    /* A skipped superblock is bits+1 = 1, and a busy one is thousands, so a single panel can span
     * four decades. Every decade boundary has to be there.
     */
    const auto wide = logAxisTicks(0.0, 4.0);
    for (const double decade : {1.0, 10.0, 100.0, 1000.0, 10000.0})
      check(hasTick(wide, decade), "a four decade span keeps " + std::to_string(int(decade)));
  }
  {
    // A narrow span would give fewer than three ticks from 1/2/5 alone; the fallback fills in.
    const auto narrow = logAxisTicks(std::log10(2100.0), std::log10(4800.0));
    check(narrow.size() >= 3, "a narrow span still gets at least three ticks");
    bool inRange = true;
    for (const auto tick : narrow)
      inRange = inRange && tick >= 2100.0 && tick <= 4800.0;
    check(inRange, "and they are still inside it");
  }

  // --- labels stay short enough for a narrow panel -----------------------------------------
  {
    /* Values that are actually ticks. A non-round value like 6435 would never be labelled, and
     * asserting its rounding only pins floating point noise (6435/1000 is 6.43499... in binary,
     * so it prints 6.43, not 6.44).
     */
    check(formatAxisRate(1.0) == "1", "a single bit reads as 1");
    check(formatAxisRate(500.0) == "500", "hundreds are written out");
    check(formatAxisRate(5000.0) == "5k", "thousands are abbreviated");
    check(formatAxisRate(20000.0) == "20k", "and tens of thousands");
    check(formatAxisRate(1.2e6) == "1.2M", "millions too");
    bool shortEnough = true;
    for (const double value : {1.0, 20.0, 500.0, 6435.0, 67304.0, 1.2e6, 3.5e7})
      shortEnough = shortEnough && formatAxisRate(value).size() <= 6;
    check(shortEnough, "no label is longer than six characters");
  }

  // --- the panel actually paints them ------------------------------------------------------
  {
    BdRateCurvePanel panel(QStringLiteral("Frame"));
    panel.resize(320, 260);

    BdRateCurvePanel::Curve curve;
    curve.name     = QStringLiteral("fast");
    curve.isAnchor = true;
    // The measured frame totals from the design doc, on the bits+1 axis the panel uses.
    curve.points = {{1072, 36.924}, {2843, 42.143}, {6436, 51.723}};
    panel.setCurves({curve});

    QImage image(panel.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    panel.render(&image);

    /* Ink in the left and bottom margins is the tick labels: nothing else is drawn there, and
     * before this change those margins held only the two rotated axis titles.
     */
    const auto inkIn = [&image](int x0, int y0, int x1, int y1) {
      int count = 0;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
          if (qGray(image.pixel(x, y)) < 200)
            ++count;
      return count;
    };

    check(inkIn(18, 22, 48, 200) > 20, "the left margin carries PSNR tick labels");
    check(inkIn(52, 200, 300, 224) > 20, "the bottom margin carries rate tick labels");
    check(inkIn(52, 22, 300, 200) > 50, "and the plot area still has the curve in it");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
