// Unit test: the luma filter chain behind the Convolution filter pane (src/filter/FilterChain).
//
// Expectations come from the formulas, not from the code's own output:
//   * Custom 3x3 is the old pane's kernel: a box on a horizontal ramp 4x gives the ramp back inside
//     and (0 + 0 + 4) / 3 -> 1 at the replicated left edge;
//   * a cascade is the stages in order: Box, Box equals one Box applied to the output of the other;
//   * a disabled stage takes no part, in the samples or in the cache key;
//   * thresholds are in 8-bit units: 128 is 512 on a 10-bit plane;
//   * GM compensation undoes a known similarity warp, and compares against the previous frame run
//     through the stages in front of it - with identical frames the difference is zero only then;
//   * a written Y4M opens again with me::YuvLumaReader with the same size, depth and frame count,
//     and its luma is what was written (10-bit 400 reads back as 8-bit 100).
// Built with and without BDA_HAVE_OPENCV; the OpenCV checks are skipped without it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>

#include "filter/FilterChain.h"
#include "filter/Y4mWriter.h"
#include "me/YuvLumaReader.h"

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

LumaPlane makePlane(int w, int h, int bitDepth)
{
  LumaPlane p;
  p.width    = w;
  p.height   = h;
  p.bitDepth = bitDepth;
  p.samples.assign(std::size_t(w) * h, 0);
  return p;
}

std::uint16_t &at(LumaPlane &p, int x, int y) { return p.samples[std::size_t(y) * p.width + x]; }

// Smooth random texture, so feature detectors and RANSAC have something to hold on to.
LumaPlane texture(int w, int h, int bitDepth, unsigned seed)
{
  auto                                    p = makePlane(w, h, bitDepth);
  std::mt19937                            rng(seed);
  std::uniform_int_distribution<int>      u(0, p.maxValue());
  std::vector<double>                     raw(p.samples.size());
  for (auto &v : raw)
    v = u(rng);
  // 5x5 box, twice, by hand - no OpenCV in the test's own expectations.
  for (int pass = 0; pass < 2; pass++)
  {
    auto src = raw;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
      {
        double s = 0;
        for (int dy = -2; dy <= 2; dy++)
          for (int dx = -2; dx <= 2; dx++)
            s += src[std::size_t(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1)];
        raw[std::size_t(y) * w + x] = s / 25;
      }
  }
  // Stretch to the full range again, the blur squeezed it towards the middle.
  const auto [lo, hi] = std::minmax_element(raw.begin(), raw.end());
  const double span   = (*hi > *lo) ? (*hi - *lo) : 1.0; // a 1x1 frame has nothing to stretch
  for (std::size_t i = 0; i < raw.size(); i++)
    p.samples[i] = std::uint16_t(std::lround((raw[i] - *lo) / span * p.maxValue()));
  return p;
}

FilterStage custom3x3(std::initializer_list<double> k)
{
  auto s   = FilterStage::withDefaults(StageType::Custom3x3);
  s.params = std::vector<double>(k);
  return s;
}

#ifdef BDA_HAVE_OPENCV
double meanAbsDiff(const LumaPlane &a, const LumaPlane &b, int border)
{
  double s = 0;
  int    n = 0;
  for (int y = border; y < a.height - border; y++)
    for (int x = border; x < a.width - border; x++)
    {
      const auto i = std::size_t(y) * a.width + x;
      s += std::abs(int(a.samples[i]) - int(b.samples[i]));
      n++;
    }
  return n ? s / n : 0;
}

#endif

} // namespace

