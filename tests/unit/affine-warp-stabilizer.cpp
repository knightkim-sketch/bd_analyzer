// Unit test: the bilinear affine warp (src/filter/AffineWarp) and the DIS trajectory smoother
// (src/filter/Stabilizer), built without Qt or OpenCV.
//
// Expectations come from the definitions, not from the code's output:
//   * the identity warp and an integer shift copy samples exactly (edges replicated);
//   * a half-pel shift on a horizontal ramp gives the mean of two neighbours, rounded;
//   * 10-bit values survive (no 8-bit clip), and the result is clipped to the bit depth;
//   * chroma: a pure translation halves exactly for every siting; with a zoom the translation
//     follows b_c = (A o + b - o) / 2, checked on a warp whose fixed point is a luma position;
//   * whole 4:2:0 frame: a shift by (4, 2) moves luma by (4, 2) and chroma by (2, 1);
//   * stabilizer: identity models give zero trajectory and identity warps; a known jitter on a
//     pan yields the raw trajectory, the clamped moving average and correction = smoothed - raw;
//     a model's rotation and zoom come back as angle and log scale; the correction warp moves
//     content by the correction;
//   * smoothing methods: Gaussian = the weighted average computed here from exp(-d^2/2 sigma^2),
//     |d| <= ceil(3 sigma), clamped and renormalised; identity models give a zero trajectory for
//     every method; a steady pan needs no interior correction (MovingAverage, Gaussian exactly,
//     Kalman RTS within 1e-6 px - the diffuse velocity prior); Kalman causal frame k does not
//     change when later frames do; on pan + jitter RTS leaves less jitter than causal;
//     stabilizeByFrame fills frames without a model with the identity.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "filter/AffineWarp.h"
#include "filter/Stabilizer.h"

using namespace bda::filter;

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol; }

std::vector<std::uint16_t> texture(int w, int h, int bitDepth)
{
  std::vector<std::uint16_t> p(std::size_t(w) * h);
  std::uint32_t              st = 12345;
  for (auto &v : p)
  {
    st = st * 1103515245u + 12345u;
    v  = std::uint16_t((st >> 16) % (1u << bitDepth));
  }
  return p;
}

} // namespace

