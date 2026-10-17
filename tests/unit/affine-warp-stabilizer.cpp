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
//     content by the correction.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
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
    const auto st = stabilize(models, 320, 192, {R});
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

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
