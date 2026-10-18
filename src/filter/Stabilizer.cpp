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

// Centred moving average of radius R, window clamped to the sequence. Kept as the original sum /
// count so the output stays bit-identical to the radius-only smoother.
std::vector<double> movingAverage(const std::vector<double> &z, int radius)
{
  const int           n = int(z.size());
  std::vector<double> s(z.size());
  radius = std::max(0, radius);
  for (int k = 0; k < n; ++k)
  {
    const int lo = std::max(0, k - radius), hi = std::min(n - 1, k + radius);
    double    sum = 0;
    for (int j = lo; j <= hi; ++j)
      sum += z[std::size_t(j)];
    s[std::size_t(k)] = sum / (hi - lo + 1);
  }
  return s;
}

// Gaussian weights exp(-d^2 / 2 sigma^2), |d| <= ceil(3 sigma), window clamped and renormalised.
std::vector<double> gaussian(const std::vector<double> &z, double sigma)
{
  const int n = int(z.size());
  if (!(sigma > 0))
    return z;
  const int           half = int(std::ceil(3 * sigma));
  std::vector<double> w(std::size_t(half) + 1);
  for (int d = 0; d <= half; ++d)
    w[std::size_t(d)] = std::exp(-double(d) * d / (2 * sigma * sigma));
  std::vector<double> s(z.size());
  for (int k = 0; k < n; ++k)
  {
    const int lo = std::max(0, k - half), hi = std::min(n - 1, k + half);
    double    sum = 0, wsum = 0;
    for (int j = lo; j <= hi; ++j)
    {
      const double wj = w[std::size_t(std::abs(j - k))];
      sum += wj * z[std::size_t(j)], wsum += wj;
    }
    s[std::size_t(k)] = sum / wsum;
  }
  return s;
}

// Constant-velocity Kalman filter on one component; with `rts` the Rauch-Tung-Striebel smoother.
std::vector<double> kalman(const std::vector<double> &z, double q, double r, bool rts)
{
  const int n = int(z.size());
  if (n == 0)
    return {};
  q = std::max(0.0, q), r = std::max(0.0, r);
  struct State
  {
    double x0, x1;           // position, velocity
    double p00, p01, p11;    // symmetric covariance
  };
  std::vector<State> filtered(z.size()), predicted(z.size());
  const double       big = 1e6 * std::max(r, 1e-9);
  State              st{z[0], 0, big, 0, big}; // prior for frame 0
  for (int k = 0; k < n; ++k)
  {
    if (k > 0)
    {
      // Predict: x = F x, P = F P F^T + q G G^T with G = [1/2, 1].
      const auto &f = filtered[std::size_t(k - 1)];
      st.x0         = f.x0 + f.x1;
      st.x1         = f.x1;
      st.p00        = f.p00 + 2 * f.p01 + f.p11 + q / 4;
      st.p01        = f.p01 + f.p11 + q / 2;
      st.p11        = f.p11 + q;
    }
    predicted[std::size_t(k)] = st;
    // Update with z_k: H = [1, 0].
    const double s = st.p00 + r;
    const double k0 = st.p00 / s, k1 = st.p01 / s;
    const double y  = z[std::size_t(k)] - st.x0;
    State        u;
    u.x0                     = st.x0 + k0 * y;
    u.x1                     = st.x1 + k1 * y;
    u.p00                    = (1 - k0) * st.p00;
    u.p01                    = (1 - k0) * st.p01;
    u.p11                    = st.p11 - k1 * st.p01;
    filtered[std::size_t(k)] = u;
  }
  std::vector<double> out(z.size());
  if (!rts)
  {
    for (int k = 0; k < n; ++k)
      out[std::size_t(k)] = filtered[std::size_t(k)].x0;
    return out;
  }
  // Backward: C = P_k|k F^T (P_k+1|k)^-1, x_k|n = x_k|k + C (x_k+1|n - x_k+1|k).
  double sx0 = filtered[std::size_t(n - 1)].x0, sx1 = filtered[std::size_t(n - 1)].x1;
  out[std::size_t(n - 1)] = sx0;
  for (int k = n - 2; k >= 0; --k)
  {
    const auto &f = filtered[std::size_t(k)], &p = predicted[std::size_t(k + 1)];
    // P F^T for F = [[1, 1], [0, 1]]: [[p00 + p01, p01], [p01 + p11, p11]].
    const double a00 = f.p00 + f.p01, a01 = f.p01, a10 = f.p01 + f.p11, a11 = f.p11;
    const double det = p.p00 * p.p11 - p.p01 * p.p01;
    const double i00 = p.p11 / det, i01 = -p.p01 / det, i11 = p.p00 / det;
    const double c00 = a00 * i00 + a01 * i01, c01 = a00 * i01 + a01 * i11;
    const double c10 = a10 * i00 + a11 * i01, c11 = a10 * i01 + a11 * i11;
    const double d0 = sx0 - p.x0, d1 = sx1 - p.x1;
    sx0                 = f.x0 + c00 * d0 + c01 * d1;
    sx1                 = f.x1 + c10 * d0 + c11 * d1;
    out[std::size_t(k)] = sx0;
  }
  return out;
}

