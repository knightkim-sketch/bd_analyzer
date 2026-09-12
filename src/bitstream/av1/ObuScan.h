// Just enough AV1 OBU structure to answer one question: does this packet code a picture, or does
// it only re-show one that was already decoded?
//
// Qt-free and dependency-free, like src/bdrate and src/assist, so it unit tests on its own.
//
// Why this exists: a `show_existing_frame` packet is five bytes and carries no blocks. The rest of
// the application treats it as an ordinary display frame, so the bitstream dump showed those five
// bytes with an empty highlight range, and the per-block statistics kept reporting whatever the
// previously coded frame left behind. Both need to know what kind of frame they are looking at,
// and neither has a parser to hand - the item does not keep one for the FFmpeg input path.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace bda::bitstream::av1
{

//!< OBU types this scan cares about. The rest are skipped by size without being interpreted.
enum class ObuType : std::uint8_t
{
  SequenceHeader = 1,
  TemporalDelimiter = 2,
  FrameHeader    = 3,
  TileGroup      = 4,
  Metadata       = 5,
  Frame          = 6,
  RedundantFrameHeader = 7,
};

struct ShowExistingFrame
{
  /* Which of the eight reference slots is being re-shown. Not a frame number: turning it into one
   * means tracking what each slot held, which the decoder does and we do not.
   */
  std::uint8_t frameToShowMapIdx{};
};

/* Scan one demuxed packet.
 *
 * Returns the show-existing details when the packet's frame header sets `show_existing_frame`,
 * and nothing when it codes a picture normally or cannot be read.
 *
 * Deliberately shallow: it walks OBU headers, finds the frame header, and reads the first bit of
 * its payload. That bit is `show_existing_frame` for any stream that is not a reduced still
 * picture, which no video stream is. Anything deeper would need the sequence header for context,
 * and the point of this is to answer the question without one.
 */
std::optional<ShowExistingFrame> scanShowExistingFrame(const std::uint8_t *data, std::size_t size);

} // namespace bda::bitstream::av1
