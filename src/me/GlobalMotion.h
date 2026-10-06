// Frame-level global motion by integral projection: one translation per window of a 4x4 grid.
//
// Design: docs/ai/30-designs/global-motion-design.md. The output is a list of motion vector
// candidates for odyssey's open-loop ME (read by analyzer/source/gm_mv.c), not AV1
// global_motion_params - nothing here reaches a bitstream.
//
// The core idea is in section 3.3 of the design: two cumulative-sum planes per picture (row-wise
// and column-wise) give every window's vertical and horizontal profile, at every displacement, as a
// difference of two lookups. Each axis is then a 1-D search, run independently - the two results
// are never fused into one decision, because a confident axis tied to an unconfident one dies with
// it (the lesson odyssey's PMD left behind).
//
// Qt-free and integer only: the arithmetic is meant to move into the encoder unchanged.
//
// Sign convention, as odyssey consumes it: the vector points from the current picture into the
// reference, pred(x) = ref(x + mv). Content that moved right by D pixels gets dx = -D.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MePlane.h"

namespace bda::me
{

enum class GmSmoothing
{
  Off,
  Binomial3, //!< [1, 2, 1] / 4
  Binomial5, //!< [1, 4, 6, 4, 1] / 16 - the design default
};

enum class GmBrightness
{
  Off,    //!< Plain SAD of the profiles.
  Dc,     //!< Both profiles mean-removed.
  DcGain, //!< Mean-removed, and the reference scaled by a gain fitted at d = 0 - the default.
};

inline constexpr int kGmGrid = 4; //!< Windows per axis. odyssey's ODS_GM_GRID.

struct GmParams
{
  /* Search range in source pixels, before downsampling - still capped at half the smallest window,
   * so 256 reaches its full range only at 4K.
   */
  int          rOrig{256};
  /* Refinement range at source resolution, around the coarse vector; 0 switches it off. Moves the
   * output from the 2^n grid of the downsampled search to full-pel. Not applied at n = 0.
   */
  int          refineRange{16};
  GmSmoothing  smoothing{GmSmoothing::Binomial5};
  GmBrightness brightness{GmBrightness::DcGain};

  /* Gate 1, per axis: cost(d*) * g1Num < cost(0) * g1Den. 16/15 asks for a 6.25% improvement. */
  int g1Num{16}, g1Den{15};
  /* Gate 2, per window: sad2d(dx, dy) * g2Mul < sad2d(0, 0). A real 2-D block SAD with a 2x margin
   * - the gate odyssey's PMD lacked and regressed for (design section 3.7). Do not remove it.
   *
   * Applied per axis as well, with the same margin: an axis is accepted only if the pair also beats
   * the pair without it - sad2d(dx, dy) * g2Mul < sad2d(0, dy) for x. Without this a confident
   * axis carries an unconfident one through the pair gate: on a smooth texture moving only
   * vertically, 9 of 16 windows accepted a spurious horizontal vector at 288p, and none with it.
   */
  int g2Mul{2};

  /* A profile whose rows (or columns) differ from their mean by less than this many grey levels
   * per pixel, on average, is flat: the gain fit is meaningless there and stays at 1.0.
   */
  int flatGreyLevels{1};
};

struct GmAxisResult
{
  int          d{};        //!< Best displacement, downsampled pixels, ref relative to current.
  int          dFull{};    //!< The vector in source pixels: d << n, refined when refinement is on.
  std::int64_t refineCost{}; //!< Profile cost at the refined vector, source resolution; 0 if none.
  std::int64_t costBest{}; //!< cost(d)
  std::int64_t costZero{}; //!< cost(0)
  int          aQ{4096};   //!< Gain, Q12 (4096 = 1.0).
  bool         g1{};
};

struct GmWindowResult
{
  int winI{}, winJ{};   //!< Column, row of the 4x4 grid - odyssey's (win_i, win_j).
  int x0{}, y0{}, w{}, h{}; //!< Window in the downsampled picture.

  GmAxisResult x, y;

  /* Vectors in quarter-pel at source resolution: dFull << 2 (d << (n + 2) without refinement).
   * Written for an axis whether or not it was accepted, so the gates can be re-tuned from the data.
   */
  int dxQpel{}, dyQpel{};
  bool acceptX{}, acceptY{};

  std::int64_t sad2dBest{}; //!< SAD at the pair gate 1 left standing (a rejected axis counts as 0).
  std::int64_t sad2dZero{};
  std::int64_t sad2dNoX{}; //!< The same pair with dx = 0 - what x has to beat on its own.
  std::int64_t sad2dNoY{}; //!< The same pair with dy = 0.
  bool         g2{};       //!< The pair gate.
  bool         g2x{}, g2y{}; //!< The per-axis gates.
};

struct GmFrameResult
{
  int n{};             //!< Downsampling exponent: the picture was reduced by 1 << n.
  int wd{}, hd{};      //!< Downsampled size.
  int rDs{};           //!< Search range in downsampled pixels.
  std::vector<GmWindowResult> windows; //!< kGmGrid * kGmGrid, row-major (winJ, then winI).
  std::string                 error;

  bool ok() const { return this->error.empty(); }
};

/* The smallest n with width >> n <= 512. Height follows with the same n. */
int gmDownsampleExponent(int width);

/* Estimate one window grid between two pictures of the same size.
 *
 * Both planes are full-resolution 8-bit luma; their padding does not matter, the estimator
 * downsamples into planes of its own with the border it needs.
 */
GmFrameResult estimateGlobalMotion(const MePlane &cur, const MePlane &ref, const GmParams &params);

} // namespace bda::me
