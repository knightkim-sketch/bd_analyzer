// Unit test: cutting source and reference blocks out of raw YUV for the ME compare popup
// (src/me/MeCompareCrop).
//
// Expectations come from the formulas, not from the code's own output:
//   * BT.709 limited range maps Y 16 / 235 to black / white, and (Y, Cb, Cr) = (63, 102, 240) to red;
//   * a reference that is the source moved by (dx, dy) gives SAD 0 and a flat 128 diff at that
//     vector, and not at another;
//   * a block reaching past the frame repeats the edge pixel, as the estimators' padding does;
//   * 10-bit is shifted down to 8 bit, so its SAD equals the 8-bit one of the same picture.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "me/MeCompareCrop.h"

using namespace bda::me;

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

bool near(int a, int b, int tolerance) { return std::abs(a - b) <= tolerance; }

// An 8-bit 4:2:0 planar frame with flat chroma and the given luma function.
template <typename F>
std::vector<std::uint8_t> frame420(int w, int h, F luma, int cb, int cr, YuvFrameLayout &layout)
{
  layout           = {};
  layout.width     = w;
  layout.height    = h;
  layout.offset[0] = 0;
  layout.offset[1] = std::int64_t(w) * h;
  layout.offset[2] = layout.offset[1] + std::int64_t(w / 2) * (h / 2);
  std::vector<std::uint8_t> f(std::size_t(w) * h * 3 / 2);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      f[std::size_t(y) * w + x] = std::uint8_t(luma(x, y));
  for (std::size_t i = std::size_t(layout.offset[1]); i < std::size_t(layout.offset[2]); ++i)
    f[i] = std::uint8_t(cb);
  for (std::size_t i = std::size_t(layout.offset[2]); i < f.size(); ++i)
    f[i] = std::uint8_t(cr);
  return f;
}

// A texture without repeats, so only the right vector lines it up.
int texture(int x, int y) { return 16 + (x * 7 + y * 13 + (x * y) % 11) % 200; }

} // namespace

int main()
{
  YuvFrameLayout l;

  // Grey levels.
  {
    const auto f = frame420(8, 8, [](int x, int) { return x < 4 ? 16 : 235; }, 128, 128, l);
    check(checkLayout(l, f.size()).empty(), "a whole 4:2:0 frame is accepted");
    check(!checkLayout(l, f.size() - 1).empty(), "a truncated one is refused");
    const auto img = cropRgb(f.data(), l, {0, 0, 8, 1});
    check(img.width == 8 && img.height == 1 && img.rgb.size() == 24, "the crop has the block's size");
    check(img.rgb[0] == 0 && img.rgb[1] == 0 && img.rgb[2] == 0, "Y 16 is black");
    check(img.rgb[21] == 255 && img.rgb[22] == 255 && img.rgb[23] == 255, "Y 235 is white");
  }

  // Red, planar and semi-planar.
  {
    const auto f   = frame420(4, 4, [](int, int) { return 63; }, 102, 240, l);
    const auto img = cropRgb(f.data(), l, {1, 1, 1, 1});
    check(near(img.rgb[0], 255, 2) && near(img.rgb[1], 0, 3) && near(img.rgb[2], 0, 2),
          "(63, 102, 240) is red: " + std::to_string(img.rgb[0]) + "," +
              std::to_string(img.rgb[1]) + "," + std::to_string(img.rgb[2]));

    YuvFrameLayout nv = l;
    nv.interleavedUV  = true;
    std::vector<std::uint8_t> g(f.begin(), f.begin() + 16);
    for (int i = 0; i < 4; ++i)
    {
      g.push_back(102);
      g.push_back(240);
    }
    check(checkLayout(nv, g.size()).empty(), "a semi-planar frame is accepted");
    const auto img2 = cropRgb(g.data(), nv, {3, 3, 1, 1});
    check(img2.rgb == img.rgb, "semi-planar chroma gives the same colour");
  }

  // Edge repetition.
  {
    const auto f   = frame420(8, 8, [](int x, int y) { return texture(x, y); }, 128, 128, l);
    const auto out = cropRgb(f.data(), l, {-2, 0, 3, 1});
    const auto in  = cropRgb(f.data(), l, {0, 0, 1, 1});
    check(out.rgb[0] == in.rgb[0] && out.rgb[3] == in.rgb[0] && out.rgb[6] == in.rgb[0],
          "pixels left of the frame repeat column 0");
  }

  // Diff at the right vector and at a wrong one.
  {
    const int  dx = 3, dy = 1;
    YuvFrameLayout ls, lr;
    const auto src = frame420(32, 32, [](int x, int y) { return texture(x, y); }, 128, 128, ls);
    // The reference holds the same picture moved, so source(x, y) == reference(x + dx, y + dy).
    const auto ref =
        frame420(32, 32, [](int x, int y) { return texture(x - dx, y - dy); }, 128, 128, lr);
    const auto hit = lumaDiff(src.data(), ref.data(), ls, {8, 8, 8, 8}, dx, dy, 4);
    bool       flat = true;
    for (auto v : hit.image.rgb)
      flat = flat && v == 128;
    check(hit.sad == 0 && hit.maxAbs == 0 && flat, "the right vector gives SAD 0 and a flat 128");
    const auto miss = lumaDiff(src.data(), ref.data(), ls, {8, 8, 8, 8}, 0, 0, 4);
    check(miss.sad > 0, "a wrong vector does not (SAD " + std::to_string(miss.sad) + ")");

    // 10-bit little-endian of the same pictures: value << 2 reads back as the 8-bit value.
    const auto to10 = [](const std::vector<std::uint8_t> &f8) {
      std::vector<std::uint8_t> f10;
      for (auto v : f8)
      {
        const unsigned w = unsigned(v) << 2;
        f10.push_back(std::uint8_t(w & 0xFF));
        f10.push_back(std::uint8_t(w >> 8));
      }
      return f10;
    };
    YuvFrameLayout l10 = ls;
    l10.bitDepth       = 10;
    for (auto &o : l10.offset)
      o *= 2;
    const auto s10 = to10(src), r10 = to10(ref);
    check(checkLayout(l10, s10.size()).empty(), "a 10-bit frame is accepted");
    const auto miss10 = lumaDiff(s10.data(), r10.data(), l10, {8, 8, 8, 8}, 0, 0, 4);
    check(miss10.sad == miss.sad, "10-bit is compared at 8 bit (SAD " +
                                      std::to_string(miss10.sad) + ")");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
