// Shared vocabulary for the motion estimation analysis feature.
//
// This header is deliberately Qt-free (and free of anything from the upstream YUView tree) so the
// estimators can be driven from the GUI and from the headless CLI with the same code. Only the
// adapter in src/integration is allowed to know about both sides.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bda::me
{

/* Motion vectors are carried in eighth-pel units throughout this module, even for the estimators
 * that only ever produce whole pixels.
 *
 * The alternative - each estimator reporting in its own unit - was tried on paper and does not
 * survive contact with the comparison view: SVT integer ME and odyssey open-loop ME are full-pel,
 * odyssey closed-loop stores eighth-pel, and its search steps in quarter-pel by default. Drawing
 * those on one overlay means converting somewhere, and doing it once at the producer is the only
 * place where the unit is still known for certain.
 */
struct MotionVector
{
  std::int32_t x = 0; // eighth-pel
  std::int32_t y = 0; // eighth-pel

  static MotionVector fromFullPel(std::int32_t fx, std::int32_t fy) { return {fx * 8, fy * 8}; }
  static MotionVector fromQuarterPel(std::int32_t qx, std::int32_t qy) { return {qx * 2, qy * 2}; }

  bool isZero() const { return this->x == 0 && this->y == 0; }
};

inline bool operator==(const MotionVector &a, const MotionVector &b)
{
  return a.x == b.x && a.y == b.y;
}

/* The block sizes the user can switch on. Square only: odyssey G2.5 does not use rectangular
 * partitions and SVT's ME works on b64 with square sub-blocks, so a rectangular entry here would
 * have nothing to drive it.
 */
enum class BlockSize
{
  Blk8 = 0,
  Blk16,
  Blk32,
  Blk64,
};
inline constexpr int kBlockSizeCount = 4;

inline int blockSizeInPixels(BlockSize size)
{
  switch (size)
  {
  case BlockSize::Blk8:
    return 8;
  case BlockSize::Blk16:
    return 16;
  case BlockSize::Blk32:
    return 32;
  case BlockSize::Blk64:
    return 64;
  }
  return 0;
}

/* A set of block sizes, one bit each - this is what the 8/16/32/64 checkboxes produce.
 *
 * The estimators still compute the whole variable-block-size pyramid internally: odyssey's VBS
 * search derives 16x16 and up by summing the 8x8 costs, so asking it for "64 only" would not save
 * the 8x8 work. The mask selects what is reported and drawn, not what is computed.
 */
class BlockSizeSet
{
public:
  BlockSizeSet() = default;
  explicit BlockSizeSet(unsigned bits) : bits_(bits & 0xFu) {}

  static BlockSizeSet all() { return BlockSizeSet(0xFu); }

  bool contains(BlockSize size) const { return (this->bits_ >> static_cast<unsigned>(size)) & 1u; }
  void add(BlockSize size) { this->bits_ |= 1u << static_cast<unsigned>(size); }
  void remove(BlockSize size) { this->bits_ &= ~(1u << static_cast<unsigned>(size)); }
  bool empty() const { return this->bits_ == 0; }
  unsigned bits() const { return this->bits_; }

private:
  unsigned bits_ = 0;
};

enum class Algorithm
{
  SvtIntegerMe = 0,  // SVT-AV1 v4.2.0, HME + integer search. Full-pel.
  OdysseyOpenLoop,   // odyssey lookahead ME. Full-pel, source reference.
  OdysseyClosedLoop, // odyssey encoder ME. Needs a recon reference and an open-loop centre.
};

/* Which reference a prediction direction points at.
 *
 * Uni-prediction is all the first cut computes, but the shape is here because bi-prediction is
 * coming: odyssey has ods_bi_me() with its own cost kernel, and retro-fitting a direction into
 * the result records later would touch every consumer. A List1-only or bi result slots in without
 * changing the container.
 */
enum class RefList
{
  List0 = 0,
  List1,
};

struct MeBlock
{
  int       x    = 0; // luma pixels, frame coordinates
  int       y    = 0;
  BlockSize size = BlockSize::Blk64;
};

/* One block's outcome.
 *
 * Two costs are reported side by side, and that is on purpose. `nativeCost` is whatever the
 * algorithm itself minimised - and the three do not agree: SVT uses SAD with no MV rate, odyssey
 * open-loop uses SSE plus a squared rate term, odyssey closed-loop uses SAD plus a linear rate
 * that is shifted differently per block size. Comparing those numbers across algorithms is
 * meaningless. `commonSad` is recomputed afterwards for every algorithm the same way - plain SAD
 * of the chosen prediction, no rate, no subsampling - so the columns can actually be lined up.
 */
struct MeBlockResult
{
  MeBlock      block;
  MotionVector mv;
  RefList      refList = RefList::List0;

  std::int64_t nativeCost = 0; // in the algorithm's own metric, see above
  std::int64_t commonSad  = 0; // recomputed identically for every algorithm

  /* Set when the estimator decided the block without searching - SVT's me_static_b64_bypass takes
   * this exit for static blocks. Worth surfacing rather than hiding: a bypassed block has a
   * trustworthy zero MV, not a missing result, and the two look identical on an overlay.
   */
  bool staticBypass = false;
};

struct MeFrameResult
{
  int                        frameIdx    = 0;
  int                        refFrameIdx = 0;
  Algorithm                  algorithm   = Algorithm::SvtIntegerMe;
  std::vector<MeBlockResult> blocks;

  // Filled in when the estimator could not run at all; blocks is then empty.
  std::string error;

  bool ok() const { return this->error.empty(); }
};

struct MeParams
{
  /* Signed on purpose. A positive interval takes the reference from the past
   * (reference = current - interval), a negative one from the future. Both encoders scale their
   * search by the distance between the two pictures - SVT through
   * svt_aom_get_scaled_picture_distance(), odyssey through the order_hint pair - so the sign has
   * to survive all the way down rather than being turned into an absolute value at the edge.
   */
  int frameInterval = 1;

  BlockSizeSet blockSizes = BlockSizeSet::all();
  Algorithm    algorithm  = Algorithm::SvtIntegerMe;

  /* Reproduce SVT's early exit for static blocks. On by default because leaving it out changes
   * which blocks get a searched MV, and the point of this feature is to match the encoder.
   */
  bool staticBypass = true;

  // Debug switch mirrored from odyssey's av1_dbg_srb_zero_center: pin the search centre to (0,0)
  // so a reproduction can be compared stage by stage.
  bool forceZeroCentre = false;
};

} // namespace bda::me
