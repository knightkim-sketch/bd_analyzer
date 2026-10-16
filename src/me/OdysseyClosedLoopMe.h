// odyssey closed-loop ME (encoder/av1/me.c, closed_loop_superblock_motion_estimate), SAD path.
//
// Per 64x64 superblock, every square block (1x64, 4x32, 16x16, 64x8) is searched on its own:
//   1. the centre is a starting vector for that block - in odyssey the open-loop result of the same
//      block and size, here the co-located block of a seed estimate the user picks, or (0,0);
//   2. an exhaustive full-pel window mvd in [-4, 3] x [-4, 3] (cl_sr 8) around it, clamped like
//      clamp_search_range() with a 64-pixel pad, scanned mvd_x outer / mvd_y inner, at
//      SAD + odysseyClosedLoopRateTerm(mvd), keeping the best and the second best;
//   3. a 64-point sub-pel search around the best: eighth-pel when high precision MVs are allowed
//      and the best and second best are neighbours (epel_eval_window), quarter-pel otherwise. Pure
//      SAD of the AV1 single-reference prediction (8-tap regular, or 4-tap regular without high
//      precision), the full-pel cost standing as the initial minimum.
//
// What is not reproduced: the interpolation filter search of switchable-filter frames (model RD
// with lambda, qindex and adapted filter CDFs, me.c:458-507) and rectangular blocks. odyssey
// searches its reconstructed reference; the frame pair given here is searched as it is.
#pragma once

#include <cstdint>

#include "IMotionEstimator.h"

namespace bda::me
{

struct OdyClOptions
{
  bool allowHighPrecisionMv = true; //!< frame_header.allow_high_precision_mv
  int  searchWidth          = 8;    //!< cl_sr_w
  int  searchHeight         = 8;    //!< cl_sr_h
};

//!< One block's outcome, with the full-pel stage kept so it can be checked against odyssey's trace.
struct OdyClBlock
{
  int          centreX = 0, centreY = 0; //!< Full-pel search centre
  int          minMvdX = 0, maxMvdX = 0, minMvdY = 0, maxMvdY = 0;
  MotionVector intBest, intSecond; //!< Full-pel vectors, stored in full pels (not eighth)
  std::int64_t intCost = 0;        //!< SAD + rate of intBest
  bool         eighthPel = false;
  MotionVector mv;   //!< Final vector, eighth-pel
  std::int64_t sad = 0; //!< Final cost: sub-pel SAD, or the full-pel SAD + rate when nothing beat it
};

/* AV1 luma prediction of one size x size block at (x, y) with an eighth-pel vector, as odyssey's
 * predict_inter_block_plane() does it for a single reference: reference reads clamped to the
 * picture, InterRound0 3, InterRound1 11, clipped to 8 bits. `out` holds size * size samples.
 */
void odysseyPredictBlock(const MePlane &ref, int x, int y, int size, MotionVector mv, bool regular8Tap,
                         std::uint8_t *out);

//!< SAD of that prediction against the source, source reads clamped to the picture.
std::int64_t odysseyClosedLoopSad(const MePlane &src, const MePlane &ref, int x, int y, int size,
                                  MotionVector mv, bool regular8Tap);

//!< Search one square block; `centreMv` is the starting vector in eighth-pel.
OdyClBlock odysseyClosedLoopSearchBlock(const MePlane &src, const MePlane &ref, int x, int y, int size,
                                        MotionVector centreMv, const OdyClOptions &options);

class OdysseyClosedLoopMe : public IMotionEstimator
{
public:
  Algorithm   algorithm() const override { return Algorithm::OdysseyClosedLoop; }
  const char *nativeCostName() const override { return "sub-pel SAD"; }
  bool        canRun(const MePictureSet &pictures, std::string *reason = nullptr) const override;

  /* params.seed, when set, gives each block its centre: the vector of the seed result's block with
   * the same position and size (a missing one counts as (0,0)). Without a seed every block starts
   * from (0,0).
   */
  MeFrameResult estimateFrame(const MePictureSet &pictures, const MeParams &params) override;
};

} // namespace bda::me
