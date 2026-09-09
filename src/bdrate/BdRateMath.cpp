#include "BdRateMath.h"

#include <algorithm>
#include <cmath>

namespace bda::bdrate
{
namespace
{

/* Solve a small dense system by Gaussian elimination with partial pivoting.
 *
 * The normal equations of a cubic fit are 4x4 at most, so there is no reason to pull in a linear
 * algebra dependency. Partial pivoting is not optional even at this size: the Vandermonde matrix of
 * PSNR values (numbers around 40) is badly scaled, and without pivoting the elimination loses the
 * high order coefficient.
 */
bool solveInPlace(std::vector<std::vector<double>> &a, std::vector<double> &b)
{
  const auto n = b.size();
  for (std::size_t col = 0; col < n; ++col)
  {
    auto pivot = col;
    for (auto row = col + 1; row < n; ++row)
      if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
        pivot = row;
    if (std::abs(a[pivot][col]) < 1e-12)
      return false;
    std::swap(a[col], a[pivot]);
    std::swap(b[col], b[pivot]);

    for (auto row = col + 1; row < n; ++row)
    {
      const auto factor = a[row][col] / a[col][col];
      if (factor == 0.0)
        continue;
      for (auto k = col; k < n; ++k)
        a[row][k] -= factor * a[col][k];
      b[row] -= factor * b[col];
    }
  }

  for (auto row = n; row-- > 0;)
  {
    auto value = b[row];
    for (auto k = row + 1; k < n; ++k)
      value -= a[row][k] * b[k];
    b[row] = value / a[row][row];
  }
  return true;
}

//!< Definite integral of a polynomial from `low` to `high`.
double polyIntegral(const std::vector<double> &coefficients, double low, double high)
{
  // The antiderivative of sum(c_i x^i) is sum(c_i x^(i+1) / (i+1)), with no constant term needed
  // because this is evaluated as a difference.
  const auto at = [&coefficients](double x) {
    double total = 0.0;
    double power = x;
    for (std::size_t i = 0; i < coefficients.size(); ++i)
    {
      total += coefficients[i] * power / double(i + 1);
      power *= x;
    }
    return total;
  };
  return at(high) - at(low);
}

/* Sorted by PSNR, with duplicate PSNRs collapsed to the cheaper point.
 *
 * Two encodes that landed on the same quality are one operating point of the curve. Keeping both
 * gives the fit two different answers for the same input, which makes the normal equations singular
 * for the higher degrees - and picking the lower rate is the right one to keep, because that is the
 * point on the convex hull.
 */
std::vector<RatePoint> normalise(const std::vector<RatePoint> &points)
{
  std::vector<RatePoint> usable;
  usable.reserve(points.size());
  for (const auto &point : points)
    if (point.rate > 0.0 && std::isfinite(point.rate) && std::isfinite(point.psnr))
      usable.push_back(point);

  std::sort(usable.begin(), usable.end(), [](const RatePoint &a, const RatePoint &b) {
    if (a.psnr != b.psnr)
      return a.psnr < b.psnr;
    return a.rate < b.rate;
  });

  usable.erase(std::unique(usable.begin(),
                           usable.end(),
                           [](const RatePoint &a, const RatePoint &b) { return a.psnr == b.psnr; }),
               usable.end());
  return usable;
}

} // namespace

std::optional<double> psnrFromSse(double sse, std::int64_t sampleCount, unsigned bitDepth)
{
  if (sampleCount <= 0 || sse < 0.0 || !std::isfinite(sse))
    return std::nullopt;
  if (sse == 0.0)
    return std::nullopt; // lossless - no finite PSNR, see the header

  const double peak = double((1u << bitDepth) - 1u);
  return 10.0 * std::log10(peak * peak * double(sampleCount) / sse);
}

std::vector<double> polyFit(const std::vector<double> &x, const std::vector<double> &y, int degree)
{
  if (x.size() != y.size() || x.empty() || degree < 0)
    return {};
  const auto terms = std::size_t(degree) + 1;
  if (x.size() < terms)
    return {};

  /* Normal equations: (V^T V) c = V^T y, where V is the Vandermonde matrix. Fine at these sizes -
   * the alternative (a QR factorisation) buys accuracy that a four point cubic does not need.
   */
  std::vector<std::vector<double>> a(terms, std::vector<double>(terms, 0.0));
  std::vector<double>              b(terms, 0.0);
  for (std::size_t sample = 0; sample < x.size(); ++sample)
  {
    std::vector<double> powers(2 * terms - 1, 1.0);
    for (std::size_t i = 1; i < powers.size(); ++i)
      powers[i] = powers[i - 1] * x[sample];
    for (std::size_t row = 0; row < terms; ++row)
    {
      for (std::size_t col = 0; col < terms; ++col)
        a[row][col] += powers[row + col];
      b[row] += powers[row] * y[sample];
    }
  }

  if (!solveInPlace(a, b))
    return {};
  return b;
}

double polyEval(const std::vector<double> &coefficients, double x)
{
  double total = 0.0;
  for (auto it = coefficients.rbegin(); it != coefficients.rend(); ++it)
    total = total * x + *it;
  return total;
}

BdRateResult bdRate(const std::vector<RatePoint> &anchor, const std::vector<RatePoint> &test)
{
  BdRateResult result;

  const auto a = normalise(anchor);
  const auto t = normalise(test);
  if (a.size() < 2 || t.size() < 2)
  {
    result.status = a.empty() || t.empty() ? BdRateStatus::Degenerate : BdRateStatus::NotEnoughPoints;
    return result;
  }

  // The interval both curves cover. Outside it one of the fits is extrapolating, and an
  // extrapolated cubic is not a measurement.
  const auto low  = std::max(a.front().psnr, t.front().psnr);
  const auto high = std::min(a.back().psnr, t.back().psnr);
  result.psnrOverlapLow  = low;
  result.psnrOverlapHigh = high;
  if (!(high > low))
  {
    result.status = BdRateStatus::NoPsnrOverlap;
    return result;
  }

  /* Cubic where there are four points, which is the standard metric; lower where there are fewer.
   * Capped by both curves so the two integrals are of comparable smoothness - fitting a cubic to
   * one side and a line to the other would compare two different kinds of curve.
   */
  const auto degree = std::min<int>(3, int(std::min(a.size(), t.size())) - 1);
  result.degree     = degree;

  const auto logRates = [](const std::vector<RatePoint> &points) {
    std::vector<double> out;
    out.reserve(points.size());
    for (const auto &point : points)
      out.push_back(std::log10(point.rate));
    return out;
  };
  const auto psnrs = [](const std::vector<RatePoint> &points) {
    std::vector<double> out;
    out.reserve(points.size());
    for (const auto &point : points)
      out.push_back(point.psnr);
    return out;
  };

  // log10(rate) as a function of PSNR, which is the direction the metric integrates in.
  const auto fitA = polyFit(psnrs(a), logRates(a), degree);
  const auto fitT = polyFit(psnrs(t), logRates(t), degree);
  if (fitA.empty() || fitT.empty())
  {
    result.status = BdRateStatus::Degenerate;
    return result;
  }

  const auto integralA = polyIntegral(fitA, low, high);
  const auto integralT = polyIntegral(fitT, low, high);
  const auto average   = (integralT - integralA) / (high - low);
  if (!std::isfinite(average))
  {
    result.status = BdRateStatus::Degenerate;
    return result;
  }

  result.percent = (std::pow(10.0, average) - 1.0) * 100.0;
  result.status  = BdRateStatus::Ok;
  return result;
}

} // namespace bda::bdrate
