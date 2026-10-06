#include "GlobalMotion.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "MeCost.h"

namespace bda::me
{

namespace
{

/* Row-wise and column-wise cumulative sums over a plane and its whole border.
 *
 *   rowSum(y, x) = sum of p(u, y) for -pad <= u < x      x in [-pad, w + pad]
 *   colSum(y, x) = sum of p(x, v) for -pad <= v < y      y in [-pad, h + pad]
 *
 * One pass each. Any window's profile along either axis, at any displacement inside the border,
 * is then the difference of two entries - no per-window or per-displacement summing.
 */
class PrefixPlanes
{
public:
  explicit PrefixPlanes(const MePlane &p) : pad(p.pad()), w(p.width()), h(p.height())
  {
    this->cols = this->w + 2 * this->pad + 1; // one more entry than samples: the empty prefix
    this->rowsAll = this->h + 2 * this->pad;
    this->sx.assign(std::size_t(this->rowsAll) * std::size_t(this->cols), 0);
    this->sy.assign(std::size_t(this->rowsAll + 1) * std::size_t(this->cols), 0);
    for (int y = -this->pad; y < this->h + this->pad; ++y)
    {
      std::int32_t run = 0;
      for (int x = -this->pad; x < this->w + this->pad; ++x)
      {
        run += p.at(x, y);
        this->sxAt(y, x + 1) = run;
        this->syAt(y + 1, x) = this->syAt(y, x) + p.at(x, y);
      }
    }
  }

  std::int32_t rowSum(int y, int x) const { return this->sx[this->index(y + this->pad, x)]; }
  std::int32_t colSum(int y, int x) const { return this->sy[this->index(y + this->pad, x)]; }

private:
  std::size_t index(int row, int x) const
  {
    return std::size_t(row) * std::size_t(this->cols) + std::size_t(x + this->pad);
  }
  std::int32_t &sxAt(int y, int x) { return this->sx[this->index(y + this->pad, x)]; }
  std::int32_t &syAt(int y, int x) { return this->sy[this->index(y + this->pad, x)]; }

  int                       pad, w, h, cols{}, rowsAll{};
  std::vector<std::int32_t> sx, sy;
};

/* Smooth a profile; samples past either end replicate the end sample. Symmetric kernels, so the
 * minimum does not move.
 */
std::vector<std::int32_t> smooth(const std::vector<std::int32_t> &v, const GmSmoothing kind)
{
  if (kind == GmSmoothing::Off || v.empty())
    return v;
  const int  n  = int(v.size());
  const auto at = [&v, n](int i) { return v[std::size_t(std::clamp(i, 0, n - 1))]; };
  std::vector<std::int32_t> out(v.size());
  for (int i = 0; i < n; ++i)
    out[std::size_t(i)] =
        kind == GmSmoothing::Binomial3
            ? (at(i - 1) + 2 * at(i) + at(i + 1) + 2) >> 2
            : (at(i - 2) + 4 * at(i - 1) + 6 * at(i) + 4 * at(i + 1) + at(i + 2) + 8) >> 4;
  return out;
}

int roundedMean(std::int64_t sum, int count) { return int((sum + count / 2) / count); }

/* One axis of one window: the current profile o (length L) against the reference profile r over
 * displacements -R..R. rExt holds r for positions -R .. L+R-1, i.e. rExt[k + R] = r[k].
 */
class AxisProfiles
{
public:
  AxisProfiles(std::vector<std::int32_t> o, std::vector<std::int32_t> rExt, int R)
      : o(std::move(o)), rExt(std::move(rExt)), R(R), L(int(this->o.size()))
  {
    // Rsum[k] = sum of rExt[0..k), so the reference mean at any displacement is O(1).
    this->rsum.assign(this->rExt.size() + 1, 0);
    for (std::size_t k = 0; k < this->rExt.size(); ++k)
      this->rsum[k + 1] = this->rsum[k] + this->rExt[k];
    for (const auto v : this->o)
      this->sumO += v;
  }