std::vector<double> smoothSeries(const std::vector<double> &z, const StabilizerParams &params)
{
  switch (params.method)
  {
  case SmoothingMethod::MovingAverage:
    return movingAverage(z, params.radius);
  case SmoothingMethod::Gaussian:
    return gaussian(z, params.sigma);
  case SmoothingMethod::KalmanCausal:
    return kalman(z, params.kalmanQ, params.kalmanR, false);
  case SmoothingMethod::KalmanRts:
    return kalman(z, params.kalmanQ, params.kalmanR, true);
  }
  return z;
}

} // namespace

const char *smoothingMethodName(SmoothingMethod method)
{
  switch (method)
  {
  case SmoothingMethod::MovingAverage:
    return "Moving average";
  case SmoothingMethod::Gaussian:
    return "Gaussian";
  case SmoothingMethod::KalmanCausal:
    return "Kalman causal";
  case SmoothingMethod::KalmanRts:
    return "Kalman RTS";
  }
  return "";
}

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

  // Each component is smoothed on its own; component c of a point is reached through `field`.
  double TrajectoryPoint::*const fields[] = {&TrajectoryPoint::dx, &TrajectoryPoint::dy, &TrajectoryPoint::angle,
                                             &TrajectoryPoint::logScale};
  std::vector<double>            z(std::size_t(std::max(0, n)));
  for (const auto field : fields)
  {
    for (int k = 0; k < n; ++k)
      z[std::size_t(k)] = out[std::size_t(k)].raw.*field;
    const auto s = smoothSeries(z, params);
    for (int k = 0; k < n; ++k)
      out[std::size_t(k)].smoothed.*field = s[std::size_t(k)];
  }

  for (auto &f : out)
  {
    f.correction = {f.smoothed.dx - f.raw.dx, f.smoothed.dy - f.raw.dy, f.smoothed.angle - f.raw.angle,
                    f.smoothed.logScale - f.raw.logScale};
    f.warp       = warpParams(f.correction, width, height);
  }
  return out;
}

std::vector<StabilizerFrame> stabilizeByFrame(const std::map<int, AffineParams> &modelsByFrame,
                                              int                                firstFrame,
                                              int                                lastFrame,
                                              int                                width,
                                              int                                height,
                                              const StabilizerParams            &params)
{
  if (lastFrame < firstFrame)
    return {};
  std::vector<AffineParams> models(std::size_t(lastFrame - firstFrame + 1), kAffineIdentity);
  for (int k = 1; k < int(models.size()); ++k)
    if (const auto it = modelsByFrame.find(firstFrame + k); it != modelsByFrame.end())
      models[std::size_t(k)] = it->second;
  return stabilize(models, width, height, params);
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
