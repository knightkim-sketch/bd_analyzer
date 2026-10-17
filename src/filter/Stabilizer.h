// Digital image stabilisation: per-frame global models -> camera trajectory -> moving-average
// smoothing -> per-frame correction warps.
//
// Qt-free. The input is one model per frame k >= 1 mapping frame k to frame k-1 in the affine form
// of bda::me::GmModelResult (x' = p0 + p2 x + p3 y, y' = p1 + p4 x + p5 y), which maps a position
// of the current picture to where the same content lies in the reference. Frame 0 has no model.
//
// Trajectory. Each inter-frame model is read as a similarity about the picture centre c:
//   ref = c + s R(phi) (cur - c) + t,   t = model(c) - c,  phi and s from the linear part
// (for an affine model, of its rotation-zoom average). The content motion from k-1 to k is its
// inverse: translation -(sR)^-1 t at the centre, angle -phi, log scale -log s. Those deltas are
// summed from frame 0 (all zero there) into the raw trajectory (dx, dy in pixels, angle in
// degrees, log scale). Summing is exact for translation and for rotation / zoom about the centre,
// and a first-order approximation when both are present - the usual DIS trade-off.
//
// Smoothing: centred moving average of radius R, the window clamped to the sequence (frames near
// either end average over the frames that exist, so the weights always sum to one).
// Correction = smoothed - raw. Applying it moves the content of frame k from its raw place to the
// smoothed one: forward p_out = c + S R(a) (p_in - c) + d with (d, a, log S) the correction, which
// the warp needs as its inverse mapping (see warpParams).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "AffineWarp.h"

namespace bda::filter
{

struct TrajectoryPoint
{
  double dx{}, dy{};   //!< Pixels
  double angle{};      //!< Degrees, positive turns content from +x towards +y
  double logScale{};   //!< Natural log of the zoom
};

struct StabilizerFrame
{
  TrajectoryPoint raw, smoothed, correction;
  AffineParams    warp = kAffineIdentity; //!< Inverse mapping to apply to frame k (luma)
};

struct StabilizerParams
{
  int radius = 15; //!< Moving-average radius in frames (window 2R + 1)
};

/* `models[k]` is the model frame k -> k-1; models[0] is ignored (use the identity). A frame whose
 * model could not be fitted is expected to be passed as the identity, i.e. "no motion".
 * Returns one entry per frame.
 */
std::vector<StabilizerFrame> stabilize(const std::vector<AffineParams> &models,
                                       int                              width,
                                       int                              height,
                                       const StabilizerParams          &params = {});

//!< The content motion k-1 -> k that one model frame k -> k-1 implies (see the header comment).
TrajectoryPoint motionFromModel(const AffineParams &model, int width, int height);

//!< The inverse-mapping warp that applies `correction` about the picture centre.
AffineParams warpParams(const TrajectoryPoint &correction, int width, int height);

/* CSV text: header "frame,raw_dx,raw_dy,raw_angle,raw_scale,smooth_dx,...,corr_scale" then one row
 * per frame. Scales are written as factors (exp of the log scale), angles in degrees.
 */
std::string trajectoryCsv(const std::vector<StabilizerFrame> &frames);

} // namespace bda::filter
