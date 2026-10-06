// Unit test: frame-level global motion by integral projection (src/me/GlobalMotion), and the
// Qt-free Y4M / raw luma reader its CLI uses.
//
// The cases are V1-V3 of docs/ai/30-designs/global-motion-design.md section 5:
//   V1  pure translation - every window must find it, to within one downsampled pixel, with the
//       right sign. A wrong sign is the failure that does not look like one: odyssey measured that
//       the right vector saves 9.6% on a panning clip and the negated one saves nothing.
//   V2  the same with a brightness change a*I + b on the current picture - the vector must not move.
//   V3  a picture against itself - nothing may be accepted.
//
// Expectations are written from the geometry, not captured from the estimator's own output.
//
// The texture is value noise rather than a sum of sinusoids: a periodic texture has false minima
// one period away, which would make "a shift beyond the search range is not found" a property of
// the texture instead of the estimator.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "me/GlobalMotion.h"
#include "me/MePlane.h"
#include "me/YuvLumaReader.h"

using namespace bda::me;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++failures;
}

double lattice(int ix, int iy, unsigned seed)
{
  unsigned h = unsigned(ix) * 374761393u + unsigned(iy) * 668265263u + seed * 2246822519u;
  h          = (h ^ (h >> 13)) * 1274126177u;
  return double((h ^ (h >> 16)) & 0xffff) / 65535.0;
}

// Smooth value noise on a grid of the given spacing, 0..1.
double noise(double x, double y, double grid, unsigned seed)
{
  const double gx = x / grid, gy = y / grid;
  const int    ix = int(std::floor(gx)), iy = int(std::floor(gy));
  const double fx = gx - ix, fy = gy - iy;
  const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
  const double a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
  const double c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
  return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy;
}

/* Two textures, both 50..170 so a gain of 1.25 with +20 and of 0.8 with -20 stay inside 0..255.
 *
 *   Noise    two octaves of value noise, 16 and 5 px - detail at every window size.
 *   OneOverF six octaves, 64 px down to 2 px, amplitude 0.6 per octave - closer to a natural picture,
 *            where the large scales dominate. Harder for projections: a window's profile is mostly
 *            a few slow undulations.
 */
enum class Texture
{
  Noise,
  OneOverF,
};
Texture g_texture = Texture::Noise;

double texel(int x, int y)
{
  if (g_texture == Texture::Noise)
    return 50.0 + 70.0 * noise(x, y, 16, 1) + 50.0 * noise(x, y, 5, 2);
  double v = 0, amp = 1, total = 0;
  for (int octave = 0, grid = 64; octave < 6; ++octave, grid /= 2)
  {
    v += amp * noise(x, y, grid, unsigned(octave + 1));
    total += amp;
    amp *= 0.6;
  }
  return 50.0 + 120.0 * v / total;
}

/* The picture of content that moved right by dx and down by dy from the reference:
 * cur(x, y) = ref(x - dx, y - dy). Generated from the texture itself, so the content beyond the
 * frame edge is real texture, not a replicated border.
 */
