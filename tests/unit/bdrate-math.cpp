// Unit test for the BD-rate maths. No Qt, no library - see src/bdrate/BdRateMath.h.
//
// The interesting thing about testing BD-rate is that there is an exact answer available without
// any reference data: if the test curve costs k times the anchor's rate at every quality, then the
// BD-rate is exactly (k-1)*100 percent, whatever the curve shape. That pins the whole pipeline -
// the log, the fit, the integral and the final exponentiation - against arithmetic rather than
// against another implementation.
//
// The rest of the cases are the ones a superblock actually runs into: curves that do not overlap in
// PSNR, two or three points instead of four, and a lossless block.
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "bdrate/BdRateMath.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
void checkNear(double got, double want, double tolerance, const std::string &what)
{
  const bool ok = std::abs(got - want) <= tolerance;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (got " << got << ", want " << want << " +-" << tolerance << ")";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

using bda::bdrate::BdRateStatus;
using bda::bdrate::RatePoint;

// A plausible RD curve: rate roughly doubles for every 6 dB, which is the usual shape.
std::vector<RatePoint> curve(double scale)
{
  return {{scale * 100000.0, 32.0},
          {scale * 200000.0, 36.0},
          {scale * 400000.0, 40.0},
          {scale * 800000.0, 44.0}};
}
} // namespace

