// Unit test for the odyssey open-loop ME reproduction.
//
// The texture is the same multi-scale, aperiodic field the SVT test uses, and for the same
// reasons: a hierarchical search cannot work on noise, and a periodic field lets the coarse stage
// lock on one period away. See tests/unit/me-svt-integer.cpp for how those two failures looked.
//
// Qt-free, like the rest of src/me.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "me/MePlane.h"
#include "me/MeTypes.h"
#include "me/OdysseyOpenLoopMe.h"

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

constexpr int kW = 128;
constexpr int kH = 128;

std::uint8_t texel(int x, int y)
{
  const double v = 100.0 + 50.0 * std::sin(x / 9.0) * std::sin(y / 11.0) +
                   20.0 * std::sin(x / 3.0 + y / 5.0) +
                   (static_cast<double>(x) * x + static_cast<double>(y) * y) / 512.0;
  const int clamped = static_cast<int>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
  return static_cast<std::uint8_t>(clamped);
}

bda::me::MePlane makePlane(int shiftX, int shiftY)
{
  std::vector<std::uint8_t> data(static_cast<std::size_t>(kW) * kH);
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      data[static_cast<std::size_t>(y) * kW + x] = texel(x - shiftX, y - shiftY);
  return bda::me::MePlane::fromLuma8(data.data(), kW, kH, kW, bda::me::kOdysseyPadFull);
}

const bda::me::MeBlockResult *find(const bda::me::MeFrameResult &r,
                                   int                           x,
                                   int                           y,
                                   bda::me::BlockSize            size)
{
  for (const auto &b : r.blocks)
    if (b.block.x == x && b.block.y == y && b.block.size == size)
      return &b;
  return nullptr;
}
} // namespace

int main()
{
  using namespace bda::me;

  OdysseyOpenLoopMe estimator;
  check(estimator.algorithm() == Algorithm::OdysseyOpenLoop, "reports its algorithm");
  check(std::string(estimator.nativeCostName()) == "SSE + rate^2",
        "names its native cost, which is not SAD");

  /* --- a known displacement ------------------------------------------------------------------
   *
   * The vector is recovered even though the final search runs on the upscaled round-trip picture
   * rather than the source, because a whole-picture translation survives the round trip.
   *
   * The SSE is NOT expected to be zero: the search compares half -> org upscaled pictures, which
   * have been through a mean pool and a bilinear expansion, so even a perfect alignment leaves a
   * residue. The common metric, measured on the original pictures, is the one that can reach zero.
   */
  {
    MePictureSet pictures;
    pictures.current     = makePlane(0, 0);
    pictures.reference   = makePlane(4, 2);
    pictures.refDistance = 1;

    MeParams params;
    params.frameInterval = 1;

    const auto result = estimator.estimateFrame(pictures, params);
    check(result.ok(), "the estimate runs: " + result.error);
    check(!result.blocks.empty(), "and produces blocks");

    const auto *b64 = find(result, 0, 0, BlockSize::Blk64);
    check(b64 != nullptr, "the top-left 64x64 block is reported");
    if (b64)
    {
      checkEq(b64->mv.x, MotionVector::fromFullPel(4, 2).x, "recovers the horizontal displacement");
      checkEq(b64->mv.y, MotionVector::fromFullPel(4, 2).y, "recovers the vertical displacement");
      checkEq(b64->commonSad, 0, "and the common metric on the original pictures is zero");
      check(b64->nativeCost > 0,
            "while the native SSE is not, because it is measured on the upscaled pictures");
    }

    check(find(result, 0, 0, BlockSize::Blk8) != nullptr, "8x8 blocks are reported");
    check(find(result, 0, 0, BlockSize::Blk16) != nullptr, "16x16 blocks are reported");
    check(find(result, 0, 0, BlockSize::Blk32) != nullptr, "32x32 blocks are reported");
  }

  /* --- the search really does run on the upscaled picture -------------------------------------
   *
   * If the implementation quietly searched the original instead, a perfectly aligned block would
   * score zero. Catching that is the whole point of the check below: it is the mistake the
   * Confluence page warns about, and it is invisible in the vectors.
   */
  {
    MePictureSet pictures;
    pictures.current   = makePlane(0, 0);
    pictures.reference = makePlane(0, 0); // identical pictures, so the answer is the zero vector

    MeParams params;
    const auto  result = estimator.estimateFrame(pictures, params);
    const auto *b64    = find(result, 0, 0, BlockSize::Blk64);
    check(b64 != nullptr, "identical pictures report blocks");
    if (b64)
    {
      check(b64->mv.isZero(), "and settle on the zero vector");
      checkEq(b64->commonSad, 0, "with a zero common metric on the originals");
      check(b64->nativeCost > 0,
            "but a non-zero native SSE - the upscaled round trip is not lossless");
    }
  }

  // --- a negative frame interval --------------------------------------------------------------
  {
    MePictureSet pictures;
    pictures.current     = makePlane(0, 0);
    pictures.reference   = makePlane(-3, 2);
    pictures.refDistance = -1;

    MeParams params;
    params.frameInterval = -1;

    const auto  result = estimator.estimateFrame(pictures, params);
    const auto *b64    = find(result, 64, 0, BlockSize::Blk64);
    check(result.ok() && b64 != nullptr, "a negative interval still produces a result");
    if (b64)
    {
      checkEq(b64->mv.x, MotionVector::fromFullPel(-3, 2).x, "recovers a leftward vector");
      checkEq(b64->mv.y, MotionVector::fromFullPel(-3, 2).y, "and the vertical part");
    }
  }

  /* --- the neighbour state is scan-order dependent --------------------------------------------
   *
   * Not a behaviour to assert a number on, but worth pinning that the estimator is deterministic
   * across runs: the shift register is per-call state, so a second call on the same input has to
   * give the same answer. If it leaked across calls this would drift.
   */
  {
    MePictureSet pictures;
    pictures.current   = makePlane(0, 0);
    pictures.reference = makePlane(4, 2);

    MeParams   params;
    const auto first  = estimator.estimateFrame(pictures, params);
    const auto second = estimator.estimateFrame(pictures, params);

    bool same = first.blocks.size() == second.blocks.size();
    for (std::size_t i = 0; same && i < first.blocks.size(); ++i)
      same = first.blocks[i].mv == second.blocks[i].mv &&
             first.blocks[i].nativeCost == second.blocks[i].nativeCost;
    check(same, "two runs on the same input agree - no state leaks between calls");
  }

  // --- the block size mask --------------------------------------------------------------------
  {
    MePictureSet pictures;
    pictures.current   = makePlane(0, 0);
    pictures.reference = makePlane(4, 2);

    MeParams params;
    params.blockSizes = BlockSizeSet();
    params.blockSizes.add(BlockSize::Blk16);

    const auto result = estimator.estimateFrame(pictures, params);
    check(find(result, 0, 0, BlockSize::Blk16) != nullptr, "the selected size is reported");
    check(find(result, 0, 0, BlockSize::Blk64) == nullptr, "an unselected size is not");
    checkEq(static_cast<long long>(result.blocks.size()), 4 * 16, "sixteen 16x16 per superblock");
  }

  // --- refusing what it cannot do -------------------------------------------------------------
  {
    MePictureSet pictures;
    std::string  reason;
    check(!estimator.canRun(pictures, &reason), "empty pictures are refused");
    check(!reason.empty(), "with a reason: " + reason);
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
