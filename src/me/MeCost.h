// Distortion and rate kernels, one per variant the encoders actually use.
//
// There is no single "cost" function here on purpose. The three algorithms disagree on every axis:
//
//   SVT integer ME              SAD,  no MV rate
//   odyssey open-loop step 2    SAD,  linear rate, divide_round(rate, 2)
//   odyssey open-loop step 3/4  SSE,  squared rate, (rate * rate) << 6
//   odyssey closed-loop full-pel SAD, linear rate, shifted per block size
//   odyssey closed-loop subpel  SAD,  no MV rate
//
// Collapsing those into one parameterised call was tried and made every call site read as a pile
// of flags, so each variant is spelled out and named after where it comes from.
//
// Qt-free by design - see MeTypes.h.
#pragma once

#include <cstdint>

#include "MePlane.h"
#include "MeTypes.h"

namespace bda::me
{

/* Sum of absolute differences over a block.
 *
 * `rowStep` reproduces the row subsampling both encoders use to halve the work: odyssey passes a
 * search stride of 2 into compute_closed_loop_inter_block_sad, and SVT's HME levels subsample
 * through `subsample_sad`. The result is the sum over the rows actually visited - neither encoder
 * scales it back up, so neither do we. Pass 1 for the full sum.
 */
std::int64_t blockSad(const MePlane &src,
                      int             srcX,
                      int             srcY,
                      const MePlane  &ref,
                      int             refX,
                      int             refY,
                      int             width,
                      int             height,
                      int             rowStep = 1);

/* Sum of squared errors, same subsampling rule. This is what odyssey open-loop minimises from
 * step 3 onward (ods_me_compute_sse), despite the variables still being called `sad`.
 */
std::int64_t blockSse(const MePlane &src,
                      int             srcX,
                      int             srcY,
                      const MePlane  &ref,
                      int             refX,
                      int             refY,
                      int             width,
                      int             height,
                      int             rowStep = 1);

/* odyssey's ods_me_get_mvcost(), including the clamp at 1919 and the four-table lookup.
 *
 * Takes the motion vector *difference* against the predictor, in whatever unit the caller's stage
 * works in - odyssey shifts the difference into the table's unit before calling, and so must the
 * caller here. Reproduces the zero-filled tail of lv4; see OdysseyMvRateLut.h for why.
 */
int odysseyMvCost(std::int32_t mvdX, std::int32_t mvdY);

/* The rate term as odyssey's open-loop search adds it: squared, then scaled from the 8x8 basis to
 * the block's basis. `mvRateWeight` is the per-block weight the candidate constructor produced.
 */
std::int64_t odysseyOpenLoopRateTerm(int mvdX, int mvdY, int mvRateWeight);

/* The rate term as odyssey's VBS stage adds it - a different formula from the one above, and the
 * difference is easy to miss:
 *
 *   ods_me_full_search_area()  rate against the PREDICTOR, weighted, divided by 6, then << 6 to
 *                              lift an 8x8 basis up to the 64x64 block it is scoring
 *   ods_me_full_search_vbs()   rate against the SEARCH CENTRE, unweighted, divided by 8, squared,
 *                              and added to each 8x8 - the larger sizes then inherit it by summing
 *                              the 8x8 costs, so the area scaling happens for free
 *
 * Using the first form in the VBS stage makes the rate swamp the SSE and every block collapses to
 * the centre vector.
 */
std::int64_t odysseyVbsRateTerm(int mvdFromCentreX, int mvdFromCentreY);

/* The rate term as odyssey's closed-loop full-pel search adds it: linear, and shifted by the
 * block's area relative to 64x64, because ods_me_get_mvcost() is defined on a 64x64 basis.
 *   8x8 -> (rate + 32) >> 6,  16x16 -> (rate + 8) >> 4,  32x32 -> (rate + 2) >> 2,  64x64 -> rate
 */
std::int64_t odysseyClosedLoopRateTerm(int mvdX, int mvdY, BlockSize size);

/* Restrict a search range so every read stays inside the reference plane and its border.
 *
 * Both encoders do this explicitly and it is not an optimisation: SVT corrects each HME level's
 * search origin against the picture ("Correct the left edge of the Search Area if it is not on the
 * reference picture", motion_estimation.c:811 and the three that follow), and odyssey runs every
 * stage through clamp_search_range(). Leaving it out reads past the allocated plane - the border
 * is finite - which is undefined behaviour rather than a slightly wrong vector.
 *
 * The range is inclusive on both ends. An empty range comes back as min > max, which the callers
 * treat as "nothing to search here".
 */
struct SearchRange
{
  int minDx = 0;
  int maxDx = 0;
  int minDy = 0;
  int maxDy = 0;

  bool empty() const { return this->minDx > this->maxDx || this->minDy > this->maxDy; }
};

SearchRange clampSearchRange(const MePlane &ref,
                             int            blockX,
                             int            blockY,
                             int            blockWidth,
                             int            blockHeight,
                             int            wantMinDx,
                             int            wantMaxDx,
                             int            wantMinDy,
                             int            wantMaxDy);

/* The common metric: plain SAD of the chosen prediction, every row, no rate.
 *
 * This exists so the three algorithms can be put in one table. Their native costs cannot be
 * compared - different distortion measures, different rate handling, different subsampling - so
 * every result also carries this, recomputed the same way from the same pair of planes. Reported
 * alongside the native cost rather than instead of it: the native number is what the algorithm
 * actually chose on, and hiding it would make a disagreement impossible to explain.
 *
 * Takes an eighth-pel MV and uses only its whole-pixel part, so full-pel and subpel estimators
 * can be measured on the same footing.
 */
std::int64_t commonMetricSad(const MePlane      &src,
                             const MePlane      &ref,
                             const MeBlock      &block,
                             const MotionVector &mvEighthPel);

} // namespace bda::me