int main()
{
  using namespace bda::bdrate;

  // --- PSNR from SSE ---------------------------------------------------------------------------
  {
    /* A 64x64 block of 8 bit samples with an average squared error of 1 per sample: SSE = 4096,
     * so PSNR = 10*log10(255^2) = 48.13 dB. Worth having as a hand-checkable anchor.
     */
    const auto psnr = psnrFromSse(4096.0, 64 * 64, 8);
    check(psnr.has_value(), "an SSE gives a PSNR");
    checkNear(psnr.value_or(0.0), 48.1308, 1e-3, "and it is the hand computed value");

    check(!psnrFromSse(0.0, 4096, 8).has_value(),
          "a lossless block reports nothing rather than infinity");
    check(!psnrFromSse(100.0, 0, 8).has_value(), "and so does a block with no samples");
    check(!psnrFromSse(-1.0, 4096, 8).has_value(), "a negative SSE means no original was attached");

    /* The edge case that matters for superblocks: the same SSE over fewer samples is a worse PSNR.
     * Using the nominal 64x64 for a clipped edge block would report the left hand number for a
     * block that only covers half as many pixels.
     */
    const auto full = psnrFromSse(4096.0, 64 * 64, 8).value_or(0.0);
    const auto half = psnrFromSse(4096.0, 64 * 32, 8).value_or(0.0);
    check(half < full, "the same error over fewer samples is a lower PSNR");
    checkNear(full - half, 10.0 * std::log10(2.0), 1e-9, "by exactly the sample count ratio");

    // 10 bit peaks at 1023, so the same normalised error is a higher number.
    check(psnrFromSse(4096.0, 4096, 10).value_or(0.0) > full, "a deeper source scales the peak");
  }

  // --- polynomial fit --------------------------------------------------------------------------
  {
    // Four points off an exact cubic must be reproduced exactly, since the fit is determined.
    const std::vector<double> x{1.0, 2.0, 3.0, 4.0};
    std::vector<double>       y;
    const auto                truth = [](double v) { return 2.0 + 3.0 * v - v * v + 0.5 * v * v * v; };
    for (const auto value : x)
      y.push_back(truth(value));

    const auto fit = polyFit(x, y, 3);
    check(fit.size() == 4, "a cubic fit has four coefficients");
    for (const auto value : {1.5, 2.5, 3.5})
      checkNear(polyEval(fit, value), truth(value), 1e-6,
                "the fit reproduces the cubic at x=" + std::to_string(value));

    check(polyFit(x, y, 4).empty(), "asking for more degrees than points gives nothing");
    check(polyFit({1.0}, {1.0}, 0).size() == 1, "a single point still fits a constant");
  }

  /* --- BD-rate against arithmetic --------------------------------------------------------------
   *
   * The whole point of this test. Scaling every rate by k, with the PSNRs untouched, must come back
   * as exactly (k-1)*100 percent - no reference implementation needed to know that.
   */
  {
    const auto anchor = curve(1.0);
    for (const auto k : {0.5, 0.8, 1.0, 1.25, 2.0})
    {
      const auto result = bdRate(anchor, curve(k));
      check(result.ok(), "a uniformly scaled curve gives a BD-rate (k=" + std::to_string(k) + ")");
      checkNear(result.percent, (k - 1.0) * 100.0, 1e-6,
                "and it is exactly (k-1)*100 for k=" + std::to_string(k));
    }

    const auto same = bdRate(anchor, anchor);
    checkNear(same.percent, 0.0, 1e-9, "a curve against itself is 0%");
    check(same.degree == 3, "four points are fitted with a cubic, as the standard metric does");
    checkNear(same.psnrOverlapLow, 32.0, 1e-9, "the overlap is the full range");
    checkNear(same.psnrOverlapHigh, 44.0, 1e-9, "at both ends");
  }

  // --- direction -------------------------------------------------------------------------------
  {
    // Cheaper for the same quality is an improvement, and improvements are negative.
    check(bdRate(curve(1.0), curve(0.7)).percent < 0.0, "needing fewer bits reads as negative");
    check(bdRate(curve(1.0), curve(1.3)).percent > 0.0, "needing more bits reads as positive");
  }

  /* --- the cases a single superblock actually produces -----------------------------------------
   *
   * A superblock's curve is short and often does not overlap the other group's at all. These must
   * come back as a status, not as a number that looks usable.
   */
  {
    const std::vector<RatePoint> low{{1000.0, 20.0}, {2000.0, 24.0}, {4000.0, 28.0}};
    const std::vector<RatePoint> high{{1000.0, 40.0}, {2000.0, 44.0}, {4000.0, 48.0}};
    const auto                   apart = bdRate(low, high);
    check(apart.status == BdRateStatus::NoPsnrOverlap,
          "curves in different quality ranges have no BD-rate");
    check(!apart.ok(), "and the result says so rather than reporting 0%");

    const auto three = bdRate(low, {{1500.0, 20.0}, {3000.0, 24.0}, {6000.0, 28.0}});
    check(three.ok(), "three points still work");
    check(three.degree == 2, "fitted with a quadratic, and it says so");
    checkNear(three.percent, 50.0, 1e-6, "and the scaling answer still holds");

    const auto two = bdRate({{1000.0, 30.0}, {2000.0, 36.0}}, {{2000.0, 30.0}, {4000.0, 36.0}});
    check(two.ok(), "two points work");
    check(two.degree == 1, "as a straight line - not the standard metric, and it says so");
    checkNear(two.percent, 100.0, 1e-6, "with the scaling answer again");

    check(bdRate({{1000.0, 30.0}}, curve(1.0)).status == BdRateStatus::NotEnoughPoints,
          "one point is not a curve");
    check(bdRate({}, curve(1.0)).status == BdRateStatus::Degenerate, "and neither is none");
  }

  // --- input hygiene ----------------------------------------------------------------------------
  {
    // Zero and negative rates cannot be logged. Dropping them beats returning a NaN.
    const auto withZero = bdRate(curve(1.0), {{0.0, 32.0},
                                              {200000.0, 36.0},
                                              {400000.0, 40.0},
                                              {800000.0, 44.0}});
    check(withZero.ok(), "a zero rate point is dropped rather than poisoning the fit");
    check(withZero.degree == 2, "which leaves three points, so a quadratic");

    /* Two encodes that reached the same PSNR are one operating point. Left in, they give the fit
     * two answers for one input and the higher degrees go singular.
     */
    const auto duplicated = bdRate(curve(1.0), {{100000.0, 32.0},
                                                {150000.0, 32.0},
                                                {200000.0, 36.0},
                                                {400000.0, 40.0},
                                                {800000.0, 44.0}});
    check(duplicated.ok(), "a duplicated PSNR is collapsed");
    checkNear(duplicated.percent, 0.0, 1e-6, "keeping the cheaper point, so this is the anchor");

    // Points out of order are the normal case when they come from a map.
    const auto shuffled = bdRate(curve(1.0), {{800000.0, 44.0},
                                              {100000.0, 32.0},
                                              {400000.0, 40.0},
                                              {200000.0, 36.0}});
    check(shuffled.ok() && std::abs(shuffled.percent) < 1e-6, "order does not matter");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
