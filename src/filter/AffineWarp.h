// Bilinear affine warp of 8/10-bit planes, and of whole 4:2:0 frames, for the stabilised Y4M export.
//
// Qt-free and OpenCV-free, like the rest of the analysis core, so it builds in the unit tests
// without either.
//
// Coordinates are continuous with sample centres at +0.5: luma sample (i, j) sits at
// (i + 0.5, j + 0.5) and covers [i, i+1) x [j, j+1). That is the convention of the generator
// (pixel centre px + 0.5) and of the block centres the global model is fitted on, so a model or a
// correction expressed in those coordinates is applied without a half-pixel slip.
//
// The warp is an inverse mapping: every output position p is read from the input at
//   q = (p0 + p2 x + p3 y,  p1 + p4 x + p5 y),
// the same 2x3 form as bda::me::GmModelResult::p. Samples are interpolated bilinearly and positions
// outside the picture take the nearest edge sample (border replicate).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace bda::filter
{

using AffineParams = std::array<double, 6>; //!< x' = p0 + p2 x + p3 y,  y' = p1 + p4 x + p5 y

inline constexpr AffineParams kAffineIdentity{0, 0, 1, 0, 0, 1};

/* Where 4:2:0 chroma samples sit relative to luma. The offset o (luma units) is
 *   luma position of chroma sample centre = 2 * (chroma position) + o,
 * with both positions in the continuous +0.5 convention above.
 *   Centre   o = (0, 0)       JPEG / MPEG-1 / Y4M C420jpeg: chroma in the middle of its 2x2 luma
 *   Left     o = (-0.5, 0)    MPEG-2 / H.264 default (Y4M C420mpeg2): co-sited horizontally
 *   TopLeft  o = (-0.5, -0.5) co-sited with the top-left luma sample (Y4M C420paldv, and what
 *                             the sequence generator samples)
 * Only the translation of the chroma warp depends on it: for luma A, b the chroma warp is
 *   A_c = A,  b_c = (A o + b - o) / 2,
 * so a pure translation halves exactly whatever the siting, and the siting matters only once the
 * linear part is not the identity (rotation, zoom).
 */
enum class ChromaSiting
{
  Centre,
  Left,
  TopLeft,
};

//!< The chroma-plane warp that goes with a luma warp, see ChromaSiting.
AffineParams chromaWarpParams(const AffineParams &luma, ChromaSiting siting);

/* Warp one plane of 16-bit samples (8 or 10 bit values) into `out`, same size as the input.
 * Rounds to nearest and clips to [0, (1 << bitDepth) - 1].
 */
void warpPlane(const std::uint16_t      *in,
               int                       width,
               int                       height,
               const AffineParams       &params,
               int                       bitDepth,
               std::vector<std::uint16_t> &out);

struct Frame420u16
{
  int                        width{}, height{}; //!< Luma size; chroma is width/2 x height/2 (Y4mWriter)
  std::vector<std::uint16_t> y, u, v;           //!< u and v empty for 4:0:0
};

//!< Luma with `lumaParams`, chroma (when present) with chromaWarpParams(lumaParams, siting).
Frame420u16 warpFrame420(const Frame420u16 &in, const AffineParams &lumaParams, int bitDepth,
                         ChromaSiting siting = ChromaSiting::Centre);

} // namespace bda::filter