  std::int64_t r(int i, int d) const { return this->rExt[std::size_t(i + d + this->R)]; }
  std::int64_t rSum(int d) const
  {
    return this->rsum[std::size_t(d + this->R + this->L)] - this->rsum[std::size_t(d + this->R)];
  }

  /* Gain of org ~ a * ref + b with the reference at displacement d, Q12, clamped to 0.5..2.0.
   * 4096 when the reference profile is flat - a slope fitted to noise means nothing.
   *
   * Fitted where the profiles line up. At d = 0, as the design first had it, a moving picture
   * gives two unrelated profiles, the slope comes out near 0 and clamps to 0.5, and that wrong
   * gain then spoils the cost at the true displacement too: measured on a smooth texture moving
   * 8 px along one axis, 5 to 10 of 16 windows found it with the d = 0 gain against 16 of 16
   * without a gain at all.
   */
  int fitGain(int d, int profileWidth, int flatGreyLevels) const
  {
    const std::int64_t sumR = this->rSum(d);
    std::int64_t       sumOR = 0, sumRR = 0;
    for (int i = 0; i < this->L; ++i)
    {
      sumOR += std::int64_t(this->o[std::size_t(i)]) * this->r(i, d);
      sumRR += this->r(i, d) * this->r(i, d);
    }
    const std::int64_t num  = std::int64_t(this->L) * sumOR - this->sumO * sumR;
    const std::int64_t den  = std::int64_t(this->L) * sumRR - sumR * sumR; // L^2 * variance
    const std::int64_t flat = std::int64_t(this->L) * this->L * flatGreyLevels * flatGreyLevels *
                              std::int64_t(profileWidth) * profileWidth;
    if (den <= flat)
      return 4096;
    // 128-bit: at source resolution a profile sample reaches 960 * 255, and num * 4096 overflows
    // 64 bits there.
    const __int128     scaled = __int128(num) * 4096;
    const std::int64_t q      = std::int64_t((scaled >= 0 ? scaled + den / 2 : scaled - den / 2) / den);
    return int(std::clamp<std::int64_t>(q, 2048, 8192));
  }