MePlane picture(int w, int h, int dx, int dy, double gain = 1.0, double offset = 0.0)
{
  MePlane p(w, h, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      p.at(x, y) = std::uint8_t(std::clamp(std::lround(gain * texel(x - dx, y - dy) + offset), 0L, 255L));
  return p;
}

struct Shift
{
  int dx, dy;
};

std::string name(const char *what, int w, int h, Shift s)
{
  char text[96];
  std::snprintf(text, sizeof(text), "%s %dx%d, content moved (%+d, %+d)", what, w, h, s.dx, s.dy);
  return text;
}

/* Check one axis of every window against the content motion D on that axis.
 *
 * In range: found to within 2^n source pixels, with the sign the consumer expects (d = -D),
 * and accepted when D is not zero. Zero motion on an axis: that axis is not accepted. Beyond the
 * search range: not accepted - there is nothing right to report.
 */
void checkAxis(const GmFrameResult &r, bool xAxis, int D, const std::string &label, bool requireAccept)
{
  const int step     = 1 << r.n;
  const int expected = -D; // source pixels
  int       found = 0, accepted = 0, badQpel = 0;
  for (const auto &win : r.windows)
  {
    const auto &axis   = xAxis ? win.x : win.y;
    const bool  accept = xAxis ? win.acceptX : win.acceptY;
    const int   qpel   = xAxis ? win.dxQpel : win.dyQpel;
    if (qpel != axis.dFull * 4)
      ++badQpel; // the quarter-pel value must be the source-pixel vector << 2
    /* Design V1: "within the quantisation 2^n" - one downsampled pixel either way. With the
     * source-resolution refinement (n > 0) there is no quantisation left, so it must be exact.
     */
    const int tolerance = r.n > 0 && GmParams{}.refineRange > 0 ? 0 : step;
    if (std::abs(axis.dFull - expected) <= tolerance)
      ++found;
    if (accept)
      ++accepted;
  }
  const char *ax = xAxis ? "x" : "y";
  if (D == 0)
    check(accepted == 0, label + ": no window accepts the still axis " + ax);
  else if (std::abs(D) / step <= r.rDs)
  {
    check(found == int(r.windows.size()),
          label + ": all 16 windows find " + ax + " = " + std::to_string(expected) + " px (" +
              std::to_string(found) + "/16)");
    if (requireAccept)
      check(accepted == int(r.windows.size()),
            label + ": and accept it (" + std::to_string(accepted) + "/16)");
  }
  else
    check(accepted == 0, label + ": beyond the search range on " + ax + ", nothing accepted (" +
                             std::to_string(accepted) + " accepted)");
  check(badQpel == 0, label + ": quarter-pel " + ax + " is the source-pixel vector << 2");
}

void v1(int w, int h, const std::vector<Shift> &shifts)
{
  const MePlane ref = picture(w, h, 0, 0);
  for (const auto s : shifts)
  {
    const auto r     = estimateGlobalMotion(picture(w, h, s.dx, s.dy), ref, GmParams{});
    const auto label =
        name(g_texture == Texture::Noise ? "V1 noise" : "V1 1/f", w, h, s);
    if (!r.ok())
    {
      check(false, label + ": " + r.error);
      continue;
    }
    checkAxis(r, true, s.dx, label, true);
    checkAxis(r, false, s.dy, label, true);
  }
}

void writeFile(const std::string &path, const std::string &bytes)
{
  std::ofstream f(path, std::ios::binary);
  f.write(bytes.data(), std::streamsize(bytes.size()));
}

} // namespace