int main()
{
  std::cout << "catalog" << std::endl;
  check(stageInfo(StageType::Custom3x3) != nullptr, "Custom 3x3 is always offered");
#ifdef BDA_HAVE_OPENCV
  check(stageCatalog().size() == 18, "with OpenCV all 18 stages are offered");
#else
  check(stageCatalog().size() == 1, "without OpenCV only Custom 3x3 is offered");
#endif
  for (const auto &info : stageCatalog())
    check(FilterStage::withDefaults(info.type).params.size() == info.params.size(),
          info.name + ": defaults fill every parameter");

  std::cout << "custom 3x3" << std::endl;
  {
    auto ramp = makePlane(8, 4, 8);
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 8; x++)
        at(ramp, x, y) = std::uint16_t(4 * x);
    auto identity = ramp;
    FilterChain{{custom3x3({0, 0, 0, 0, 1, 0, 0, 0, 0})}}.apply(identity);
    check(identity.samples == ramp.samples, "the identity kernel changes nothing");

    auto box = ramp;
    FilterChain{{custom3x3({1, 1, 1, 1, 1, 1, 1, 1, 1})}}.apply(box);
    check(at(box, 3, 1) == 12, "box on the ramp gives the ramp back inside (12)");
    check(at(box, 0, 2) == 1, "replicated left edge: (0 + 0 + 4) / 3 -> 1");
    check(at(box, 7, 2) == 27, "replicated right edge: (24 + 28 + 28) / 3 -> 27");

    auto edge = ramp;
    FilterChain{{custom3x3({-1, 0, 1, -2, 0, 2, -1, 0, 1})}}.apply(edge);
    check(at(edge, 3, 1) == 32, "zero-sum kernel is not divided: Sobel x of slope 4 is 32");

    auto twice = ramp, once = ramp;
    FilterChain{{custom3x3({1, 1, 1, 1, 1, 1, 1, 1, 1}), custom3x3({1, 1, 1, 1, 1, 1, 1, 1, 1})}}
        .apply(twice);
    FilterChain{{custom3x3({1, 1, 1, 1, 1, 1, 1, 1, 1})}}.apply(once);
    FilterChain{{custom3x3({1, 1, 1, 1, 1, 1, 1, 1, 1})}}.apply(once);
    check(twice.samples == once.samples, "a two-stage cascade is the stages applied in order");
  }

  std::cout << "enable and key" << std::endl;
  {
    auto        blur = custom3x3({1, 1, 1, 1, 1, 1, 1, 1, 1});
    auto        off  = blur;
    off.enabled      = false;
    FilterChain withOff{{blur, off}};
    FilterChain plain{{blur}};
    auto        a = texture(32, 16, 8, 1), b = a;
    withOff.apply(a);
    plain.apply(b);
    check(a.samples == b.samples, "a disabled stage does not touch the samples");
    check(withOff.key() == plain.key(), "a disabled stage does not change the key");
    check(FilterChain{{off}}.isNoOp() && FilterChain{{off}}.key() == FilterChain{}.key(),
          "a chain with nothing enabled is a no-op with the empty key");
    auto other      = blur;
    other.params[0] = 2;
    check(FilterChain{{other}}.key() != plain.key(), "a parameter change changes the key");
  }

  std::cout << "Y4M writer" << std::endl;
  {
    Y4mFormat mono10;
    mono10.width    = 16;
    mono10.height   = 8;
    mono10.bitDepth = 10;
    mono10.mono     = true;
    Y4mWriter   writer;
    std::string error;
    check(!writer.open("filter-chain-mono10.y4m", mono10, error), "10-bit 4:0:0 is refused");
    std::remove("filter-chain-mono10.y4m");
  }
  for (const int depth : {8, 10})
  {
    Y4mFormat fmt;
    fmt.width          = 16;
    fmt.height         = 8;
    fmt.bitDepth       = depth;
    fmt.fpsNumerator   = 25;
    const auto path    = std::string("filter-chain-test-") + std::to_string(depth) + ".y4m";
    const int  value   = depth == 8 ? 100 : 400;
    std::vector<std::uint8_t> y(std::size_t(fmt.lumaBytes())), uv(std::size_t(fmt.chromaBytes()));
    for (std::size_t i = 0; i < y.size(); i += (depth > 8 ? 2 : 1))
    {
      y[i] = std::uint8_t(value & 0xff);
      if (depth > 8)
        y[i + 1] = std::uint8_t(value >> 8);
    }
    Y4mWriter   writer;
    std::string error;
    bool        ok = writer.open(path, fmt, error);
    for (int f = 0; ok && f < 3; f++)
      ok = writer.writeFrame(y.data(), uv.data(), uv.data(), error);
    writer = Y4mWriter{}; // closes and flushes the file
    check(ok && Y4mWriter::header(fmt).find(depth > 8 ? "C420p10" : "C420jpeg") != std::string::npos,
          std::to_string(depth) + "-bit: written with the right colour tag");

    bda::me::YuvLumaReader reader;
    const bool opened = reader.openY4m(path, error);
    check(opened && reader.width() == 16 && reader.height() == 8 && reader.bitDepth() == depth &&
              reader.frameCount() == 3,
          std::to_string(depth) + "-bit: reads back as 16x8, 3 frames");
    if (opened)
    {
      auto luma = reader.readLuma(2, 0, error);
      check(!luma.empty() && luma.at(5, 3) == 100,
            std::to_string(depth) + "-bit: luma reads back as 100 at 8 bit");
    }
    std::remove(path.c_str());
  }

