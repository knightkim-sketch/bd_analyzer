// Writes planar 4:2:0 (or mono) frames as YUV4MPEG2, 8 or 10 bit.
//
// Qt-free, like the rest of src/filter. The output is what me::YuvLumaReader::openY4m reads back
// (C420jpeg / C420p10 / Cmono), so a filtered sequence can be fed to the Global motion window or the
// CLI, which read files rather than playlist items.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace bda::filter
{

struct Y4mFormat
{
  int  width{};
  int  height{};
  int  bitDepth{8}; //!< 8 or 10. 10-bit samples are 16-bit little endian, as Y4M requires.
  bool mono{false}; //!< 4:0:0, no chroma planes. 8 bit only (Cmono).
  int  fpsNumerator{30};
  int  fpsDenominator{1};

  std::int64_t lumaBytes() const;
  std::int64_t chromaBytes() const; //!< One chroma plane; 0 for mono.
};

class Y4mWriter
{
public:
  bool open(const std::string &path, const Y4mFormat &format, std::string &error);

  /* One frame. u and v are ignored for mono. Each plane must hold lumaBytes() / chromaBytes(). */
  bool writeFrame(const std::uint8_t *y,
                  const std::uint8_t *u,
                  const std::uint8_t *v,
                  std::string        &error);

  int framesWritten() const { return this->frames; }

  //!< The stream header line, without the newline. Exposed for the unit test.
  static std::string header(const Y4mFormat &format);

private:
  std::ofstream file;
  Y4mFormat     format;
  int           frames{};
};

} // namespace bda::filter