int main()
{
  std::cout << "== n and search range ==" << std::endl;
  check(gmDownsampleExponent(3840) == 3, "3840 -> n = 3");
  check(gmDownsampleExponent(1920) == 2, "1920 -> n = 2");
  check(gmDownsampleExponent(1280) == 2, "1280 -> n = 2");
  check(gmDownsampleExponent(512) == 0, "512 -> n = 0");
  {
    // Design section 2: R_ds = clamp(R_orig >> n, 4, smallest window / 2).
    const auto r288 = estimateGlobalMotion(picture(512, 288, 0, 0), picture(512, 288, 0, 0), {});
    check(r288.rDs == 36, "512x288: R_ds 36 (the half-window cap, not 128)");
    const auto r1080 =
        estimateGlobalMotion(picture(1920, 1080, 0, 0), picture(1920, 1080, 0, 0), {});
    // R_orig 256 >> 2 = 64, capped at half the smallest window (67 / 2 = 33).
    check(r1080.n == 2 && r1080.wd == 480 && r1080.hd == 270 && r1080.rDs == 33,
          "1920x1080: n 2, 480x270, R_ds 33 (the half-window cap)");
    const auto r4k = estimateGlobalMotion(picture(3840, 2160, 0, 0), picture(3840, 2160, 0, 0), {});
    check(r4k.n == 3 && r4k.rDs == 32, "3840x2160: n 3, R_ds 32 - 256 px, the full R_orig");
    check(r1080.windows.size() == 16 && r1080.windows[1].winI == 1 && r1080.windows[1].winJ == 0,
          "16 windows, win_i the column and win_j the row");
    check(r1080.windows[5].x0 == 120 && r1080.windows[5].y0 == 67 && r1080.windows[5].w == 120 &&
              r1080.windows[5].h == 68,
          "window (1, 1) of 480x270 is 120x68 at (120, 67)");

    // V3: a picture against itself.
    int accepted = 0, nonzero = 0, sadNonzero = 0;
    for (const auto &win : r1080.windows)
    {
      accepted += win.acceptX + win.acceptY + win.x.g1 + win.y.g1 + win.g2;
      nonzero += (win.x.d != 0) + (win.y.d != 0);
      sadNonzero += win.sad2dZero != 0;
    }
    std::cout << "== V3: a picture against itself ==" << std::endl;
    check(nonzero == 0, "d* = 0 on both axes of every window");
    check(accepted == 0, "neither gate passes anywhere");
    check(sadNonzero == 0, "and the zero-vector SAD is 0");
  }

  std::cout << "== sign: content moved right by 24 px ==" << std::endl;
  {
    const auto r = estimateGlobalMotion(picture(512, 288, 24, 0), picture(512, 288, 0, 0), {});
    // The sign is the point here; the value gets the same 2^n tolerance as V1 (4 qpel at n = 0).
    int negative = 0, exact = 0;
    for (const auto &win : r.windows)
    {
      negative += win.dxQpel < 0 && std::abs(win.dxQpel + 96) <= 4 && win.acceptX;
      exact += win.dxQpel == -96;
    }
    check(negative == 16, "every window reports dx = -96 quarter-pel (= -24 px) to within 1 px, "
                          "accepted (" + std::to_string(negative) + "/16, exact in " +
                              std::to_string(exact) + ")");
  }

  /* Refinement only moves values: with it off, every window must make the same coarse decision
   * (same d, same gates), and the vector must be exactly d << n.
   */
  std::cout << "== refinement off vs on: same coarse decisions ==" << std::endl;
  {
    GmParams off;
    off.refineRange = 0;
    for (const auto texture : {Texture::Noise, Texture::OneOverF})
    {
      g_texture        = texture;
      const MePlane c  = picture(1920, 1080, 24, -16);
      const MePlane rf = picture(1920, 1080, 0, 0);
      const auto    a  = estimateGlobalMotion(c, rf, off);
      const auto    b  = estimateGlobalMotion(c, rf, GmParams{});
      int same = 0, plain = 0, moved = 0;
      for (std::size_t k = 0; k < a.windows.size(); ++k)
      {
        const auto &x = a.windows[k], &y = b.windows[k];
        same += x.x.d == y.x.d && x.y.d == y.y.d && x.acceptX == y.acceptX && x.acceptY == y.acceptY;
        plain += x.x.dFull == x.x.d << a.n && x.y.dFull == x.y.d << a.n;
        moved += std::max(std::abs(y.x.dFull - (y.x.d << b.n)), std::abs(y.y.dFull - (y.y.d << b.n)));
      }
      const std::string t = texture == Texture::Noise ? "noise" : "1/f";
      check(same == 16, t + ": refinement leaves every coarse d and gate unchanged");
      check(plain == 16, t + ": with refinement off the vector is d << n");
      std::cout << "        " << t << ": refinement moved vectors by " << moved
                << " px in total over 16 windows" << std::endl;
    }
    g_texture = Texture::Noise;
    const auto r288 = estimateGlobalMotion(picture(512, 288, 8, 0), picture(512, 288, 0, 0), {});
    bool unrefined = true;
    for (const auto &w : r288.windows)
      unrefined = unrefined && w.x.refineCost == 0 && w.y.refineCost == 0;
    check(unrefined, "n = 0: no refinement step runs");
  }

  const std::vector<Shift> shifts = {{0, 0},   {8, 0},     {-8, 0},  {0, 8},    {0, -8},
                                     {24, 16}, {-24, -16}, {24, -16}, {64, 64}, {-64, -64},
                                     {120, 0}, {-120, 0}};
  for (const auto texture : {Texture::Noise, Texture::OneOverF})
  {
    g_texture = texture;
    const char *t = texture == Texture::Noise ? "noise" : "1/f";
    std::cout << "== V1 (" << t << "): translation, 512x288 (n = 0, R 36) ==" << std::endl;
    v1(512, 288, shifts);
    std::cout << "== V1 (" << t << "): translation, 1920x1080 (n = 2, R 33 = 132 px) ==" << std::endl;
    v1(1920, 1080, shifts);
    std::cout << "== V1 (" << t << "): translation, 3840x2160 (n = 3, 3 x meanpool step 2) ==" << std::endl;
    v1(3840, 2160, {{24, -16}});
  }
  g_texture = Texture::Noise;

  /* 8K, n = 4. A single step-16 meanpool divides 256 samples by 65536 and leaves a picture of 0s and
   * 1s; the cascade of step-2 pools keeps it a real mean. This is the case that tells them apart.
   */
  std::cout << "== V1 (noise): translation, 7680x4320 (n = 4, 4 x meanpool step 2) ==" << std::endl;
  v1(7680, 4320, {{32, -16}});

  /* V2: the same translation with a brightness change on the current picture. The design's
   * criterion is "the same MV as V1", window by window - brightness must not move the vector.
   * Acceptance is reported, not required: gate 2 is a plain 2-D SAD with no brightness
   * compensation (section 3.7), so a strong gain change can legitimately fail it.
   */
  std::cout << "== V2: translation with brightness a*I + b on the current picture ==" << std::endl;
  for (const auto texture : {Texture::Noise, Texture::OneOverF})
  {
    g_texture       = texture;
    const char *t   = texture == Texture::Noise ? "noise" : "1/f";
    const Shift s{24, -16};
    const MePlane ref = picture(512, 288, 0, 0);
    const auto base = estimateGlobalMotion(picture(512, 288, s.dx, s.dy), ref, GmParams{});
    for (const double a : {0.8, 1.0, 1.25})
      for (const double b : {-20.0, 0.0, 20.0})
      {
        const auto r = estimateGlobalMotion(picture(512, 288, s.dx, s.dy, a, b), ref, GmParams{});
        int same = 0, accepted = 0;
        for (std::size_t k = 0; k < r.windows.size(); ++k)
        {
          same += r.windows[k].x.d == base.windows[k].x.d && r.windows[k].y.d == base.windows[k].y.d;
          accepted += r.windows[k].acceptX && r.windows[k].acceptY;
        }
        char label[96];
        std::snprintf(label, sizeof(label),
                      "V2 %s a %.2f b %+.0f: same vector as V1 in all 16 windows (%d/16), both "
                      "axes accepted in %d/16",
                      t, a, b, same, accepted);
        check(same == 16, label);
      }
  }
  g_texture = Texture::Noise;

  std::cout << "== reader ==" << std::endl;
  {
    // 4x2 luma, 2x1 per chroma plane: 12 bytes a frame. Frame 1 carries a frame parameter.
    const std::string y4m = std::string("YUV4MPEG2 W4 H2 F25:1 Ip A1:1 C420jpeg\n") + "FRAME\n" +
                            std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8) + "uuvv" +
                            "FRAME Ixyz\n" + std::string("\x11\x12\x13\x14\x15\x16\x17\x18", 8) +
                            "uuvv" + "FRAME\n" + "trunc";
    writeFile("/tmp/bda-gm-reader.y4m", y4m);
    YuvLumaReader rd;
    std::string   error;
    check(rd.openY4m("/tmp/bda-gm-reader.y4m", error), "an 8-bit Y4M opens " + error);
    check(rd.width() == 4 && rd.height() == 2 && rd.bitDepth() == 8, "size and depth from the header");
    check(rd.frameCount() == 2, "two whole frames; the truncated third is dropped");
    const auto f1 = rd.readLuma(1, 2, error);
    check(!f1.empty() && f1.at(0, 0) == 0x11 && f1.at(3, 1) == 0x18,
          "frame 1 luma read past a FRAME line with parameters");
    check(!f1.empty() && f1.at(-2, -2) == 0x11, "and its border replicates the corner");

    // 10-bit: 512 (0x0200) and 1023 (0x03ff), little endian; shifted down to 128 and 255.
    std::string y10 = "YUV4MPEG2 W2 H2 C420p10\nFRAME\n";
    y10 += std::string("\x00\x02\xff\x03\x00\x02\xff\x03", 8) + std::string(4, '\0');
    writeFile("/tmp/bda-gm-reader10.y4m", y10);
    YuvLumaReader rd10;
    check(rd10.openY4m("/tmp/bda-gm-reader10.y4m", error) && rd10.bitDepth() == 10, "a 10-bit Y4M opens");
    const auto p10 = rd10.readLuma(0, 0, error);
    check(!p10.empty() && p10.at(0, 0) == 128 && p10.at(1, 0) == 255,
          "10-bit luma arrives shifted down to 8 bits (>> 2, as odyssey does)");

    writeFile("/tmp/bda-gm-reader444.y4m", "YUV4MPEG2 W4 H2 C444\nFRAME\n" + std::string(24, 'x'));
    YuvLumaReader rd444;
    const bool    refused = !rd444.openY4m("/tmp/bda-gm-reader444.y4m", error);
    check(refused && !error.empty(), "4:4:4 is refused with a reason: " + error);

    writeFile("/tmp/bda-gm-reader.yuv", std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8) +
                                            "uuvv" + std::string("\x21\x22\x23\x24\x25\x26\x27\x28", 8) +
                                            "uuvv");
    YuvLumaReader raw;
    check(raw.openRaw("/tmp/bda-gm-reader.yuv", 4, 2, 8, error) && raw.frameCount() == 2,
          "raw 4:2:0 with a given size: two frames");
    const auto r1 = raw.readLuma(1, 0, error);
    check(!r1.empty() && r1.at(2, 1) == 0x27, "raw frame 1 luma");
    check(raw.readLuma(2, 0, error).empty() && !error.empty(), "a frame past the end is refused");
    std::remove("/tmp/bda-gm-reader.y4m");
    std::remove("/tmp/bda-gm-reader10.y4m");
    std::remove("/tmp/bda-gm-reader444.y4m");
    std::remove("/tmp/bda-gm-reader.yuv");
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  return failures == 0 ? 0 : 1;
}