#ifdef BDA_HAVE_OPENCV
  std::cout << "every stage, 8 and 10 bit" << std::endl;
  for (const int depth : {8, 10})
    for (const auto &info : stageCatalog())
    {
      if (info.type == StageType::GmCompensation)
        continue;
      auto plane = texture(96, 64, depth, 7);
      auto orig  = plane;
      FilterChain{{FilterStage::withDefaults(info.type)}}.apply(plane);
      const bool inRange = std::all_of(plane.samples.begin(),
                                       plane.samples.end(),
                                       [&](std::uint16_t v) { return v <= plane.maxValue(); });
      const bool changed = plane.samples != orig.samples || info.type == StageType::Custom3x3;
      check(plane.valid() && inRange && changed,
            info.name + " at " + std::to_string(depth) + " bit: runs, stays in range, changes");
    }

  std::cout << "tiny frames" << std::endl;
  for (const auto &[tw, th] : {std::pair{1, 1}, std::pair{2, 2}, std::pair{7, 3}, std::pair{16, 16}})
  {
    bool ok = true;
    for (const auto &info : stageCatalog())
    {
      auto       plane    = texture(tw, th, 10, 11);
      const auto previous = texture(tw, th, 10, 12);
      // Must not throw out of apply(): on a caching thread that would end the program.
      FilterChain{{FilterStage::withDefaults(info.type)}}.apply(plane, [&](LumaPlane &out) {
        out = previous;
        return true;
      });
      ok = ok && plane.valid() &&
           std::all_of(plane.samples.begin(), plane.samples.end(), [](std::uint16_t v) {
             return v <= 1023;
           });
    }
    check(ok, std::to_string(tw) + "x" + std::to_string(th) + ": every stage returns a valid plane");
  }

  std::cout << "threshold units" << std::endl;
  {
    auto p = makePlane(2, 1, 10);
    at(p, 0, 0) = 600; // above 128 * 4 = 512
    at(p, 1, 0) = 400;
    FilterChain{{FilterStage::withDefaults(StageType::Threshold)}}.apply(p);
    check(at(p, 0, 0) == 1023 && at(p, 1, 0) == 0, "threshold 128 is 512 on a 10-bit plane");
  }

  std::cout << "GM compensation" << std::endl;
  {
    const int w = 320, h = 192;
    auto      previous = texture(w, h, 8, 3);
    // current = previous moved by MV(x, y) = (5, -3), rotated 1 degree and scaled 1.01 about the
    // centre. Built with the inverse mapping by hand, bilinear.
    auto         current = makePlane(w, h, 8);
    const double a = 1.01 * std::cos(M_PI / 180), b = 1.01 * std::sin(M_PI / 180);
    const double cx = w / 2.0, cy = h / 2.0, tx = 5, ty = -3;
    const double det = a * a + b * b;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
      {
        // current(x, y) = previous(T^-1(x, y)), T(p) = R*S*(p - c) + c + t
        const double u  = x - cx - tx, v = y - cy - ty;
        const double sx = (a * u + b * v) / det + cx, sy = (-b * u + a * v) / det + cy;
        const int    x0 = std::clamp(int(std::floor(sx)), 0, w - 2);
        const int    y0 = std::clamp(int(std::floor(sy)), 0, h - 2);
        const double fx = std::clamp(sx - x0, 0.0, 1.0), fy = std::clamp(sy - y0, 0.0, 1.0);
        const double val = (1 - fy) * ((1 - fx) * at(previous, x0, y0) + fx * at(previous, x0 + 1, y0)) +
                           fy * ((1 - fx) * at(previous, x0, y0 + 1) + fx * at(previous, x0 + 1, y0 + 1));
        at(current, x, y) = std::uint16_t(std::lround(val));
      }
    const auto provider = [&](LumaPlane &out) {
      out = previous;
      return true;
    };

    auto gm    = FilterStage::withDefaults(StageType::GmCompensation);
    auto plain = current;
    FilterChain{{gm}}.apply(plain, provider);
    const double before = meanAbsDiff(current, previous, 24);
    const double after  = meanAbsDiff(plain, previous, 24);
    std::cout << "        mean |cur - prev| " << before << " -> |aligned - prev| " << after << std::endl;
    check(after < before / 5 && after < 3.0, "the aligned frame matches the previous one");

    auto noPrev = current;
    FilterChain{{gm}}.apply(noPrev);
    check(noPrev.samples == current.samples, "without a previous frame the stage passes through");

    // Identical frames, a blur in front of GM, difference output: zero only if the previous frame
    // went through the blur too.
    auto diff                         = gm;
    diff.params[3]                    = 1;
    auto       same                   = previous;
    const auto sameProvider           = [&](LumaPlane &out) {
      out = previous;
      return true;
    };
    FilterChain{{FilterStage::withDefaults(StageType::Gaussian), diff}}.apply(same, sameProvider);
    const auto maxDiff = *std::max_element(same.samples.begin(), same.samples.end());
    check(maxDiff <= 1, "the reference runs through the stages in front of GM (max diff " +
                            std::to_string(maxDiff) + ")");
  }
#endif

  std::cout << (g_failures ? "FAILED" : "PASSED") << std::endl;
  return g_failures ? 1 : 0;
}
