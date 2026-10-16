// Unit test: odyssey closed-loop ME (src/me/OdysseyClosedLoopMe), the SAD path.
//
// The bit-exact reference is odyssey's own trace (trace_cl_inter), checked block for block by a
// separate tool - see docs/ai/30-designs/odyssey-closedloop-design.md. This test pins the rules that
// check relies on, from first principles:
//   * a whole-pel vector predicts the reference unchanged; a half-pel one on a linear ramp lands
//     exactly between the samples (8-tap and 4-tap regular);
//   * the full-pel window is mvd [-4, 3] around the centre, clamped 64 pixels past the picture;
//   * the cost is SAD + the per-size shifted rate (identical pictures: just the rate at mvd 0);
//   * an integer pan is found from (0, 0); a sub-pel target made with the same predictor is found
//     with SAD 0, at eighth-pel when allowed and quarter-pel otherwise;
//   * a seed moves the centre: a pan too far for the window from (0, 0) is found from the seed.
#include <cmath>
#include <iostream>
#include <string>

#include "me/MeCost.h"
#include "me/OdysseyClosedLoopMe.h"

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

// Smooth texture, so the SAD surface has a single clear minimum.
std::uint8_t texel(int x, int y)
{
  return std::uint8_t(128 + 50 * std::sin(x * 0.21 + y * 0.07) + 40 * std::cos(y * 0.17 - x * 0.05));
}

MePlane texture(int w, int h, int shiftX = 0, int shiftY = 0)
{
  MePlane p(w, h, kOdysseyPadFull);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      p.at(x, y) = texel(x - shiftX, y - shiftY);
  p.padEdges();
  return p;
}
} // namespace

int main()
{
  // Prediction.
  {
    const auto ref = texture(128, 128);
    std::uint8_t pred[64 * 64];
    odysseyPredictBlock(ref, 32, 32, 16, MotionVector::fromFullPel(3, -2), true, pred);
    bool same = true;
    for (int r = 0; r < 16; ++r)
      for (int c = 0; c < 16; ++c)
        same = same && pred[r * 16 + c] == ref.at(35 + c, 30 + r);
    check(same, "a whole-pel vector predicts the reference block unchanged");

    MePlane ramp(64, 64, kOdysseyPadFull);
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x)
        ramp.at(x, y) = std::uint8_t(4 * x);
    ramp.padEdges();
    for (const bool tap8 : {true, false})
    {
      odysseyPredictBlock(ramp, 16, 16, 8, {4, 0}, tap8, pred);
      bool half = true;
      for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
          half = half && pred[r * 8 + c] == 4 * (16 + c) + 2;
      check(half, std::string("half-pel on a ramp lands between the samples (") + (tap8 ? "8" : "4") +
                      "-tap regular)");
    }
  }

  // Window and cost.
  {
    const auto pic = texture(192, 128);
    OdyClOptions opt;
    const auto b = odysseyClosedLoopSearchBlock(pic, pic, 64, 64, 8, {}, opt);
    check(b.minMvdX == -4 && b.maxMvdX == 3 && b.minMvdY == -4 && b.maxMvdY == 3,
          "the full-pel window is mvd [-4, 3] in x and y");
    check(b.intBest.isZero() && b.intCost == odysseyClosedLoopRateTerm(0, 0, BlockSize::Blk8),
          "identical pictures: best (0,0), cost = the 8x8 rate term at mvd 0 alone");
    check(b.mv.isZero() && b.sad == 0, "and the sub-pel stage keeps (0,0) with SAD 0");

    const auto edge = odysseyClosedLoopSearchBlock(pic, pic, 0, 0, 8, MotionVector::fromFullPel(-70, 0), opt);
    check(edge.centreX == -70 && edge.minMvdX == 6 && edge.maxMvdX == 6,
          "a centre past the 64-pixel pad clamps the window to the last allowed vector (-64)");

    const auto b64 = odysseyClosedLoopSearchBlock(pic, pic, 0, 0, 64, {}, opt);
    check(b64.intCost == odysseyClosedLoopRateTerm(0, 0, BlockSize::Blk64) &&
              b64.intCost > b.intCost,
          "the 64x64 rate is not shifted, the 8x8 one is (>> 6)");
  }

  // Integer pan and sub-pel targets.
  {
    const auto ref = texture(192, 192);
    const auto cur = texture(192, 192, 3, -2); // content moved by (3, -2): the vector is (-3, 2)
    OdyClOptions opt;
    const auto b = odysseyClosedLoopSearchBlock(cur, ref, 64, 64, 32, {}, opt);
    check(b.intBest.x == -3 && b.intBest.y == 2, "an integer pan (3, -2) is found from (0, 0) as MV(-3, 2)");
    check(b.mv == MotionVector::fromFullPel(-3, 2) && b.sad == 0, "and stays whole-pel with SAD 0");

    for (const bool hp : {true, false})
    {
      const MotionVector target = hp ? MotionVector{-13, 6} : MotionVector{-14, 6}; // -1.625 / -1.75, 0.75
      MePlane            sub    = texture(192, 192);
      std::uint8_t       pred[64 * 64];
      odysseyPredictBlock(ref, 64, 64, 64, target, hp, pred);
      for (int r = 0; r < 64; ++r)
        for (int c = 0; c < 64; ++c)
          sub.at(64 + c, 64 + r) = pred[r * 64 + c];
      OdyClOptions o;
      o.allowHighPrecisionMv = hp;
      const auto s = odysseyClosedLoopSearchBlock(sub, ref, 64, 64, 64, {}, o);
      check(s.mv == target && s.sad == 0,
            std::string("a sub-pel target is found exactly at ") + (hp ? "eighth" : "quarter") + "-pel (" +
                std::to_string(s.mv.x) + ", " + std::to_string(s.mv.y) + ")/8, eighth " +
                std::to_string(s.eighthPel));
      if (!hp)
        check(!s.eighthPel && s.mv.x % 2 == 0 && s.mv.y % 2 == 0, "without high precision only quarter steps");
    }
  }

  // Seed.
  {
    const auto ref = texture(256, 192);
    const auto cur = texture(256, 192, 9, 0); // 9 pixels: beyond the [-4, 3] window from (0, 0)
    MePictureSet pictures;
    pictures.current   = cur;
    pictures.reference = ref;
    MeParams params;
    params.blockSizes = BlockSizeSet();
    params.blockSizes.add(BlockSize::Blk64);
    OdysseyClosedLoopMe me;
    const auto           noSeed = me.estimateFrame(pictures, params);
    MeFrameResult        seed;
    for (int y = 0; y < 192; y += 64)
      for (int x = 0; x < 256; x += 64)
        seed.blocks.push_back({{x, y, BlockSize::Blk64}, MotionVector::fromFullPel(-8, 0)});
    params.seed       = &seed;
    const auto seeded = me.estimateFrame(pictures, params);
    const auto inner  = [](const MeFrameResult &r) {
      for (const auto &b : r.blocks)
        if (b.block.x == 64 && b.block.y == 64)
          return b;
      return MeBlockResult{};
    };
    check(!(inner(noSeed).mv == MotionVector::fromFullPel(-9, 0)), "from (0, 0) a 9-pixel pan is out of reach");
    check(inner(seeded).mv == MotionVector::fromFullPel(-9, 0) && inner(seeded).nativeCost == 0,
          "from a seed of MV(-8, 0) it is found exactly");
    check(seeded.algorithm == Algorithm::OdysseyClosedLoop && seeded.blocks.size() == 12,
          "one result per 64x64 block of the 256x192 picture");
  }

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
