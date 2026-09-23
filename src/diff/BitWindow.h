// Read a fixed-width window of bits out of a byte buffer, for comparing two streams at a position.
//
// Qt-free on purpose: the caller supplies the frame's bytes and a bit offset that came from the
// decoder, and gets back a plain integer it can compare. Nothing here knows about AV1.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bda::diff
{

/* Bits are numbered from the most significant bit of byte 0, which is how the decoder counts them.
 *
 * Returns false when the window would run past the end of the buffer; `out` is untouched then. A
 * short read is not silently zero-padded, because zero padding compares equal to a real run of
 * zeroes and would report a match that was never read.
 */
bool readBitWindow(const std::uint8_t *data,
                   std::size_t         size,
                   std::uint64_t       startBit,
                   unsigned            count,
                   std::uint64_t &     out);

/* One superblock's comparison point.
 *
 * `startBit` is the smallest block start the decoder reported inside that superblock. Measured on
 * real streams these are monotonic in raster order and mostly non-overlapping, but not exactly:
 * the values are the arithmetic decoder's read positions, so a boundary can sit a few bits off.
 * That is why a mismatch here locates a region rather than naming a symbol.
 */
struct SuperblockProbe
{
  int           x{};        //!< Superblock origin in frame pixels.
  int           y{};
  std::uint64_t startBit{};
  bool          hasBits{};  //!< false when no block in this superblock reported a range.
};

enum class WindowCompare
{
  Equal,
  Differ,
  Unreadable, //!< One side or the other could not supply the whole window.
  Missing,    //!< One side has no probe for this superblock at all.
};

struct SuperblockBitDiff
{
  int           x{};
  int           y{};
  WindowCompare result{};
  std::uint64_t bitsA{};
  std::uint64_t bitsB{};
};

struct BitWindowOptions
{
  unsigned windowBits{24}; //!< 24 is what the workflow has been using; not a spec quantity.
};

/* Compare the window at every superblock the two sides share, in raster order.
 *
 * Probes are matched by superblock position, not by index: a stream may skip a superblock's range
 * entirely, and pairing by index would then compare different parts of the picture.
 */
std::vector<SuperblockBitDiff> compareBitWindows(const std::vector<SuperblockProbe> &probesA,
                                                 const std::uint8_t *dataA,
                                                 std::size_t         sizeA,
                                                 const std::vector<SuperblockProbe> &probesB,
                                                 const std::uint8_t *dataB,
                                                 std::size_t         sizeB,
                                                 const BitWindowOptions &options = {});

/* The first superblock in raster order whose window differs, or nullptr when none does.
 *
 * Unreadable and missing superblocks are not "first differences" - they are gaps in what could be
 * measured, and calling them divergence would send the reader to the wrong place.
 */
const SuperblockBitDiff *firstDiffering(const std::vector<SuperblockBitDiff> &diffs);

} // namespace bda::diff
