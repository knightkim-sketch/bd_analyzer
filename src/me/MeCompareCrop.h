// Cutting a block out of a raw YUV frame for the ME compare popup: the source block and the
// reference block a motion vector points at, as colour, and their luma difference.
//
// Qt-free like the rest of src/me. The frame layout comes in as plain numbers; translating
// upstream's PixelFormatYUV into them is src/integration's job (MeFrameSource).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bda::me
{

/* Where the samples of one planar YUV frame sit. Semi-planar (NV12-style) chroma is one plane at
 * offset[1] with Cb and Cr alternating; offset[2] is then unused.
 */
struct YuvFrameLayout
{
  int          width{}, height{};
  int          bitDepth{8};
  bool         bigEndian{};
  int          subX{2}, subY{2}; //!< Chroma subsampling factors; 0 for luma-only (4:0:0).
  std::int64_t offset[3]{};      //!< Byte offset of the Y, Cb and Cr planes.
  bool         interleavedUV{};
  bool         crFirst{}; //!< Semi-planar only: Cr precedes Cb in each pair (NV21-style).
};

//!< A rectangle in luma pixels, frame coordinates.
struct CropRect
{
  int x{}, y{}, w{}, h{};
};

struct RgbImage
{
  int                       width{}, height{};
  std::vector<std::uint8_t> rgb; //!< RGB888, row-major, no padding.
};

/* Empty when `frameBytes` holds a frame of this layout; otherwise why not, for the popup to show. */
std::string checkLayout(const YuvFrameLayout &layout, std::size_t frameBytes);

/* The block at `rect` as RGB. Pixels outside the frame repeat the nearest edge pixel - the same
 * padding the estimators search over, so a vector reaching past the border shows what was compared.
 * Colour is BT.709 limited range at 8 bit (deeper sources are shifted down), chroma nearest.
 */
RgbImage cropRgb(const std::uint8_t *frame, const YuvFrameLayout &layout, const CropRect &rect);

struct LumaDiff
{
  RgbImage     image;    //!< Grey, 128 = equal, brighter = source above reference.
  std::int64_t sad{};    //!< Sum of |source - reference| over the block, 8-bit luma.
  int          maxAbs{}; //!< Largest single difference, 8-bit luma.
};

/* Source block at `rect` against the reference block at `rect` moved by (dx, dy) whole pixels.
 * Luma is taken at 8 bit (value >> (bitDepth - 8)), as the estimators see it, so `sad` matches the
 * common SAD they report. The image shows 128 + gain * difference, clipped.
 */
LumaDiff lumaDiff(const std::uint8_t   *source,
                  const std::uint8_t   *reference,
                  const YuvFrameLayout &layout,
                  const CropRect       &rect,
                  int                   dx,
                  int                   dy,
                  int                   gain);

} // namespace bda::me
