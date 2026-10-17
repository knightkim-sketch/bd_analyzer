#include "Stabilizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace bda::filter
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

} // namespace

TrajectoryPoint motionFromModel(const AffineParams &m, int width, int height)
{
  const double cx = width / 2.0, cy = height / 2.0;
  // Rotation-zoom reading of the linear part: [[a, -b], [b, a]] = s R(phi).
  const double a = (m[2] + m[5]) / 2, b = (m[4] - m[3]) / 2;
  const double s = std::hypot(a, b);
  if (!(s > 1e-9))
    return {};
  const double phi = std::atan2(b, a);
  // t = model(c) - c, the reference-side offset of the centre.
  const double tx = m[0] + m[2] * cx + m[3] * cy - cx;
  const double ty = m[1] + m[4] * cx + m[5] * cy - cy;
  // Content k-1 -> k: cur = c + (sR)^-1 (ref - c - t); at ref = c the centre moves by -(sR)^-1 t.
  const double c = std::cos(phi) / s, sn = std::sin(phi) / s; // (sR)^-1 = (1/s) R(-phi)
  TrajectoryPoint d;
  d.dx       = -(c * tx + sn * ty);
  d.dy       = -(-sn * tx + c * ty);
  d.angle    = -phi * 180.0 / kPi;
  d.logScale = -std::log(s);
  return d;
}

AffineParams warpParams(const TrajectoryPoint &corr, int width, int height)
{
  const double cx = width / 2.0, cy = height / 2.0;
  // Forward: p_out = c + S R(a) (p_in - c) + d.  Inverse: p_in = c + (1/S) R(-a) (p_out - c - d).
  const double rad = corr.angle * kPi / 180.0;
  const double k   = std::exp(-corr.logScale);
  const double c = k * std::cos(rad), s = k * std::sin(rad);
  // (1/S) R(-a) = [[c, s], [-s, c]]
  AffineParams p;
  p[2] = c, p[3] = s, p[4] = -s, p[5] = c;
  const double ux = cx + corr.dx, uy = cy + corr.dy;
  p[0] = cx - (p[2] * ux + p[3] * uy);
  p[1] = cy - (p[4] * ux + p[5] * uy);
  return p;
}

std::vector<StabilizerFrame> stabilize(const std::vector<AffineParams> &models,
                                       int                              width,
                                       int                              height,
                                       const StabilizerParams          &params)
{
  const int                    n = int(models.size());
  std::vector<StabilizerFrame> out(std::size_t(std::max(0, n)));
  for (int k = 1; k < n; ++k)
  {
    const auto d  = motionFromModel(models[std::size_t(k)], width, height);
    auto      &r  = out[std::size_t(k)].raw;
    const auto &p = out[std::size_t(k - 1)].raw;
    r.dx = p.dx + d.dx, r.dy = p.dy + d.dy, r.angle = p.angle + d.angle, r.logScale = p.logScale + d.logScale;
  }
  const int radius = std::max(0, params.radius);
  for (int k = 0; k < n; ++k)
  {
    const int lo = std::max(0, k - radius), hi = std::min(n - 1, k + radius);
    TrajectoryPoint sum;
    for (int j = lo; j <= hi; ++j)
    {
      const auto &r = out[std::size_t(j)].raw;
      sum.dx += r.dx, sum.dy += r.dy, sum.angle += r.angle, sum.logScale += r.logScale;
    }
    const double cnt = hi - lo + 1;
    auto        &f   = out[std::size_t(k)];
    f.smoothed       = {sum.dx / cnt, sum.dy / cnt, sum.angle / cnt, sum.logScale / cnt};
    f.correction     = {f.smoothed.dx - f.raw.dx, f.smoothed.dy - f.raw.dy, f.smoothed.angle - f.raw.angle,
                        f.smoothed.logScale - f.raw.logScale};
    f.warp           = warpParams(f.correction, width, height);
  }
  return out;
}

std::string trajectoryCsv(const std::vector<StabilizerFrame> &frames)
{
  std::string text = "frame,raw_dx,raw_dy,raw_angle,raw_scale,smooth_dx,smooth_dy,smooth_angle,smooth_scale,"
                     "corr_dx,corr_dy,corr_angle,corr_scale\n";
  char line[512];
  for (std::size_t k = 0; k < frames.size(); ++k)
  {
    const auto &f = frames[k];
    std::snprintf(line, sizeof line,
                  "%zu,%.4f,%.4f,%.5f,%.6f,%.4f,%.4f,%.5f,%.6f,%.4f,%.4f,%.5f,%.6f\n", k, f.raw.dx,
                  f.raw.dy, f.raw.angle, std::exp(f.raw.logScale), f.smoothed.dx, f.smoothed.dy,
                  f.smoothed.angle, std::exp(f.smoothed.logScale), f.correction.dx, f.correction.dy,
                  f.correction.angle, std::exp(f.correction.logScale));
    text += line;
  }
  return text;
}

} // namespace bda::filter
