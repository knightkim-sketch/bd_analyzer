// The Bjøntegaard delta rate, and the PSNR that feeds it.
//
// Deliberately free of Qt and of anything from the upstream tree, so it can be unit tested without
// a build of the library behind it - the same split src/me uses. Everything here is a pure function
// over plain numbers.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace bda::bdrate
{

/* One operating point of a curve: what it cost, and what it bought.
 *
 * `rate` is bits - of a superblock, of a frame, or of a sequence, depending on which panel is
 * asking. BD-rate is scale free in the rate axis (it reports a ratio), so the unit only has to be
 * consistent within a comparison, not absolute.
 */
struct RatePoint
{
  double rate{};
  double psnr{};
};

/* PSNR of a block or frame from its sum of squared errors.
 *
 * sampleCount must be the number of samples the SSE was actually summed over. For a superblock at
 * the right or bottom edge of the picture that is less than the nominal block area, and using the
 * nominal area instead makes those blocks look better than they are - it divides the same error
 * over more samples.
 *
 * An SSE of zero is lossless and has no finite PSNR. Rather than returning infinity, which
 * poisons every average and plot downstream, this reports nothing and lets the caller say
 * "lossless".
 */
std::optional<double> psnrFromSse(double sse, std::int64_t sampleCount, unsigned bitDepth = 8);

//!< Why a BD-rate could not be produced. Reported rather than silently returning 0.
enum class BdRateStatus
{
  Ok,
  NotEnoughPoints, //!< Fewer than two points on one of the curves.
  NoPsnrOverlap,   //!< The two curves do not share a PSNR interval, so there is nothing to compare.
  Degenerate       //!< Zero or negative rates, or a curve with no PSNR spread to integrate over.
};

struct BdRateResult
{
  BdRateStatus status{BdRateStatus::NotEnoughPoints};
  //!< Percent. Negative means the test curve needs fewer bits than the anchor, which is better.
  double       percent{};
  /* The polynomial degree actually fitted. The standard metric uses a cubic over four points; with
   * three points this drops to a quadratic and with two to a line. Reported because a two-point
   * "BD-rate" is a slope comparison and should not be read as the standard figure - which matters
   * here, where a single superblock often has few usable points.
   */
  int          degree{};
  //!< The PSNR interval the integration ran over, in dB. A narrow one means a weak number.
  double       psnrOverlapLow{};
  double       psnrOverlapHigh{};

  bool ok() const { return this->status == BdRateStatus::Ok; }
};

/* BD-rate of `test` against `anchor`, following Bjøntegaard.
 *
 * log10(rate) is fitted as a function of PSNR, both fits are integrated over the PSNR interval the
 * two curves share, and the average difference becomes a rate ratio. The direction is the usual
 * one: a negative percentage means the test curve reached the same quality for fewer bits.
 *
 * Points may arrive in any order and are sorted here. Duplicate PSNRs are collapsed, keeping the
 * lower rate: two encodes that reached the same quality are one operating point, and leaving both
 * in makes the fit singular.
 */
BdRateResult bdRate(const std::vector<RatePoint> &anchor, const std::vector<RatePoint> &test);

/* Least squares polynomial fit, y over x, returned lowest order coefficient first.
 *
 * Exposed because it is the part worth testing on its own, and because the plot draws the fitted
 * curve rather than straight segments between the points - the integral is taken over the fit, so
 * showing the segments would draw a curve the number does not come from.
 */
std::vector<double> polyFit(const std::vector<double> &x, const std::vector<double> &y, int degree);

//!< Evaluate a polynomial from polyFit() at x.
double polyEval(const std::vector<double> &coefficients, double x);

} // namespace bda::bdrate
