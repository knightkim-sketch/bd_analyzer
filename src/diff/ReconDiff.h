// Compare two reconstructed frames sample by sample.
//
// Layer D of the Find diff design. Steps 3 and 4 say where the two bitstreams first part company;
// this says where the pictures do. The two are not the same place: a divergence inside a hidden
// alternate reference is invisible until a later frame predicts from it, and a residual coded
// differently can still reconstruct to the same samples.
//
// Qt-free. The caller decodes and hands over the raw planar buffers; this only does the arithmetic,
// so it can be unit tested without a decoder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bda::diff
{

/* How the planes of one raw frame sit in its buffer: consecutive, each width * height samples. */
struct FrameLayout
{
  struct Plane
  {
    std::string name;
    int         width{}, height{};
  };
  std::vector<Plane> planes;
  unsigned           bitDepth{8};
  bool               bigEndian{}; //!< Only read when samples are wider than one byte.

  int         bytesPerSample() const { return this->bitDepth > 8 ? 2 : 1; }
  std::size_t bytesPerFrame() const;
};

struct PlaneDiff
{
  std::uint64_t sse{};
  std::uint64_t samples{};
  std::uint64_t differingSamples{};
  /* The first differing sample in raster order, in this plane's own coordinates - (x, y), not
   * (row, col), and in chroma samples for a chroma plane. Unset when the plane is identical.
   */
  std::optional<int> firstX, firstY;

  bool identical() const { return this->differingSamples == 0; }
};

struct FrameDiff
{
  std::vector<PlaneDiff> planes; //!< Same order as FrameLayout::planes.

  bool identical() const;
};

/* Compare two frames of the same layout. Both buffers must hold at least layout.bytesPerFrame(). */
FrameDiff compareFrames(const std::uint8_t *a, const std::uint8_t *b, const FrameLayout &layout);

/* PSNR in dB, or nothing for an identical plane - which has no finite PSNR, and reporting infinity
 * would poison any average built on it.
 */
std::optional<double> psnr(const PlaneDiff &plane, unsigned bitDepth);

} // namespace bda::diff
