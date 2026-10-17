#include "GlobalModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bda::me
{

namespace
{

struct Point
{
  double x, y;   //!< Block centre in the current picture
  double rx, ry; //!< Where it lands in the reference
};

using Params = std::array<double, 6>;

// libaom's LCG (av1/encoder/random.h): lcg_next, and lcg_randint from the high bits.
struct Lcg
{
  std::uint32_t state;
  int           randint(int n)
  {
    this->state = this->state * 1103515245u + 12345u;
    return int((std::uint64_t(this->state) * std::uint64_t(n)) >> 32);
  }
};

int minPoints(GmModelType type)
{
  switch (type)
  {
  case GmModelType::Translation:
    return 1;
  case GmModelType::RotZoom:
    return 2;
  default:
    return 3;
  }
}

/* Solve the n x n system a * x = b in place, Gaussian elimination with partial pivoting. False
 * when it is singular, which is what a degenerate sample (coincident or collinear points) gives.
 */
template <int N> bool solve(double (&a)[N][N], double (&b)[N], double (&x)[N])
{
  for (int c = 0; c < N; ++c)
  {
    int pivot = c;
    for (int r = c + 1; r < N; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
        pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-9)
      return false;
    if (pivot != c)
    {
      for (int k = 0; k < N; ++k)
        std::swap(a[c][k], a[pivot][k]);
      std::swap(b[c], b[pivot]);
    }
    for (int r = c + 1; r < N; ++r)
    {
      const double f = a[r][c] / a[c][c];
      for (int k = c; k < N; ++k)
        a[r][k] -= f * a[c][k];
      b[r] -= f * b[c];
    }
  }
  for (int r = N - 1; r >= 0; --r)
  {
    double s = b[r];
    for (int k = r + 1; k < N; ++k)
      s -= a[r][k] * x[k];
    x[r] = s / a[r][r];
  }
  return true;
}

/* Least squares over the chosen points. Coordinates are taken relative to (ox, oy) - the picture
 * centre - which keeps the normal equations well conditioned on large pictures; the result is
 * moved back to picture coordinates.
 */
bool fit(GmModelType type, const std::vector<Point> &pts, const std::vector<int> &idx, double ox, double oy,
         Params &out)
{
  if (int(idx.size()) < minPoints(type))
    return false;
  double a0 = 0, b0 = 0, c0 = 0, d0 = 0; // translation and linear part, centred coordinates
  double a1 = 0, b1 = 0;                 // affine second row (y' = c0 + a1 x + b1 y)
  if (type == GmModelType::Translation)
  {
    double sx = 0, sy = 0;
    for (int i : idx)
      sx += pts[i].rx - pts[i].x, sy += pts[i].ry - pts[i].y;
    out = {sx / idx.size(), sy / idx.size(), 1, 0, 0, 1};
    return true;
  }
  if (type == GmModelType::RotZoom)
  {
    // Unknowns (tx, ty, a, b): x' = tx + a x + b y, y' = ty - b x + a y.
    double m[4][4] = {}, v[4] = {}, s[4];
    for (int i : idx)
    {
      const double x = pts[i].x - ox, y = pts[i].y - oy, rx = pts[i].rx - ox, ry = pts[i].ry - oy;
      const double r1[4] = {1, 0, x, y}, r2[4] = {0, 1, y, -x};
      for (int p = 0; p < 4; ++p)
      {
        for (int q = 0; q < 4; ++q)
          m[p][q] += r1[p] * r1[q] + r2[p] * r2[q];
        v[p] += r1[p] * rx + r2[p] * ry;
      }
    }
    if (!solve<4>(m, v, s))
      return false;
    a0 = s[0], c0 = s[1], b0 = s[2], d0 = s[3];
    a1 = -d0, b1 = b0;
  }
  else
  {
    // Two independent rows: x' = a0 + b0 x + d0 y, y' = c0 + a1 x + b1 y.
    double m[3][3] = {}, vx[3] = {}, vy[3] = {}, sx[3], sy[3];
    for (int i : idx)
    {
      const double x = pts[i].x - ox, y = pts[i].y - oy, rx = pts[i].rx - ox, ry = pts[i].ry - oy;
      const double r[3] = {1, x, y};
      for (int p = 0; p < 3; ++p)
      {
        for (int q = 0; q < 3; ++q)
          m[p][q] += r[p] * r[q];
        vx[p] += r[p] * rx;
        vy[p] += r[p] * ry;
      }
    }
    double m2[3][3];
    std::copy(&m[0][0], &m[0][0] + 9, &m2[0][0]);
    if (!solve<3>(m, vx, sx) || !solve<3>(m2, vy, sy))
      return false;
    a0 = sx[0], b0 = sx[1], d0 = sx[2];
    c0 = sy[0], a1 = sy[1], b1 = sy[2];
  }
  // Back to picture coordinates: x' = ox + a0 + b0 (x - ox) + d0 (y - oy), likewise y'.
  out = {ox + a0 - b0 * ox - d0 * oy, oy + c0 - a1 * ox - b1 * oy, b0, d0, a1, b1};
  return true;
}

void apply(const Params &p, double x, double y, double &rx, double &ry)
{
  rx = p[0] + p[2] * x + p[3] * y;
  ry = p[1] + p[4] * x + p[5] * y;
}

// Inliers within `threshold` and their squared error.
int score(const Params &p, const std::vector<Point> &pts, double threshold, double &sse, std::vector<int> *inliers)
{
  const double t2 = threshold * threshold;
  int          n  = 0;
  sse             = 0;
  if (inliers)
    inliers->clear();
  for (int i = 0; i < int(pts.size()); ++i)
  {
    double rx, ry;
    apply(p, pts[i].x, pts[i].y, rx, ry);
    const double e = (rx - pts[i].rx) * (rx - pts[i].rx) + (ry - pts[i].ry) * (ry - pts[i].ry);
    if (e < t2)
    {
      ++n, sse += e;
      if (inliers)
        inliers->push_back(i);
    }
  }
  return n;
}

} // namespace

const char *gmModelTypeName(GmModelType type)
{
  switch (type)
  {
  case GmModelType::Translation:
    return "translation";
  case GmModelType::RotZoom:
    return "rotzoom";
  default:
    return "affine";
  }
}

double GmModelResult::scale() const { return std::sqrt(std::fabs(this->p[2] * this->p[5] - this->p[3] * this->p[4])); }

double GmModelResult::angleDegrees() const
{
  return std::atan2(this->p[4] - this->p[3], this->p[2] + this->p[5]) * 180.0 / M_PI;
}

GmModelResult fitGlobalModel(const MeFrameResult &me, int width, int height, const GmModelParams &params)
{
  GmModelResult out;
  out.type = params.type;

  std::vector<Point> pts;
  for (const auto &b : me.blocks)
  {
    // A static-bypass block is SVT deciding "no motion" without a search: a trustworthy (0,0), and
    // on a still frame pair the only evidence there is.
    if (b.block.size != params.fitSize)
      continue;
    const int s = blockSizeInPixels(b.block.size);
    if (b.block.x + s > width || b.block.y + s > height)
      continue;
    const double cx = b.block.x + s / 2.0, cy = b.block.y + s / 2.0;
    pts.push_back({cx, cy, cx + b.mv.x / 8.0, cy + b.mv.y / 8.0});
    out.blocks.push_back(b.block);
  }
  out.total = int(pts.size());
  out.inlier.assign(pts.size(), 0);

  const int k = minPoints(params.type);
  // libaom: fewer than minpts * MINPTS_MULTIPLIER points is no basis for a model.
  if (out.total < k * 5)
  {
    out.error = "too few points (" + std::to_string(out.total) + ")";
    return out;
  }
  const int    minInliers = std::max(int(0.1 * out.total), k);
  const double ox = width / 2.0, oy = height / 2.0;

  Lcg              rng{std::uint32_t(out.total)};
  Params           best{};
  int              bestCount = -1;
  double           bestSse   = 0;
  std::vector<int> sample(static_cast<std::size_t>(k));
  for (int t = 0; t < params.trials; ++t)
  {
    // Distinct indices, redrawing on a repeat (lcg_pick).
    for (int i = 0; i < k; ++i)
    {
      int v;
      do
        v = rng.randint(out.total);
      while (std::find(sample.begin(), sample.begin() + i, v) != sample.begin() + i);
      sample[std::size_t(i)] = v;
    }
    Params p;
    if (!fit(params.type, pts, sample, ox, oy, p))
      continue;
    double     sse;
    const int  n = score(p, pts, params.threshold, sse, nullptr);
    if (n > bestCount || (n == bestCount && sse < bestSse))
      best = p, bestCount = n, bestSse = sse;
  }
  if (bestCount < minInliers)
  {
    out.error = "too few inliers (" + std::to_string(std::max(bestCount, 0)) + " of " + std::to_string(out.total) + ")";
    return out;
  }

  // Refit on the inliers until their number stops growing.
  std::vector<int> inliers;
  score(best, pts, params.threshold, bestSse, &inliers);
  for (int r = 0; r < params.refines; ++r)
  {
    Params p;
    if (!fit(params.type, pts, inliers, ox, oy, p))
      break;
    std::vector<int> next;
    double           sse;
    const int        n = score(p, pts, params.threshold, sse, &next);
    if (n < bestCount || (n == bestCount && sse >= bestSse))
      break;
    const bool grew = n > bestCount;
    best = p, bestCount = n, bestSse = sse, inliers = next;
    if (!grew)
      break;
  }

  out.p       = best;
  out.inliers = bestCount;
  out.rms     = bestCount > 0 ? std::sqrt(bestSse / bestCount) : 0;
  for (int i : inliers)
    out.inlier[std::size_t(i)] = 1;
  return out;
}

MotionVector gmModelMvAt(const GmModelResult &model, double x, double y)
{
  double rx, ry;
  apply(model.p, x, y, rx, ry);
  return {std::int32_t(std::lround((rx - x) * 8)), std::int32_t(std::lround((ry - y) * 8))};
}

MeFrameResult synthesiseModelSeed(const GmModelResult &model, int width, int height)
{
  MeFrameResult out;
  const int     w = (width + 63) / 64 * 64, h = (height + 63) / 64 * 64;
  for (int s = 0; s < kBlockSizeCount; ++s)
  {
    const auto size = static_cast<BlockSize>(s);
    const int  n    = blockSizeInPixels(size);
    for (int y = 0; y < h; y += n)
      for (int x = 0; x < w; x += n)
      {
        MeBlockResult b;
        b.block = {x, y, size};
        b.mv    = gmModelMvAt(model, x + n / 2.0, y + n / 2.0);
        out.blocks.push_back(b);
      }
  }
  return out;
}

} // namespace bda::me
