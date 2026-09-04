// SVT-AV1 v4.2.0 integer motion estimation, reproduced for analysis.
//
// Structure follows svt_aom_motion_estimation_b64():
//
//   me_static_b64_bypass()   static block early exit          (kept - see MeParams::staticBypass)
//   hme_level_0()            on the sixteenth picture
//   hme_level_1()            on the quarter picture, centred on the level 0 result
//   hme_level_2()            on the full picture, centred on the level 1 result
//   check_00_center()        zero MV against the HME centre
//   integer_search_b64()     full-pel search around the final centre
//
// Deliberately NOT reproduced, and why:
//
//   prehme_b64()                  preset gating unconfirmed, so reproducing it would be guesswork
//   hme_prune_ref_and_adjust_sr() multi-reference pruning; meaningless with one reference
//   get_hme_l0_search_area() /    SVT sizes the search area from picture distance and from the
//   apply_me_sa_boost()           SAD it is seeing. We use fixed per-level areas scaled by the
//                                 frame interval instead - see kHmeL0Area and friends. This is
//                                 the largest approximation in this file and the first thing to
//                                 revisit if vectors disagree with the encoder.
//
// Qt-free by design - see MeTypes.h.
#pragma once

#include "IMotionEstimator.h"

namespace bda::me
{

/* Search half-extents in each level's own resolution, per axis.
 *
 * SVT derives these at runtime; these are our stand-ins. They are deliberately generous at level 0
 * (where a pixel covers 16 of the original) and tight at level 2, which is the shape SVT's adaptive
 * sizing produces as well.
 */
inline constexpr int kHmeL0Area       = 16; // sixteenth resolution
inline constexpr int kHmeL1Area       = 8;  // quarter resolution
inline constexpr int kHmeL2Area       = 8;  // full resolution
inline constexpr int kIntegerSearchSr = 8;  // full resolution, around the final centre

class SvtIntegerMe : public IMotionEstimator
{
public:
  Algorithm   algorithm() const override { return Algorithm::SvtIntegerMe; }
  const char *nativeCostName() const override { return "SAD"; }

  bool canRun(const MePictureSet &pictures, std::string *reason = nullptr) const override;

  MeFrameResult estimateFrame(const MePictureSet &pictures, const MeParams &params) override;
};

} // namespace bda::me
