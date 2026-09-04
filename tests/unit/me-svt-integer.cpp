// Unit test for the SVT-AV1 integer ME reproduction.
//
// Driven with synthetic pictures where the right answer is known by construction: the reference is
// the current picture displaced by a fixed amount, so the estimator has to come back with exactly
// that displacement. Qt-free, like the rest of src/me.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "me/MePlane.h"
#include "me/MeTypes.h"
#include "me/SvtIntegerMe.h"

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

/* Deterministic texture with structure at several scales.
 *
 * This matters more than it looks. A hash-like noise field was tried first and the estimator came
 * back with nonsense - correctly so: HME searches the sixteenth picture first, and downscaling
 * noise by four in each axis destroys the correlation the coarse level depends on. Hierarchical
 * search assumes the picture still looks like itself when shrunk, which real video does and white
 * noise does not.
 *
 * So: two low-frequency terms that survive the pyramid, plus a mid-frequency one to make the
 * minimum sharp enough that the fine level has something to lock onto.
 *
 * The quadratic term is not decoration either. With sinusoids alone the field is periodic - the
 * x term repeats every 2*pi*9 = 57 pixels - and the coarse level happily locked onto a match one
 * whole period away, returning -61 where the answer was -4. A smooth aperiodic envelope makes the
 * correct alignment the only good one at every scale.
 */
std::uint8_t texel(int x, int y)
{
  const double v = 100.0 + 50.0 * std::sin(x / 9.0) * std::sin(y / 11.0) +
                   20.0 * std::sin(x / 3.0 + y / 5.0) +
                   (static_cast<double>(x) * x + static_cast<double>(y) * y) / 512.0;
  const int clamped = static_cast<int>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
  return static_cast<std::uint8_t>(clamped);
}

