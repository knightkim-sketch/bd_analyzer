// Unit test for the Qt-free motion estimation primitives in src/me.
//
// Compiled with plain g++ - no Qt, no libYUViewLib. That is part of what is being tested: the ME
// core has to stay usable from the headless CLI, and a stray Qt include would only show up here.
//
// The rescaler expectations are hand-computed from the encoder sources rather than captured from
// our own output, so a change in behaviour fails instead of being blessed.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "me/MeCost.h"
#include "me/MePlane.h"
#include "me/MeTypes.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
void checkEq(long long got, long long want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (got " << got << ", want " << want << ")";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

// A plane whose sample at (x, y) is a known function, so every expectation can be written out.
bda::me::MePlane ramp(int w, int h, int pad)
{
  std::vector<std::uint8_t> data(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      data[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>((y * 16 + x) & 0xFF);
  return bda::me::MePlane::fromLuma8(data.data(), w, h, w, pad);
}
} // namespace

int main()
{
  using namespace bda::me;

  // --- border replication --------------------------------------------------------------------
  {
    auto plane = ramp(8, 8, 4);
    check(plane.width() == 8 && plane.height() == 8 && plane.pad() == 4, "plane keeps its shape");
    checkEq(plane.at(0, 0), 0, "top-left active sample");
    checkEq(plane.at(-4, 0), plane.at(0, 0), "left border replicates the first column");
    checkEq(plane.at(11, 3), plane.at(7, 3), "right border replicates the last column");
    checkEq(plane.at(2, -4), plane.at(2, 0), "top border replicates the first row");
    checkEq(plane.at(2, 11), plane.at(2, 7), "bottom border replicates the last row");
    checkEq(plane.at(-4, -4), plane.at(0, 0), "corner replicates the corner sample");
  }

  /* --- the two downscalers, step 2 -----------------------------------------------------------
   *
   * Worth being precise about, because the two kernels look very different in source and are not:
   *
   *   svt_aom_downsample_2d_c starts at half_decim_step and reads (index - 1, index) on the
   *   previous and current line. For step 2 that is half_decim_step = 1, so it reads columns
   *   0 and 1 of rows 0 and 1 - the aligned 2x2 box - and rounds (sum + 2) >> 2.
   *
   *   meanpooling_2d starts at (0, 0), sums the 2x2 box and rounds (sum + (1 << 1)) >> 2.
   *
   * Same box, same rounding. At step 2 the two agree exactly, and this test pins that down so the
   * claim is not carried on trust. They diverge at step 4 - see the next block.
   */
  {
    auto plane = ramp(8, 8, 4);
    auto svt   = plane.downscaleSvt(2, 2);
    auto ody   = plane.downscaleOdysseyMeanpool(2, 2);

    check(svt.width() == 4 && svt.height() == 4, "step 2 halves the dimensions");

    // (0,0): samples (0,0)=0 (1,0)=1 (0,1)=16 (1,1)=17 -> (34 + 2) >> 2 = 9
    checkEq(svt.at(0, 0), 9, "SVT step 2 averages the aligned 2x2 box");
    checkEq(ody.at(0, 0), 9, "odyssey meanpool step 2 gives the same value");

    bool same = true;
    for (int y = 0; y < 4 && same; ++y)
      for (int x = 0; x < 4 && same; ++x)
        same = svt.at(x, y) == ody.at(x, y);
    check(same, "at step 2 the SVT and odyssey downscalers agree on every sample");
  }

  /* --- step 4: where the kernels really differ, and where they only look identical ----------
   *
   * At step 4 the SVT kernel still reads two lines and two columns - half_decim_step = 2, so
   * (index - 1, index) is columns 1 and 2 of rows 1 and 2 of each 4x4 block. It samples the middle
   * 2x2 and ignores the other twelve pixels. meanpooling_2d averages all sixteen.
   *
   * On a linear ramp those two agree, because the mean of a symmetric neighbourhood equals its
   * centre - which is exactly the trap. The first block below records that coincidence so nobody
   * "verifies" the kernels with a gradient and concludes they are interchangeable; the second uses
   * a spike, where centre-sampling and averaging cannot agree.
   */
  {
    auto plane = ramp(8, 8, 4);
    auto svt   = plane.downscaleSvt(4, 2);
    auto ody   = plane.downscaleOdysseyMeanpool(4, 2);

    check(svt.width() == 2 && svt.height() == 2, "step 4 quarters the dimensions");
    // SVT: rows 1,2 x columns 1,2 -> 17, 18, 33, 34 -> (102 + 2) >> 2 = 26
    checkEq(svt.at(0, 0), 26, "SVT step 4 samples the middle 2x2 of the 4x4 block");
    // odyssey: all sixteen -> 6 + 70 + 134 + 198 = 408 -> (408 + 8) >> 4 = 26
    checkEq(ody.at(0, 0), 26, "odyssey meanpool step 4 averages all sixteen, same answer here");
    check(svt.at(0, 0) == ody.at(0, 0), "on a linear ramp the two coincide - not a general result");
  }
  {
    // One bright pixel in the corner of the first 4x4 block, nothing else.
    std::vector<std::uint8_t> data(64, 0);
    data[0] = 255;
    auto plane = MePlane::fromLuma8(data.data(), 8, 8, 8, 4);

    auto svt = plane.downscaleSvt(4, 2);
    auto ody = plane.downscaleOdysseyMeanpool(4, 2);

    // SVT never looks at (0,0) - it reads rows 1,2 and columns 1,2, all zero.
    checkEq(svt.at(0, 0), 0, "SVT step 4 misses a corner spike entirely");
    // odyssey sums it: (255 + 8) >> 4 = 16
    checkEq(ody.at(0, 0), 16, "odyssey meanpool step 4 carries the spike into the average");
    check(svt.at(0, 0) != ody.at(0, 0), "so the two kernels are not interchangeable at step 4");
  }

  /* --- how SVT actually builds its pyramid ---------------------------------------------------
   *
   * The HME levels do not run on "the input downscaled by 4". SVT cascades two step 2 downscales
   * (quarter from the input, sixteenth from the quarter), and the direct step 4 call is only the
   * fallback for HME level 1 being off. Those are three different pictures, so this pins the
   * cascade against the shortcut - getting it wrong changes the picture level 0 searches.
   */
  {
    std::vector<std::uint8_t> data(64, 0);
    data[0] = 255;
    auto plane = MePlane::fromLuma8(data.data(), 8, 8, 8, 4);

    auto pyramid = buildSvtPyramid(plane, 2, 2);
    check(pyramid.quarter.width() == 4 && pyramid.sixteenth.width() == 2,
          "the pyramid halves twice");

    // quarter(0,0) is the aligned 2x2 mean of the spike block: (255 + 2) >> 2 = 64.
    checkEq(pyramid.quarter.at(0, 0), 64, "the spike survives into the quarter picture");
    // sixteenth(0,0) is then the 2x2 mean of quarter's (0,0)..(1,1) = 64, 0, 0, 0 -> (64 + 2) >> 2
    checkEq(pyramid.sixteenth.at(0, 0), 16, "and is averaged again into the sixteenth picture");

    // The step 4 shortcut reads the middle 2x2 of the input and misses the spike entirely.
    auto shortcut = plane.downscaleSvt(4, 2);
    checkEq(shortcut.at(0, 0), 0, "a direct step 4 downscale is a different picture");
    check(pyramid.sixteenth.at(0, 0) != shortcut.at(0, 0),
          "so the cascade cannot be replaced by one step 4 call");
  }

  /* --- odyssey bilinear upscale --------------------------------------------------------------
   *
   * bilinear() weights the four neighbours by (scale - x, x) and (scale - y, y) and then shifts by
   * `scale`, not by 2 * log2(scale). For scale 2 the weights sum to 4 while the shift is 2, so the
   * result is roughly twice the input - odyssey's arithmetic, kept as is. The x = y = 0 sample is
   * the one place it is easy to state exactly: wsum = p00 * 2 * 2 = 4 * p00, so (4 * p00 + 2) >> 2
   * comes back to p00 + rounding.
   */
  {
    std::vector<std::uint8_t> data = {10, 20, 30, 40};
    auto small = MePlane::fromLuma8(data.data(), 2, 2, 2, 2);
    auto up    = small.upscaleOdysseyBilinear(2, 2);

    check(up.width() == 4 && up.height() == 4, "scale 2 doubles the dimensions");
    // (0,0): p00 = 10 -> (10 * 2 * 2 + 2) >> 2 = 10
    checkEq(up.at(0, 0), 10, "the co-sited sample survives the round trip");
    // (1,0): x = 1 -> wsum0 = 10 * 1 + 20 * 1 = 30, wsum1 likewise from 30, 40 -> 70
    //        wsum = 30 * 2 + 70 * 0 = 60 -> (60 + 2) >> 2 = 15
    checkEq(up.at(1, 0), 15, "the horizontal midpoint interpolates its two neighbours");
    // last column has sft_hor = 0, so it duplicates rather than reaching into the border
    checkEq(up.at(3, 0), up.at(2, 0), "the last column repeats instead of reading the border");
  }

  // --- 10-bit input is shifted down, as odyssey's open-loop path does -------------------------
  {
    std::vector<std::uint16_t> deep = {0, 4, 1023, 512};
    auto plane = MePlane::fromLuma10AsShifted8(deep.data(), 2, 2, 2, 1);
    checkEq(plane.at(0, 0), 0, "10-bit 0 shifts to 0");
    checkEq(plane.at(1, 0), 1, "10-bit 4 shifts to 1");
    checkEq(plane.at(0, 1), 255, "10-bit 1023 shifts to 255");
    checkEq(plane.at(1, 1), 128, "10-bit 512 shifts to 128");
  }

  // --- SAD / SSE, including the row subsampling both encoders use -----------------------------
  {
    std::vector<std::uint8_t> a(16, 10);
    std::vector<std::uint8_t> b(16, 13);
    auto src = MePlane::fromLuma8(a.data(), 4, 4, 4, 2);
    auto ref = MePlane::fromLuma8(b.data(), 4, 4, 4, 2);

    checkEq(blockSad(src, 0, 0, ref, 0, 0, 4, 4, 1), 3 * 16, "SAD over every row");
    checkEq(blockSad(src, 0, 0, ref, 0, 0, 4, 4, 2), 3 * 8, "SAD with a row step of 2 visits half");
    checkEq(blockSse(src, 0, 0, ref, 0, 0, 4, 4, 1), 9 * 16, "SSE over every row");
    checkEq(blockSse(src, 0, 0, ref, 0, 0, 4, 4, 2), 9 * 8, "SSE with a row step of 2");
  }

  /* --- odyssey MV rate ----------------------------------------------------------------------
   *
   * The table boundaries are where an off-by-one would hide, and the zero tail of lv4 is the
   * upstream quirk this reproduces on purpose (see OdysseyMvRateLut.h).
   */
  {
    checkEq(odysseyMvCost(0, 0), 17 + 17, "zero difference still costs the lv1[0] entry twice");
    checkEq(odysseyMvCost(1, 0), 89 + 17, "lv1 is indexed directly");
    checkEq(odysseyMvCost(127, 0), 377 + 17, "the last lv1 entry");
    checkEq(odysseyMvCost(128, 0), 378 + 17, "128 crosses into lv2");
    checkEq(odysseyMvCost(-1, 0), 89 + 17, "the sign of the difference does not matter");

    // lv4 index 120 lands in the zero-filled tail: 896 + 120 * 8 = 1856.
    checkEq(odysseyMvCost(1856, 0), 0 + 17, "the zero-filled tail of lv4 is reproduced");
    checkEq(odysseyMvCost(4000, 0), 0 + 17, "and the clamp at 1919 lands in that same tail");
  }

  // --- closed-loop rate shifts differ per block size ------------------------------------------
  {
    const auto rate = static_cast<long long>(odysseyMvCost(3, 3));
    checkEq(odysseyClosedLoopRateTerm(3, 3, BlockSize::Blk8), (rate + 32) >> 6, "8x8 shifts by 6");
    checkEq(odysseyClosedLoopRateTerm(3, 3, BlockSize::Blk16), (rate + 8) >> 4, "16x16 shifts by 4");
    checkEq(odysseyClosedLoopRateTerm(3, 3, BlockSize::Blk32), (rate + 2) >> 2, "32x32 shifts by 2");
    checkEq(odysseyClosedLoopRateTerm(3, 3, BlockSize::Blk64), rate, "64x64 uses the rate as is");
  }

  /* --- the common metric --------------------------------------------------------------------
   *
   * Negative vectors are the interesting case: an arithmetic shift would floor towards minus
   * infinity and bias every one of them by a pixel.
   */
  {
    std::vector<std::uint8_t> data(64);
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x)
        data[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(x * 8);
    auto plane = MePlane::fromLuma8(data.data(), 8, 8, 8, 8);

    MeBlock block{0, 0, BlockSize::Blk8};
    checkEq(commonMetricSad(plane, plane, block, MotionVector{0, 0}),
            0,
            "a zero vector against the same plane costs nothing");

    // -7/8 of a pixel truncates towards zero, i.e. to 0, not to -1.
    checkEq(commonMetricSad(plane, plane, block, MotionVector{-7, 0}),
            0,
            "a sub-pixel negative vector truncates towards zero");

    // A full pixel to the left: every column shifts by 8 in this ramp, except where the border
    // replication flattens it.
    check(commonMetricSad(plane, plane, block, MotionVector{-8, 0}) > 0,
          "a whole-pixel negative vector does move the prediction");
  }

  // --- the block size set the checkboxes drive ------------------------------------------------
  {
    auto all = BlockSizeSet::all();
    check(all.contains(BlockSize::Blk8) && all.contains(BlockSize::Blk64), "all() has every size");
    BlockSizeSet only;
    check(only.empty(), "a default set is empty");
    only.add(BlockSize::Blk16);
    check(only.contains(BlockSize::Blk16) && !only.contains(BlockSize::Blk8), "add is selective");
    only.remove(BlockSize::Blk16);
    check(only.empty(), "remove takes it back out");
  }

  // --- motion vector units --------------------------------------------------------------------
  {
    check(MotionVector::fromFullPel(3, -2) == MotionVector{24, -16}, "full-pel scales by 8");
    check(MotionVector::fromQuarterPel(3, -2) == MotionVector{6, -4}, "quarter-pel scales by 2");
    check(MotionVector{}.isZero(), "a default vector is zero");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