int main()
{
  constexpr int W = 32, H = 24;

  // Identity and integer shift.
  {
    const auto                 in = texture(W, H, 8);
    std::vector<std::uint16_t> out;
    warpPlane(in.data(), W, H, kAffineIdentity, 8, out);
    check(out == in, "identity warp copies the plane");

    // out(x, y) = in(x + 3, y - 2): the content moves by (-3, +2).
    warpPlane(in.data(), W, H, {3, -2, 1, 0, 0, 1}, 8, out);
    bool copy = true;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
      {
        const int sx = std::min(W - 1, x + 3), sy = std::max(0, y - 2);
        copy         = copy && out[std::size_t(y) * W + x] == in[std::size_t(sy) * W + sx];
      }
    check(copy, "integer shift (3, -2) is a copy, edges replicated");
  }

  // Half-pel bilinear on a ramp (10 bit, values above 255).
  {
    std::vector<std::uint16_t> ramp(std::size_t(W) * H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        ramp[std::size_t(y) * W + x] = std::uint16_t(500 + 7 * x);
    std::vector<std::uint16_t> out;
    warpPlane(ramp.data(), W, H, {0.5, 0, 1, 0, 0, 1}, 10, out);
    bool half = true;
    for (int x = 0; x < W - 1; ++x)
      half = half && out[std::size_t(5) * W + x] == std::lround((500 + 7 * x + 500 + 7 * (x + 1)) / 2.0);
    check(half, "half-pel shift on a 10-bit ramp: mean of the two neighbours, rounded");
    check(out[std::size_t(5) * W + W - 1] == 500 + 7 * (W - 1), "last column: right neighbour replicated");

    std::vector<std::uint16_t> hi(std::size_t(W) * H, 1023);
    warpPlane(hi.data(), W, H, {0.3, 0.7, 1.01, 0, 0, 1}, 10, out);
    check(out[100] == 1023, "10-bit maximum survives interpolation (no 8-bit clip)");
  }

  // Chroma convention.
  {
    const AffineParams t{6, -4, 1, 0, 0, 1};
    for (auto siting : {ChromaSiting::Centre, ChromaSiting::Left, ChromaSiting::TopLeft})
    {
      const auto c = chromaWarpParams(t, siting);
      check(near(c[0], 3) && near(c[1], -2) && c[2] == 1 && c[5] == 1,
            "pure translation (6, -4) is (3, -2) on chroma for siting " + std::to_string(int(siting)));
    }
    // A 2x zoom whose fixed point is luma position f = (10, 6): q = f + 0.5 (p - f).
    const double       fx = 10, fy = 6;
    const AffineParams z{fx - 0.5 * fx, fy - 0.5 * fy, 0.5, 0, 0, 0.5};
    const struct
    {
      ChromaSiting s;
      double       ox, oy;
    } sitings[] = {{ChromaSiting::Centre, 0, 0}, {ChromaSiting::Left, -0.5, 0}, {ChromaSiting::TopLeft, -0.5, -0.5}};
    for (const auto &e : sitings)
    {
      const auto c = chromaWarpParams(z, e.s);
      // The chroma position that lies on luma f must also be fixed: luma = 2 chroma + o.
      const double cx = (fx - e.ox) / 2, cy = (fy - e.oy) / 2;
      check(near(c[0] + c[2] * cx + c[3] * cy, cx) && near(c[1] + c[4] * cx + c[5] * cy, cy),
            "zoom: chroma fixed point sits on the luma fixed point, siting " + std::to_string(int(e.s)));
    }
  }

  // Whole 4:2:0 frame shifted by (4, 2): luma by (4, 2), chroma by (2, 1).
  {
    Frame420u16 f;
    f.width = W, f.height = H;
    f.y     = texture(W, H, 8);
    f.u     = texture(W / 2, H / 2, 8);
    f.v     = f.u;
    for (auto &v : f.v)
      v = std::uint16_t(255 - v);
    const auto o = warpFrame420(f, {-4, -2, 1, 0, 0, 1}, 8); // content moves by (+4, +2)
    check(o.y[std::size_t(10) * W + 20] == f.y[std::size_t(8) * W + 16] &&
              o.u[std::size_t(5) * (W / 2) + 10] == f.u[std::size_t(4) * (W / 2) + 8] &&
              o.v[std::size_t(5) * (W / 2) + 10] == f.v[std::size_t(4) * (W / 2) + 8],
          "frame warp: luma moves (4, 2), chroma (2, 1)");
    Frame420u16 mono;
    mono.width = W, mono.height = H, mono.y = f.y;
    const auto m = warpFrame420(mono, kAffineIdentity, 8);
    check(m.y == mono.y && m.u.empty(), "4:0:0 frame: luma only");
  }

  // Stabilizer: identity in, identity out.
  {
    const std::vector<AffineParams> models(10, kAffineIdentity);
    const auto                      st = stabilize(models, 320, 192);
    bool                            id = st.size() == 10;
    for (const auto &f : st)
      for (int i = 0; i < 6; ++i)
        id = id && near(f.warp[std::size_t(i)], kAffineIdentity[std::size_t(i)]) && f.raw.dx == 0 && f.correction.dx == 0;
    check(id, "identity models: zero trajectory, identity warps");
  }

  // Stabilizer: pan of 2 px/frame in x plus a known jitter j_k in y.
  {
    const int           n = 12;
    const double        j[n] = {0, 3, -2, 1, 4, -3, 0, 2, -1, 3, -4, 1};
    std::vector<AffineParams> models(n, kAffineIdentity);
    for (int k = 1; k < n; ++k)
    {
      // Content at frame k is at (2k, j_k): ref = cur - (motion k-1 -> k).
      models[std::size_t(k)][0] = -2;
      models[std::size_t(k)][1] = -(j[k] - j[k - 1]);
    }
    const int  R  = 2;
    const auto st = stabilize(models, 320, 192, {SmoothingMethod::MovingAverage, R});
    bool raw = true, smooth = true, corr = true;
    for (int k = 0; k < n; ++k)
    {
      raw = raw && near(st[std::size_t(k)].raw.dx, 2.0 * k) && near(st[std::size_t(k)].raw.dy, j[k]);
      const int lo = std::max(0, k - R), hi = std::min(n - 1, k + R);
      double    sx = 0, sy = 0;
      for (int i = lo; i <= hi; ++i)
        sx += 2.0 * i, sy += j[i];
      sx /= hi - lo + 1, sy /= hi - lo + 1;
      smooth = smooth && near(st[std::size_t(k)].smoothed.dx, sx) && near(st[std::size_t(k)].smoothed.dy, sy);
      corr   = corr && near(st[std::size_t(k)].correction.dx, sx - 2.0 * k) && near(st[std::size_t(k)].correction.dy, sy - j[k]);
    }
    check(raw, "raw trajectory = accumulated content motion (2k, j_k)");
    check(smooth, "smoothed = clamped centred moving average, radius 2");
    check(corr, "correction = smoothed - raw");
    // Interior pan is untouched by the average: frame 5's x correction is 0.
    check(near(st[5].correction.dx, 0), "a steady pan needs no x correction away from the ends");
    // The warp moves content by the correction: content at p_in lands on p_in + d.
    const auto  &w  = st[4].warp;
    const double dx = st[4].correction.dx, dy = st[4].correction.dy;
    const double px = 100 + dx, py = 50 + dy;
    check(near(w[0] + w[2] * px + w[3] * py, 100) && near(w[1] + w[4] * px + w[5] * py, 50),
          "the correction warp reads p_out - correction (pure translation)");
    const auto csv = trajectoryCsv(st);
    check(csv.rfind("frame,raw_dx,raw_dy,raw_angle,raw_scale,smooth_dx", 0) == 0 &&
              std::count(csv.begin(), csv.end(), '\n') == n + 1,
          "trajectory CSV: header and one row per frame");
  }

  // Rotation and zoom about the centre: a model with s = 1.02, phi = 1.5 deg comes back as content
  // motion angle -1.5 deg, log scale -log 1.02, no translation at the centre.
  {
    const int    w = 320, h = 192;
    const double s = 1.02, phi = 1.5 * 3.14159265358979323846 / 180, cx = w / 2.0, cy = h / 2.0;
    const double a = s * std::cos(phi), b = s * std::sin(phi);
    // ref = c + sR(phi)(cur - c), as a rotzoom model: p2 = a, p3 = -b, p4 = b, p5 = a.
    const AffineParams m{cx - (a * cx - b * cy), cy - (b * cx + a * cy), a, -b, b, a};
    const auto         d = motionFromModel(m, w, h);
    check(near(d.angle, -1.5, 1e-9) && near(d.logScale, -std::log(1.02), 1e-12) && near(d.dx, 0, 1e-9) && near(d.dy, 0, 1e-9),
          "rotzoom model about the centre -> angle -1.5 deg, log scale -log 1.02, no shift");
    // warpParams of a pure rotation: the inverse rotation about the centre; the centre is fixed.
    const auto p = warpParams({0, 0, 2.0, 0}, w, h);
    check(near(p[0] + p[2] * cx + p[3] * cy, cx) && near(p[1] + p[4] * cx + p[5] * cy, cy) && near(p[2], std::cos(2 * 3.14159265358979323846 / 180)),
          "correction warp of a rotation keeps the centre");
    // Composing the model's inverse content motion with that correction gives the identity.
    const auto inv = warpParams(d, w, h); // moves content by the k-1 -> k motion
    // Content motion then model: reading the model's output through inv returns p.
    const double qx = 37, qy = 141;
    const double rx = m[0] + m[2] * qx + m[3] * qy, ry = m[1] + m[4] * qx + m[5] * qy; // ref position of cur q
    // inv maps an output (cur) position to the input (ref) position: inv(q) must equal model(q).
    check(near(inv[0] + inv[2] * qx + inv[3] * qy, rx, 1e-9) && near(inv[1] + inv[4] * qx + inv[5] * qy, ry, 1e-9),
          "warpParams(motionFromModel(m)) == m for a similarity about the centre");
  }

  // Smoothing methods. A pan of 2 px/frame in x plus pseudo-random jitter in y, as models.
  {
    const SmoothingMethod all[] = {SmoothingMethod::MovingAverage, SmoothingMethod::Gaussian,
                                   SmoothingMethod::KalmanCausal, SmoothingMethod::KalmanRts};
    const auto panJitter = [](int n, double vx, double vy, double amp, std::uint32_t seed) {
      std::vector<double> jy(std::size_t(n), 0.0);
      for (int k = 1; k < n; ++k)
      {
        seed                = seed * 1103515245u + 12345u;
        jy[std::size_t(k)] = amp * (double((seed >> 16) & 0x7fff) / 32767.0 * 2 - 1);
      }
      std::vector<AffineParams> models(std::size_t(n), kAffineIdentity);
      for (int k = 1; k < n; ++k)
      {
        // Content at frame k is at (vx k, vy k + jy_k); the model maps cur -> ref = cur - motion.
        models[std::size_t(k)][0] = -vx;
        models[std::size_t(k)][1] = -(vy + jy[std::size_t(k)] - jy[std::size_t(k - 1)]);
      }
      return std::make_pair(models, jy);
    };

    // Gaussian against the definition, computed here.
    {
      const int    n  = 30;
      const auto   pj = panJitter(n, 2, 0, 3, 7);
      StabilizerParams gp;
      gp.method = SmoothingMethod::Gaussian, gp.sigma = 2.5;
      const auto st   = stabilize(pj.first, 320, 192, gp);
      const int  half = int(std::ceil(3 * gp.sigma)); // 8
      bool       ok   = half == 8 && int(st.size()) == n;
      for (int k = 0; k < n && ok; ++k)
      {
        double sx = 0, sy = 0, sw = 0;
        for (int j = std::max(0, k - half); j <= std::min(n - 1, k + half); ++j)
        {
          const double w = std::exp(-double(j - k) * (j - k) / (2 * gp.sigma * gp.sigma));
          sx += w * 2.0 * j, sy += w * pj.second[std::size_t(j)], sw += w;
        }
        ok = near(st[std::size_t(k)].smoothed.dx, sx / sw, 1e-9) && near(st[std::size_t(k)].smoothed.dy, sy / sw, 1e-9) &&
             near(st[std::size_t(k)].correction.dy, sy / sw - pj.second[std::size_t(k)], 1e-9);
      }
      check(ok, "Gaussian (sigma 2.5): smoothed = clamped, renormalised exp(-d^2/2s^2) average over |d| <= 8");
    }

    // Identity models: zero trajectory and identity warps for every method.
    for (const auto m : all)
    {
      StabilizerParams p;
      p.method      = m;
      const auto st = stabilize(std::vector<AffineParams>(25, kAffineIdentity), 320, 192, p);
      bool       ok = st.size() == 25;
      for (const auto &f : st)
      {
        ok = ok && f.raw.dx == 0 && f.raw.dy == 0 && f.smoothed.dx == 0 && f.smoothed.dy == 0 && f.smoothed.angle == 0 &&
             f.smoothed.logScale == 0 && f.correction.dx == 0 && f.correction.dy == 0;
        for (int i = 0; i < 6; ++i)
          ok = ok && near(f.warp[std::size_t(i)], kAffineIdentity[std::size_t(i)]);
      }
      check(ok, std::string("identity models -> zero trajectory, identity warps: ") + smoothingMethodName(m));
    }

    // Steady pan (2, -0.5) px/frame: no correction away from the ends.
    {
      const int n    = 60;
      const auto pan = panJitter(n, 2, -0.5, 0, 1).first;
      struct Case
      {
        SmoothingMethod m;
        int             margin; // frames whose window reaches an end
        double          tol;
      };
      for (const auto c : {Case{SmoothingMethod::MovingAverage, 15, 1e-9}, Case{SmoothingMethod::Gaussian, 18, 1e-9},
                           Case{SmoothingMethod::KalmanRts, 0, 1e-6}})
      {
        StabilizerParams p;
        p.method      = c.m;
        const auto st = stabilize(pan, 320, 192, p);
        double     worst = 0;
        for (int k = c.margin; k < n - c.margin; ++k)
          worst = std::max({worst, std::abs(st[std::size_t(k)].correction.dx), std::abs(st[std::size_t(k)].correction.dy)});
        std::cout << "  info  steady pan, " << smoothingMethodName(c.m) << ": worst interior |correction| " << worst
                  << " px" << std::endl;
        check(worst <= c.tol, std::string("steady pan needs no interior correction: ") + smoothingMethodName(c.m));
      }
    }

    // Kalman causal: frame k ignores frames after k; RTS does not (the check has teeth).
    {
      const int  n = 50, k0 = 20;
      auto       a = panJitter(n, 1, 0.5, 2, 11).first, b = a;
      for (int k = k0 + 1; k < n; ++k)
        b[std::size_t(k)][0] += 3, b[std::size_t(k)][1] -= 1.5 * (k % 3);
      StabilizerParams p;
      p.method       = SmoothingMethod::KalmanCausal;
      const auto ca  = stabilize(a, 320, 192, p), cb = stabilize(b, 320, 192, p);
      p.method       = SmoothingMethod::KalmanRts;
      const auto ra  = stabilize(a, 320, 192, p), rb = stabilize(b, 320, 192, p);
      bool same = true, rtsMoved = false;
      for (int k = 0; k <= k0; ++k)
      {
        same     = same && ca[std::size_t(k)].smoothed.dx == cb[std::size_t(k)].smoothed.dx &&
               ca[std::size_t(k)].smoothed.dy == cb[std::size_t(k)].smoothed.dy;
        rtsMoved = rtsMoved || std::abs(ra[std::size_t(k)].smoothed.dx - rb[std::size_t(k)].smoothed.dx) > 1e-3;
      }
      check(same && !(ca[std::size_t(k0 + 1)].smoothed.dx == cb[std::size_t(k0 + 1)].smoothed.dx),
            "Kalman causal: smoothed[k] for k <= 20 is bit-identical when frames > 20 change");
      check(rtsMoved, "Kalman RTS: the same change does move frames <= 20 (backward pass)");
    }

    // Pan + jitter: RTS leaves less jitter than causal (rms of smoothed - true pan, all frames).
    {
      const int  n  = 120;
      const auto pj = panJitter(n, 2, 0.25, 3, 5);
      double     res[2]{};
      for (int i = 0; i < 2; ++i)
      {
        StabilizerParams p;
        p.method      = i ? SmoothingMethod::KalmanRts : SmoothingMethod::KalmanCausal;
        const auto st = stabilize(pj.first, 320, 192, p);
        double     v  = 0;
        for (int k = 0; k < n; ++k)
          v += std::pow(st[std::size_t(k)].smoothed.dx - 2.0 * k, 2) + std::pow(st[std::size_t(k)].smoothed.dy - 0.25 * k, 2);
        res[i] = std::sqrt(v / n);
      }
      std::cout << "  info  pan + jitter (+-3 px): rms residual causal " << res[0] << " px, RTS " << res[1] << " px"
                << std::endl;
      check(res[1] < res[0], "Kalman RTS residual jitter < Kalman causal");
    }

    // stabilizeByFrame: absolute frame keys, gaps and out-of-range keys are the identity.
    {
      const AffineParams m12{-3, 1, 1, 0, 0, 1}, m14{2, -4, 1, 0, 0, 1};
      const std::map<int, AffineParams> byFrame{{10, {-50, -50, 1, 0, 0, 1}}, {12, m12}, {14, m14}, {20, {9, 9, 1, 0, 0, 1}}};
      std::vector<AffineParams>         vec(6, kAffineIdentity);
      vec[2] = m12, vec[4] = m14;
      bool ok = true;
      for (const auto m : all)
      {
        StabilizerParams p;
        p.method       = m;
        const auto got = stabilizeByFrame(byFrame, 10, 15, 320, 192, p);
        const auto exp = stabilize(vec, 320, 192, p);
        ok             = ok && got.size() == 6;
        for (std::size_t k = 0; ok && k < 6; ++k)
          ok = got[k].raw.dx == exp[k].raw.dx && got[k].raw.dy == exp[k].raw.dy &&
               got[k].smoothed.dx == exp[k].smoothed.dx && got[k].correction.dy == exp[k].correction.dy;
      }
      const auto g = stabilizeByFrame(byFrame, 10, 15, 320, 192);
      ok = ok && g[0].raw.dx == 0 && g[1].raw.dx == 0 && near(g[2].raw.dx, 3) && near(g[2].raw.dy, -1) &&
           near(g[3].raw.dx, 3) && near(g[4].raw.dx, 1) && near(g[4].raw.dy, 3) && near(g[5].raw.dx, 1);
      check(ok, "stabilizeByFrame(10..15): frames 11, 13, 15 without a model are still, keys 10 and 20 ignored");
      check(stabilizeByFrame(byFrame, 5, 4, 320, 192).empty(), "stabilizeByFrame: empty range -> no frames");
    }
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
