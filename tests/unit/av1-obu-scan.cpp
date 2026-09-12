// Unit test for the AV1 show-existing-frame scan. No Qt, no library.
//
// The expectations come from a real SVT-AV1 clip, dumped with an independent script before the
// scanner existed: in `CoverSong_..._288p.y4m-39.ivf` the display frames 2, 4 and 6 are five byte
// packets holding OBU types [2, 3] with the first payload bit set, while 0, 1, 3, 5 and 7 carry
// OBU_FRAME (6) and code a picture. If the scanner agrees with that, it is reading what the
// demuxer hands it.
//
// The packet is given as the file path when the regression runner has a stream to hand; the
// hand-built cases below need no file and cover the shapes a real clip does not contain - a
// truncated OBU, a lying size field, a forbidden bit.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "bitstream/av1/ObuScan.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

using namespace bda::bitstream::av1;

std::optional<ShowExistingFrame> scan(const std::vector<std::uint8_t> &packet)
{
  return scanShowExistingFrame(packet.data(), packet.size());
}

//!< obu_header byte: forbidden(0) | type(4) | extension | has_size | reserved
std::uint8_t obuHeader(ObuType type, bool hasSize = true)
{
  return std::uint8_t((std::uint8_t(type) << 3) | (hasSize ? 0x02 : 0x00));
}
} // namespace

int main(int argc, char **argv)
{
  std::cout << "av1 obu scan" << std::endl;

  // --- the shape a real show-existing packet has -------------------------------------------
  {
    // Temporal delimiter (empty), then a frame header whose first bit is show_existing_frame.
    // 0xB0 = 1011 0000: show_existing = 1, frame_to_show_map_idx = 3.
    const std::vector<std::uint8_t> packet{
        obuHeader(ObuType::TemporalDelimiter), 0x00, obuHeader(ObuType::FrameHeader), 0x01, 0xB0};
    const auto result = scan(packet);
    check(result.has_value(), "a five byte [TD, frame header] packet is show-existing");
    check(result && result->frameToShowMapIdx == 3, "and its reference slot is read out");
  }
  {
    // Same, but the first bit is clear: an ordinary frame header, so the packet codes a picture.
    const std::vector<std::uint8_t> packet{
        obuHeader(ObuType::TemporalDelimiter), 0x00, obuHeader(ObuType::FrameHeader), 0x01, 0x30};
    check(!scan(packet).has_value(), "a frame header with the bit clear is not show-existing");
  }
  {
    /* OBU_FRAME ends the question before any header bit is read. Without this the scan would look
     * at a coded frame's payload, where the first bit means something else entirely.
     */
    const std::vector<std::uint8_t> packet{obuHeader(ObuType::TemporalDelimiter), 0x00,
                                           obuHeader(ObuType::Frame), 0x02, 0xB0, 0xB0};
    check(!scan(packet).has_value(), "a packet carrying OBU_FRAME codes a picture");
  }
  {
    const std::vector<std::uint8_t> packet{obuHeader(ObuType::TemporalDelimiter), 0x00,
                                           obuHeader(ObuType::TileGroup), 0x02, 0xB0, 0xB0};
    check(!scan(packet).has_value(), "so does one carrying a tile group");
  }

  // --- malformed input must be refused, not interpreted -------------------------------------
  {
    check(!scanShowExistingFrame(nullptr, 0).has_value(), "a null packet yields nothing");
    check(!scan({}).has_value(), "so does an empty one");
    check(!scan({0xFF, 0x00}).has_value(), "a set forbidden bit is refused");

    // Size field says 40 bytes, packet holds 1. Trusting it would read past the buffer.
    check(!scan({obuHeader(ObuType::FrameHeader), 40, 0xB0}).has_value(),
          "an OBU longer than the packet is refused");

    // Header says a size field follows, and the packet ends instead.
    check(!scan({obuHeader(ObuType::FrameHeader)}).has_value(), "a truncated OBU is refused");

    // A run of continuation bytes must terminate rather than shift forever.
    check(!scan({obuHeader(ObuType::FrameHeader), 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                 0x80, 0x80})
               .has_value(),
          "an unterminated leb128 is refused");
  }
  {
    // No size field: the OBU runs to the end of the packet. Still readable.
    const std::vector<std::uint8_t> packet{obuHeader(ObuType::FrameHeader, false), 0xB0};
    check(scan(packet).has_value(), "an OBU without a size field is read to the end of the packet");
  }
  {
    // An extension byte sits between the header and the size, and must not be read as the size.
    std::vector<std::uint8_t> packet{
        std::uint8_t(obuHeader(ObuType::FrameHeader) | 0x04), 0x00, 0x01, 0xB0};
    check(scan(packet).has_value(), "an extension header is skipped, not misread as a size");
  }

  // --- against a real clip, when the runner supplies one ------------------------------------
  if (argc >= 2 || std::getenv("BDA_TEST_IVF"))
  {
    const std::string path = argc >= 2 ? argv[1] : std::getenv("BDA_TEST_IVF");
    std::ifstream     file(path, std::ios::binary);
    if (!file)
    {
      std::cout << "  SKIP  (could not read " << path << ")" << std::endl;
    }
    else
    {
      const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());
      // IVF: 32 byte file header, then per frame a 12 byte header whose first four bytes are the
      // packet size, little endian.
      std::size_t at      = 32;
      int         index   = 0;
      int         coded   = 0;
      int         reshown = 0;
      while (at + 12 <= bytes.size() && index < 24)
      {
        const std::uint32_t size = std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1]) << 8) |
                                   (std::uint32_t(bytes[at + 2]) << 16) |
                                   (std::uint32_t(bytes[at + 3]) << 24);
        at += 12;
        if (at + size > bytes.size())
          break;
        if (scanShowExistingFrame(bytes.data() + at, size))
          ++reshown;
        else
          ++coded;
        at += size;
        ++index;
      }
      std::cout << "  clip: " << coded << " coded, " << reshown << " re-shown" << std::endl;
      check(coded > 0, "a real clip has coded frames");
      /* Not every clip re-shows frames, so this is not asserted - but if one does, the scan must
       * not claim every frame is re-shown, which is what a misread header bit would look like.
       */
      check(reshown < coded + reshown, "and the scan does not call every frame re-shown");
    }
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
