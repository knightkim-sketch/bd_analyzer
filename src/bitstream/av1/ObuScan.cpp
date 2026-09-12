#include "bitstream/av1/ObuScan.h"

namespace bda::bitstream::av1
{

namespace
{

/* leb128, as AV1 writes OBU sizes: seven bits per byte, high bit continues.
 *
 * Capped at eight bytes because that is the spec's limit, and because a run of 0x80 in a corrupt
 * packet would otherwise walk off the end shifting forever.
 */
bool readLeb128(const std::uint8_t *data, std::size_t size, std::size_t &at, std::uint64_t &value)
{
  value = 0;
  for (int byte = 0; byte < 8; ++byte)
  {
    if (at >= size)
      return false;
    const auto current = data[at++];
    value |= std::uint64_t(current & 0x7f) << (7 * byte);
    if ((current & 0x80) == 0)
      return true;
  }
  return false;
}

} // namespace

std::optional<ShowExistingFrame> scanShowExistingFrame(const std::uint8_t *data, std::size_t size)
{
  if (data == nullptr)
    return std::nullopt;

  std::size_t at = 0;
  while (at < size)
  {
    const auto header = data[at++];

    /* obu_forbidden_bit must be zero. A one means this is not an OBU stream - or that we have lost
     * alignment inside one - and continuing would read noise as structure.
     */
    if ((header & 0x80) != 0)
      return std::nullopt;

    const auto type            = ObuType((header >> 3) & 0x0f);
    const bool hasExtension    = (header & 0x04) != 0;
    const bool hasSizeField    = (header & 0x02) != 0;

    if (hasExtension)
    {
      if (at >= size)
        return std::nullopt;
      ++at;
    }

    std::uint64_t payloadSize = 0;
    if (hasSizeField)
    {
      if (!readLeb128(data, size, at, payloadSize))
        return std::nullopt;
    }
    else
    {
      // No size field: this OBU runs to the end of the packet, so it is necessarily the last.
      payloadSize = size - at;
    }

    if (payloadSize > size - at)
      return std::nullopt; // Declared longer than the packet holds.

    const auto payload = at;
    at += std::size_t(payloadSize);

    /* A coded picture ends the question: OBU_FRAME carries its own frame header, and a tile group
     * means the header before it described a frame that is actually coded here.
     */
    if (type == ObuType::Frame || type == ObuType::TileGroup)
      return std::nullopt;

    if (type == ObuType::FrameHeader && payloadSize > 0)
    {
      /* uncompressed_header() opens with show_existing_frame, then frame_to_show_map_idx(3), for
       * every stream that is not a reduced still picture. Verified against ffmpeg-written AV1: the
       * re-shown frames in an SVT-AV1 clip are five byte packets holding exactly a temporal
       * delimiter and this header, with the top bit set.
       */
      const auto first = data[payload];
      if ((first & 0x80) == 0)
        return std::nullopt; // A real frame header. This packet codes a picture.

      ShowExistingFrame result;
      result.frameToShowMapIdx = std::uint8_t((first >> 4) & 0x07);
      return result;
    }
  }

  return std::nullopt;
}

} // namespace bda::bitstream::av1
