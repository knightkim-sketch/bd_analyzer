// Odyssey open-loop (lookahead) motion estimation, reproduced for analysis.
//
// Structure follows ods_me_search() (analyzer/source/openloop_me.c:943):
//
//   step 1  ods_me_construct_candidates()  centre candidates + pmv + mv_rate_weight
//   step 2  ods_me_determine_center()      on the HALF picture, 32x32, +-1 refine, SAD + linear rate
//   step 3a ods_me_full_search_area()      "quarter": the half picture with an MV step of 2
//   step 3b ods_me_full_search_area()      half picture, step 1
//   step 4  ods_me_full_search_vbs()       on the half -> org upscaled picture, SSE + squared rate
//
// The picture handling is the part most easily got wrong, so to be explicit:
//
//   * The centre decision (step 2) runs on the mean-pooled HALF picture.
//   * The final VBS search (step 4) runs on `half_to_org` - the half picture put back up to full
//     size with the bilinear upscaler. It is NOT the original: feeding the source picture there
//     gives different SSE and different vectors.
//   * A 10-bit source is shifted down to 8-bit before any of this.
//
// WHAT IS NOT REPRODUCED, and why it matters:
//
//   ods_me_construct_candidates() is modelled, not copied. The original tracks per-tile hardware
//   pipeline availability (hw_avail_b/c/d keyed off how many superblocks have been processed in
//   the row), carries an A_PASS candidate across superblocks, and maintains pre/post candidate
//   lists. What is reproduced here is the spatial predictor shape - a left neighbour delayed by
//   the hardware pipeline, an above neighbour from a line buffer, and get_median_mv()'s selection
//   rules - plus the zero candidate.
//
//   Consequence: where our vectors disagree with the encoder, the candidate list is the first
//   place to look, not the search. Everything downstream of it - the four search stages, the cost
//   definitions, the picture each stage runs on, the fixed 8x8 candidate order - follows the
//   source.
//
//   Temporal MV candidates are also absent. They are gated on latency_mode == LATENCY_HIGH in the
//   encoder, and an analysis tool holding two pictures has no temporal MV field to offer anyway.
//
// Qt-free by design - see MeTypes.h.
#pragma once

#include <vector>

#include "IMotionEstimator.h"

namespace bda::me
{

// Search extents, from common/include/ody_me_common.h.
inline constexpr int kOdyQuarterMaxSr = 32; // ODY_ME_QUARTER_MAX_SR_W/H
inline constexpr int kOdyHalfMaxSr    = 8;  // ODY_ME_3STEP_HALF_MAX_SR_W/H
inline constexpr int kOdyFullMaxSr    = 16; // ODY_ME_FULL_MAX_SR_W/H
inline constexpr int kOdyHalfCentres  = 4;  // ODY_ME_3STEP_NUM_HALF_CENTERS

class OdysseyOpenLoopMe : public IMotionEstimator
{
public:
  Algorithm   algorithm() const override { return Algorithm::OdysseyOpenLoop; }
  const char *nativeCostName() const override { return "SSE + rate^2"; }

  bool canRun(const MePictureSet &pictures, std::string *reason = nullptr) const override;

  MeFrameResult estimateFrame(const MePictureSet &pictures, const MeParams &params) override;

private:
  /* The neighbour motion vector state, which is why this estimator cannot run one block in
   * isolation.
   *
   * odyssey models a hardware shift register: the left neighbour is not the superblock just
   * finished but one several steps back in the pipeline (nbmv_a_before[0..2]), and the above
   * neighbour comes out of a line buffer holding one vector per superblock column. The register
   * is cleared at the start of every superblock row - ods_me_nbmv_row_reset() carries the comment
   * "Must be called at the start of each new SB row within a tile to match HW behavior".
   *
   * Scanning superblocks out of order, or estimating only the one the user clicked, therefore
   * produces different predictors and different vectors.
   */
  struct NeighbourState
  {
    std::vector<MotionVector> lineBuffer;    // one per superblock column: the row above
    MotionVector              left;          // A, as seen by the pipeline
    MotionVector              leftDelay[3]{}; // nbmv_a_before[0..2]
    MotionVector              above;         // D, the line buffer entry before it is overwritten

    void resetRow();
    void update(int sbCol, const MotionVector &b64Mv);
  };
};

} // namespace bda::me
