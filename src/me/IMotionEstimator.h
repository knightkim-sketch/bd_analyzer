// The interface every estimator implements, and the picture set they are handed.
//
// Qt-free by design - see MeTypes.h.
#pragma once

#include <memory>

#include "MePlane.h"
#include "MeTypes.h"

namespace bda::me
{

/* Everything an estimator may read for one (current, reference) pair.
 *
 * Each estimator prepares its own derived pictures, because they do not agree on how: SVT wants
 * sixteenth- and quarter-resolution pictures built with its own phase-shifted kernel, odyssey
 * wants a half picture built with meanpooling plus that half sent back up through bilinear. Those
 * derived planes are therefore built inside the estimator from `current` and `reference` rather
 * than being fields here - a shared "halfPlane" would inevitably be used by the estimator it was
 * not built for.
 *
 * `current` and `reference` are already 8-bit. For a 10-bit source the caller uses
 * MePlane::fromLuma10AsShifted8, which is what odyssey's open-loop path does; SVT's ME kernels
 * are 8-bit as well.
 */
struct MePictureSet
{
  MePlane current;
  MePlane reference;

  int frameIdx    = 0;
  int refFrameIdx = 0;

  /* Display-order distance from current to reference, signed the same way as
   * MeParams::frameInterval: positive means the reference is in the past.
   *
   * Both encoders scale by this - SVT through svt_aom_get_scaled_picture_distance(), odyssey
   * through the order_hint pair feeding temporal MV scaling and mv_rate_weight - so it is carried
   * explicitly instead of being recovered from the frame indices, which would lose the sign as
   * soon as a reordered stream is involved.
   */
  int refDistance = 1;
};

class IMotionEstimator
{
public:
  virtual ~IMotionEstimator() = default;

  virtual Algorithm algorithm() const = 0;

  /* Human-readable name of the metric behind MeBlockResult::nativeCost, for the column header -
   * "SAD", "SSE + rate^2" and so on. The comparison view has to label these because the numbers
   * are not comparable across algorithms.
   */
  virtual const char *nativeCostName() const = 0;

  /* Whether this estimator can run on the given pictures at all.
   *
   * odyssey closed-loop answers false for a raw YUV pair: it needs a reconstruction to search
   * against and an open-loop result to centre on. Asking up front lets the UI grey the choice out
   * with a reason instead of producing an empty overlay.
   */
  virtual bool canRun(const MePictureSet &pictures, std::string *reason = nullptr) const = 0;

  /* Estimate the whole frame.
   *
   * Whole-frame, not per block, and that is a hard constraint rather than a convenience: odyssey's
   * open-loop predictor is a hardware shift register (ods_me_nbmv_update, reset per superblock row
   * via ods_me_nbmv_row_reset), so a block's candidate list depends on the blocks scanned before
   * it. Estimating "just the block the user clicked" would silently use the wrong predictors.
   *
   * params.blockSizes filters what lands in the result, not what is computed - the variable block
   * size search derives the larger sizes by summing the 8x8 costs, so they all get computed anyway.
   */
  virtual MeFrameResult estimateFrame(const MePictureSet &pictures, const MeParams &params) = 0;
};

using MotionEstimatorPtr = std::unique_ptr<IMotionEstimator>;

/* Build the estimator for an algorithm. Returns null for one that is not implemented yet.
 *
 * Bi-prediction is not a separate algorithm here and deliberately so: odyssey reaches it through
 * ods_bi_me() as a second pass over the same open-loop search, with its own cost kernel, so when
 * it arrives it belongs behind a flag on MeParams and a RefList on the results - both of which are
 * already in place - rather than as a fourth enumerator.
 */
MotionEstimatorPtr makeEstimator(Algorithm algorithm);

} // namespace bda::me
