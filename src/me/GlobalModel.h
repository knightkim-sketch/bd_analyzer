// A global motion model fitted to block ME vectors with RANSAC (Qt-free, OpenCV-free).
//
// The points are the blocks of one ME result: (block centre) -> (block centre + MV). The model maps a
// position in the current picture to its position in the reference, so for every inlier
//   ref = model(cur),  MV = model(cur) - cur,
// the same sign convention as the block vectors (pred(x) = ref(x + mv)).
//
// The RANSAC follows libaom (aom_dsp/flow_estimation/ransac.c, v3.15.0): minimal samples drawn with
// the same LCG seeded with the point count, a fixed number of trials, inliers within 1.25 px, most
// inliers wins and fewer squared error breaks a tie, then least-squares refits on the inliers until
// their number stops growing. The result is deterministic: the same input gives the same model on
// every run and platform.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "MeTypes.h"

namespace bda::me
{

// GmModelType is in MeTypes.h, so MeParams can name it.

const char *gmModelTypeName(GmModelType type);

struct GmModelParams
{
  GmModelType type      = GmModelType::RotZoom;
  BlockSize   fitSize   = BlockSize::Blk16; //!< Which block size of the ME result supplies the points
  double      threshold = 1.25;             //!< Inlier distance in pixels (libaom INLIER_THRESHOLD)
  int         trials    = 20;               //!< libaom NUM_TRIALS
  int         refines   = 5;                //!< libaom NUM_REFINES
};

struct GmModelResult
{
  GmModelType           type = GmModelType::RotZoom;
  std::array<double, 6> p{0, 0, 1, 0, 0, 1}; //!< Affine form: x' = p0 + p2 x + p3 y, y' = p1 + p4 x + p5 y

  int total   = 0; //!< Points offered (blocks of fitSize inside the picture; static-bypass ones count as (0,0))
  int inliers = 0;
  double rms  = 0; //!< Over the inliers, pixels

  std::vector<MeBlock>      blocks; //!< The points, in the order of `inlier`
  std::vector<std::uint8_t> inlier;

  std::string error; //!< Set when no model could be fitted (too few points or inliers)
  bool        ok() const { return this->error.empty(); }

  // Readable summaries of the linear part (rotzoom reading: scale and angle; affine: of its average).
  double scale() const;
  double angleDegrees() const;
};

//!< `width`/`height`: the picture, so blocks hanging past its edge are not used as points.
GmModelResult fitGlobalModel(const MeFrameResult &me, int width, int height, const GmModelParams &params);

//!< The vector the model gives at a position of the current picture, eighth-pel, rounded.
MotionVector gmModelMvAt(const GmModelResult &model, double x, double y);

/* A full ME result made from the model: every square size over the 64x64 superblock grid of a
 * width x height picture, each block with the model's vector at its centre. Used as the start
 * vectors of odyssey closed-loop.
 */
MeFrameResult synthesiseModelSeed(const GmModelResult &model, int width, int height);

} // namespace bda::me