bda::me::MePlane makePlane(int shiftX, int shiftY, int pad)
{
  std::vector<std::uint8_t> data(static_cast<std::size_t>(kW) * kH);
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      data[static_cast<std::size_t>(y) * kW + x] = texel(x - shiftX, y - shiftY);
  return bda::me::MePlane::fromLuma8(data.data(), kW, kH, kW, pad);
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

  SvtIntegerMe estimator;
  check(estimator.algorithm() == Algorithm::SvtIntegerMe, "reports its algorithm");
  check(std::string(estimator.nativeCostName()) == "SAD", "names its native cost SAD");

  /* --- a known displacement ------------------------------------------------------------------
   *
   * reference = current content moved right 3 and down 2, so predicting the current block means
   * reaching (+3, +2) into the reference.
   */
  {
    MePictureSet pictures;
    pictures.current     = makePlane(0, 0, 64);
    pictures.reference   = makePlane(3, 2, 64);
    pictures.refDistance = 1;

    MeParams params;
    params.frameInterval = 1;
    params.staticBypass  = false; // the pictures differ everywhere, but keep the path out of it

    const auto result = estimator.estimateFrame(pictures, params);
    check(result.ok(), "the estimate runs: " + result.error);
    check(!result.blocks.empty(), "and produces blocks");

    /* Check the block at the origin, not the one at (64, 64).
     *
     * With a displacement of (+3, +2) the prediction for the bottom-right superblock reaches past
     * the right edge, where the border is replicated rather than being the real content - so its
     * SAD cannot be zero however good the search is. The top-left block reads entirely inside the
     * picture and is the one place the answer is exactly zero.
     */
    const auto *b64 = find(result, 0, 0, BlockSize::Blk64);
    check(b64 != nullptr, "the top-left 64x64 block is reported");
    if (b64)
    {
      checkEq(b64->mv.x, MotionVector::fromFullPel(3, 2).x, "recovers the horizontal displacement");
      checkEq(b64->mv.y, MotionVector::fromFullPel(3, 2).y, "recovers the vertical displacement");
      checkEq(b64->nativeCost, 0, "and the SAD at the true displacement is zero");
      checkEq(b64->commonSad, 0, "the common metric agrees");
      check(!b64->staticBypass, "the block was searched, not bypassed");
    }

    // The interior block should agree on the vector even though its SAD cannot reach zero.
    const auto *interior = find(result, 64, 64, BlockSize::Blk64);
    check(interior && interior->mv == MotionVector::fromFullPel(3, 2),
          "the interior block finds the same displacement");

    // Every requested size should be present for that superblock.
    check(find(result, 64, 64, BlockSize::Blk8) != nullptr, "8x8 blocks are reported");
    check(find(result, 64, 64, BlockSize::Blk16) != nullptr, "16x16 blocks are reported");
    check(find(result, 64, 64, BlockSize::Blk32) != nullptr, "32x32 blocks are reported");
  }

  /* --- a negative frame interval -------------------------------------------------------------
   *
   * The sign picks which picture is the reference; the estimator only sees the pair. What is
   * being checked is that a negative interval is not mistaken for "no distance" and does not
   * collapse the search area.
   */
  {
    MePictureSet pictures;
    pictures.current     = makePlane(0, 0, 64);
    pictures.reference   = makePlane(-4, 1, 64);
    pictures.refDistance = -1;

    MeParams params;
    params.frameInterval = -1;
    params.staticBypass  = false;

    // dx = -4 reads to the left, so the block that stays inside is on the right-hand side.
    const auto  result = estimator.estimateFrame(pictures, params);
    const auto *b64    = find(result, 64, 64, BlockSize::Blk64);
    check(result.ok() && b64 != nullptr, "a negative interval still produces a result");
    if (b64)
    {
      checkEq(b64->mv.x, MotionVector::fromFullPel(-4, 1).x, "and recovers a leftward vector");
      checkEq(b64->mv.y, MotionVector::fromFullPel(-4, 1).y, "and the vertical part");
    }
  }

  // --- the static bypass ----------------------------------------------------------------------
  {
    MePictureSet pictures;
    pictures.current   = makePlane(0, 0, 64);
    pictures.reference = makePlane(0, 0, 64); // identical

    MeParams params;
    params.staticBypass = true;

    const auto  result = estimator.estimateFrame(pictures, params);
    const auto *b64    = find(result, 0, 0, BlockSize::Blk64);
    check(b64 != nullptr, "identical pictures still report blocks");
    if (b64)
    {
      check(b64->staticBypass, "an unchanged block takes the static bypass");
      check(b64->mv.isZero(), "and keeps a zero vector");
      checkEq(b64->nativeCost, 0, "with a zero cost");
    }

    params.staticBypass = false;
    const auto  without = estimator.estimateFrame(pictures, params);
    const auto *plain   = find(without, 0, 0, BlockSize::Blk64);
    check(plain != nullptr && !plain->staticBypass,
          "with the bypass off the same block is searched instead");
    check(plain != nullptr && plain->mv.isZero(),
          "and the search still settles on zero, so the flag is the only difference");
  }

  /* --- the block size mask -------------------------------------------------------------------
   *
   * The mask filters what is reported. It is not expected to change the vectors, because the
   * search that produces them runs over all sizes either way.
   */
  {
    MePictureSet pictures;
    pictures.current   = makePlane(0, 0, 64);
    pictures.reference = makePlane(3, 2, 64);

    MeParams params;
    params.staticBypass = false;
    params.blockSizes   = BlockSizeSet();
    params.blockSizes.add(BlockSize::Blk64);

    const auto result = estimator.estimateFrame(pictures, params);
    check(find(result, 64, 64, BlockSize::Blk64) != nullptr, "the selected size is reported");
    check(find(result, 64, 64, BlockSize::Blk8) == nullptr, "an unselected size is not");
    checkEq(static_cast<long long>(result.blocks.size()), 4, "only the four 64x64 blocks come back");

    params.blockSizes.add(BlockSize::Blk32);
    const auto more = estimator.estimateFrame(pictures, params);
    checkEq(static_cast<long long>(more.blocks.size()), 4 + 16, "adding 32x32 adds sixteen more");

    const auto *sameBlock = find(more, 64, 64, BlockSize::Blk64);
    const auto *before    = find(result, 64, 64, BlockSize::Blk64);
    check(sameBlock && before && sameBlock->mv == before->mv,
          "and the mask does not change the vector of a size that was already selected");
  }

  // --- refusing what it cannot do -------------------------------------------------------------
  {
    MePictureSet pictures;
    std::string  reason;
    check(!estimator.canRun(pictures, &reason), "empty pictures are refused");
    check(!reason.empty(), "with a reason: " + reason);

    pictures.current = makePlane(0, 0, 8);
    std::vector<std::uint8_t> small(64 * 64);
    pictures.reference = MePlane::fromLuma8(small.data(), 64, 64, 64, 8);
    reason.clear();
    check(!estimator.canRun(pictures, &reason), "mismatched picture sizes are refused");
    check(!reason.empty(), "with a reason: " + reason);
  }

  /* --- cancellation --------------------------------------------------------------------------
   *
   * A whole-frame estimate is what the UI runs in the background on every frame change, so it has
   * to be abandonable. Cancelling before the call is the sharp case: nothing should come back as a
   * usable result, and ok() has to report false so a caller cannot mistake a partial frame for a
   * finished one.
   */
  {
    MePictureSet pictures;
    pictures.current   = makePlane(0, 0, 64);
    pictures.reference = makePlane(3, 2, 64);

    CancelToken token;
    token.cancel();

    MeParams params;
    params.staticBypass = false;
    params.cancel       = &token;

    const auto result = estimator.estimateFrame(pictures, params);
    check(result.cancelled, "a pre-cancelled token stops the estimate");
    check(!result.ok(), "and the result does not report itself as usable");
    check(result.error.empty(), "cancellation is not an error - it is a different outcome");

    // Reusing the same token after a reset has to work, because the UI keeps one per item.
    token.reset();
    const auto again = estimator.estimateFrame(pictures, params);
    check(!again.cancelled && again.ok(), "resetting the token lets the next estimate run");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