  /* Full search over -R..R. Outward from 0, replacing only on a strict improvement: a tie keeps the
   * smaller |d|.
   */
  GmAxisResult search(int aQ, bool removeMean, const GmParams &params) const
  {
    const int  mo   = removeMean ? roundedMean(this->sumO, this->L) : 0;
    const auto cost = [&](int d) {
      const int    mr  = removeMean ? roundedMean(this->rSum(d), this->L) : 0;
      std::int64_t acc = 0;
      for (int i = 0; i < this->L; ++i)
      {
        const std::int64_t oc = std::int64_t(this->o[std::size_t(i)]) - mo;
        const std::int64_t rc = (std::int64_t(aQ) * (this->r(i, d) - mr)) >> 12;
        acc += std::llabs(oc - rc);
      }
      return acc;
    };

    GmAxisResult out;
    out.aQ       = aQ;
    out.costZero = cost(0);
    out.costBest = out.costZero;
    for (int m = 1; m <= this->R; ++m)
      for (const int d : {-m, m})
      {
        const auto c = cost(d);
        if (c < out.costBest)
        {
          out.costBest = c;
          out.d        = d;
        }
      }
    out.g1 = out.d != 0 && out.costBest * params.g1Num < out.costZero * params.g1Den;
    return out;
  }

private:
  std::vector<std::int32_t> o, rExt;
  std::vector<std::int64_t> rsum;
  std::int64_t              sumO{};
  int                       R, L;
};

/* Profiles straight from a full-resolution plane, without cumulative sums: at 4K two prefix planes
 * per picture would be 132 MB each, for a step that touches each window once.
 *
 * rowProfile: one sample per row y0 + t, t in [from, to), summed over columns [x0, x1).
 * colProfile: one sample per column x0 + t, t in [from, to), summed over rows [y0, y1).
 */
std::vector<std::int32_t> rowProfile(const MePlane &p, int x0, int x1, int y0, int from, int to)
{
  std::vector<std::int32_t> out(std::size_t(to - from));
  for (int t = from; t < to; ++t)
  {
    const std::uint8_t *row = p.row(y0 + t);
    std::int32_t        sum = 0;
    for (int x = x0; x < x1; ++x)
      sum += row[x];
    out[std::size_t(t - from)] = sum;
  }
  return out;
}

std::vector<std::int32_t> colProfile(const MePlane &p, int y0, int y1, int x0, int from, int to)
{
  std::vector<std::int32_t> out(std::size_t(to - from), 0);
  for (int y = y0; y < y1; ++y)
  {
    const std::uint8_t *row = p.row(y);
    for (int t = from; t < to; ++t)
      out[std::size_t(t - from)] += row[x0 + t];
  }
  return out;
}

} // namespace

int gmDownsampleExponent(const int width)
{
  int n = 0;
  while ((width >> n) > 512)
    ++n;
  return n;
}

GmFrameResult estimateGlobalMotion(const MePlane &cur, const MePlane &ref, const GmParams &params)
{
  GmFrameResult out;
  if (cur.empty() || ref.empty() || cur.width() != ref.width() || cur.height() != ref.height())
  {
    out.error = "the two pictures are empty or of different sizes";
    return out;
  }

  out.n  = gmDownsampleExponent(cur.width());
  out.wd = cur.width() >> out.n;
  out.hd = cur.height() >> out.n;

  /* The upper bound is half the smallest window, not half the picture: without it a 288p picture
   * searches further than its windows are tall and the reference window stops overlapping its own
   * position at all.
   */
  const int upper = std::min(out.wd / kGmGrid, out.hd / kGmGrid) / 2;
  if (upper < 1)
  {
    out.error = "the picture is too small for a 4x4 window grid";
    return out;
  }
  out.rDs = std::min(std::max(params.rOrig >> out.n, 4), upper);

  // Border: the search range plus the smoothing taps, so every reference profile sample is real
  // picture or its replicated edge, never past the plane.
  const int  pad   = out.rDs + 2;
  /* 1/2^n as n successive step-2 meanpools, not one step-2^n call. odyssey's meanpool shifts by
   * the step rather than by log2(step^2), which is an average only at step 2 and 4: at step 8 it
   * divides 64 samples by 256 and a 4K picture came out at a quarter of its brightness, at step 16
   * (8K) it collapses to 0 and 1. Step 2 is the kernel's exact range, so cascading it keeps the
   * encoder's own block and gives a real mean (rounded per stage).
   */
  const auto scale = [&](const MePlane &p) {
    if (out.n == 0)
      return MePlane::fromLuma8(&p.at(0, 0), p.width(), p.height(), p.stride(), pad);
    MePlane level = p.downscaleOdysseyMeanpool(2, out.n == 1 ? pad : 0);
    for (int k = 1; k < out.n; ++k)
      level = level.downscaleOdysseyMeanpool(2, k == out.n - 1 ? pad : 0);
    return level;
  };
  const MePlane curD = scale(cur);
  const MePlane refD = scale(ref);

  const PrefixPlanes curS(curD), refS(refD);
  const int          R = out.rDs;

  /* Refinement at source resolution: +-refineRange around the coarse vector, which can sit up to
   * R << n away - the reference copy gets a border wide enough for both, so every read is real
   * picture or its replicated edge. Skipped at n = 0, where there is no finer resolution.
   */
  const bool    refine = params.refineRange > 0 && out.n > 0;
  const int     rr     = params.refineRange;
  const MePlane refF   = refine ? MePlane::fromLuma8(&ref.at(0, 0), ref.width(), ref.height(),
                                                     ref.stride(), (R << out.n) + rr + 2)
                                : MePlane();

  for (int j = 0; j < kGmGrid; ++j)
    for (int i = 0; i < kGmGrid; ++i)
    {
      GmWindowResult win;
      win.winI   = i;
      win.winJ   = j;
      win.x0     = out.wd * i / kGmGrid;
      win.y0     = out.hd * j / kGmGrid;
      const int x1 = out.wd * (i + 1) / kGmGrid;
      const int y1 = out.hd * (j + 1) / kGmGrid;
      win.w      = x1 - win.x0;
      win.h      = y1 - win.y0;

      // Smooth the whole extended reference first, then drop the two guard samples per side, so
      // the samples near -R and L+R see real neighbours rather than replicated ones.
      const auto trimGuard = [](std::vector<std::int32_t> v) {
        return std::vector<std::int32_t>(v.begin() + 2, v.end() - 2);
      };

      /* Vertical profile (one sample per row, summed over the window's columns) -> dy. The
       * reference sums its columns shifted by colShift: the horizontal motion, once known.
       */
      std::vector<std::int32_t> oV(std::size_t(win.h));
      for (int t = 0; t < win.h; ++t)
        oV[std::size_t(t)] = curS.rowSum(win.y0 + t, x1) - curS.rowSum(win.y0 + t, win.x0);
      const auto vertical = [&](int colShift) {
        std::vector<std::int32_t> rV(std::size_t(win.h + 2 * R + 4));
        for (int t = -R - 2; t < win.h + R + 2; ++t)
          rV[std::size_t(t + R + 2)] = refS.rowSum(win.y0 + t, x1 + colShift) -
                                       refS.rowSum(win.y0 + t, win.x0 + colShift);
        return AxisProfiles(smooth(oV, params.smoothing),
                            trimGuard(smooth(rV, params.smoothing)), R);
      };

      // Horizontal profile (one sample per column, summed over the window's rows) -> dx.
      std::vector<std::int32_t> oH(std::size_t(win.w));
      for (int t = 0; t < win.w; ++t)
        oH[std::size_t(t)] = curS.colSum(y1, win.x0 + t) - curS.colSum(win.y0, win.x0 + t);
      const auto horizontal = [&](int rowShift) {
        std::vector<std::int32_t> rH(std::size_t(win.w + 2 * R + 4));
        for (int t = -R - 2; t < win.w + R + 2; ++t)
          rH[std::size_t(t + R + 2)] = refS.colSum(y1 + rowShift, win.x0 + t) -
                                       refS.colSum(win.y0 + rowShift, win.x0 + t);
        return AxisProfiles(smooth(oH, params.smoothing),
                            trimGuard(smooth(rH, params.smoothing)), R);
      };

      const bool removeMean = params.brightness != GmBrightness::Off;

      /* Pass 1: each axis on the window's own span, without a gain.
       *
       * Pass 2: each axis again, with the reference profile summed over the span the other axis
       * found. A window's vertical profile sums its columns; when the picture also moved
       * horizontally, part of those columns are not the same columns in the reference, and that
       * part is noise in the profile. Measured on a smooth texture moving (+24, -16) at 288p, 1 to
       * 6 of 16 windows found the motion from pass 1 alone. This shifts only where the profile is
       * summed - the two axes are still searched and gated separately, never fused.
       *
       * Pass 3, with DC + gain: the gain fitted at the displacement pass 2 found, then the search
       * once more with it.
       */
      const auto firstX = horizontal(0).search(4096, removeMean, params);
      const auto firstY = vertical(0).search(4096, removeMean, params);
      const auto profX  = horizontal(firstY.d);
      const auto profY  = vertical(firstX.d);
      win.x             = profX.search(4096, removeMean, params);
      win.y             = profY.search(4096, removeMean, params);
      if (params.brightness == GmBrightness::DcGain)
      {
        win.x = profX.search(profX.fitGain(win.x.d, win.h, params.flatGreyLevels), true, params);
        win.y = profY.search(profY.fitGain(win.y.d, win.w, params.flatGreyLevels), true, params);
      }

      // Gate 2 on the pair gate 1 left standing: an axis that failed gate 1 contributes 0.
      const int dx   = win.x.g1 ? win.x.d : 0;
      const int dy   = win.y.g1 ? win.y.d : 0;
      win.sad2dZero  = blockSad(curD, win.x0, win.y0, refD, win.x0, win.y0, win.w, win.h);
      win.sad2dBest  = (dx == 0 && dy == 0)
                           ? win.sad2dZero
                           : blockSad(curD, win.x0, win.y0, refD, win.x0 + dx, win.y0 + dy, win.w,
                                      win.h);
      win.g2         = win.sad2dBest * params.g2Mul < win.sad2dZero;

      // Per axis: the pair must also beat itself with that axis taken out. When the other axis was
      // rejected this is the pair gate again, so it costs nothing new.
      const auto sad = [&](int ddx, int ddy) {
        return (ddx == 0 && ddy == 0) ? win.sad2dZero
                                      : blockSad(curD, win.x0, win.y0, refD, win.x0 + ddx,
                                                 win.y0 + ddy, win.w, win.h);
      };
      win.sad2dNoX   = dx == 0 ? win.sad2dBest : sad(0, dy);
      win.sad2dNoY   = dy == 0 ? win.sad2dBest : sad(dx, 0);
      win.g2x        = win.sad2dBest * params.g2Mul < win.sad2dNoX;
      win.g2y        = win.sad2dBest * params.g2Mul < win.sad2dNoY;
      win.acceptX    = win.x.g1 && win.g2 && win.g2x;
      win.acceptY    = win.y.g1 && win.g2 && win.g2y;
      win.x.dFull = win.x.d << out.n;
      win.y.dFull = win.y.d << out.n;

      /* Refine the value at source resolution, around the coarse vector. Gates stay as decided
       * above - the refinement moves the vector to full-pel, it does not re-judge it, so accepted
       * and rejected mean the same thing with and without it.
       *
       * Same passes as the coarse search: the other axis's coarse shift decides where the
       * reference profile is summed, DC first, then the gain fitted where that search landed.
       */
      if (refine)
      {
        const int X0 = win.x0 << out.n, Y0 = win.y0 << out.n;
        // The last column and row of windows take the source pixels the downsampling dropped.
        const int X1 = i == kGmGrid - 1 ? cur.width() : x1 << out.n;
        const int Y1 = j == kGmGrid - 1 ? cur.height() : y1 << out.n;
        const int Dx = win.x.dFull, Dy = win.y.dFull;

        const auto refineAxis = [&](std::vector<std::int32_t> o, std::vector<std::int32_t> r,
                                    int profileWidth, GmAxisResult &axis) {
          const AxisProfiles prof(smooth(o, params.smoothing),
                                  trimGuard(smooth(r, params.smoothing)), rr);
          auto best = prof.search(4096, removeMean, params);
          if (params.brightness == GmBrightness::DcGain)
            best = prof.search(prof.fitGain(best.d, profileWidth, params.flatGreyLevels), true,
                               params);
          axis.dFull += best.d;
          axis.refineCost = best.costBest;
        };
        // dy: rows around Y0 + Dy, columns shifted by the coarse dx.
        refineAxis(rowProfile(cur, X0, X1, Y0, 0, Y1 - Y0),
                   rowProfile(refF, X0 + Dx, X1 + Dx, Y0 + Dy, -rr - 2, Y1 - Y0 + rr + 2),
                   X1 - X0, win.y);
        // dx: columns around X0 + Dx, rows shifted by the coarse dy.
        refineAxis(colProfile(cur, Y0, Y1, X0, 0, X1 - X0),
                   colProfile(refF, Y0 + Dy, Y1 + Dy, X0 + Dx, -rr - 2, X1 - X0 + rr + 2),
                   Y1 - Y0, win.x);
      }

      win.dxQpel = win.x.dFull * 4;
      win.dyQpel = win.y.dFull * 4;
      out.windows.push_back(win);
    }
  return out;
}

} // namespace bda::me
