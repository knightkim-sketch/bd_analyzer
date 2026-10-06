#include "YuvLumaReader.h"

#include <sstream>

namespace bda::me
{

std::size_t YuvLumaReader::frameBytes() const
{
  // Chroma rounds up, as Y4M writers (ffmpeg) size it; for even sizes this is plain w * h / 4.
  const std::size_t luma   = std::size_t(this->w) * std::size_t(this->h);
  const std::size_t chroma = this->hasChroma ? 2 * std::size_t((this->w + 1) / 2) *
                                                   std::size_t((this->h + 1) / 2)
                                             : 0;
  return (luma + chroma) * (this->depth > 8 ? 2 : 1);
}

bool YuvLumaReader::openY4m(const std::string &path, std::string &error)
{
  this->file.open(path, std::ios::binary);
  if (!this->file)
  {
    error = "cannot open " + path;
    return false;
  }

  std::string header;
  if (!std::getline(this->file, header) || header.rfind("YUV4MPEG2 ", 0) != 0)
  {
    error = path + " is not a Y4M file (no YUV4MPEG2 header)";
    return false;
  }

  std::string colour = "420jpeg"; // the Y4M default when C is absent
  std::istringstream tokens(header.substr(10));
  for (std::string t; tokens >> t;)
  {
    if (t[0] == 'W')
      this->w = std::stoi(t.substr(1));
    else if (t[0] == 'H')
      this->h = std::stoi(t.substr(1));
    else if (t[0] == 'C')
      colour = t.substr(1);
  }
  if (this->w <= 0 || this->h <= 0)
  {
    error = "Y4M header has no picture size";
    return false;
  }

  if (colour == "420" || colour == "420jpeg" || colour == "420mpeg2" || colour == "420paldv")
    this->depth = 8;
  else if (colour == "420p10")
    this->depth = 10;
  else if (colour == "mono")
  {
    this->depth     = 8;
    this->hasChroma = false;
  }
  else
  {
    error = "Y4M colour space C" + colour + " is not supported (4:2:0 or mono, 8 or 10 bit)";
    return false;
  }

  /* Each frame is "FRAME[ params]\n" then the samples. The frame header may carry parameters, so
   * its length is read per frame rather than assumed.
   */
  const auto bytes = std::int64_t(this->frameBytes());
  const auto start = std::int64_t(this->file.tellg());
  this->file.seekg(0, std::ios::end);
  const auto end = std::int64_t(this->file.tellg());
  this->file.seekg(start);

  std::string line;
  while (std::getline(this->file, line))
  {
    if (line.rfind("FRAME", 0) != 0)
    {
      error = "Y4M frame " + std::to_string(this->frameOffsets.size()) +
              " does not start with FRAME";
      return false;
    }
    const auto at = std::int64_t(this->file.tellg());
    if (at + bytes > end)
      break; // a truncated last frame is dropped rather than read short
    this->frameOffsets.push_back(at);
    this->file.seekg(at + bytes);
  }
  this->file.clear();
  if (this->frameOffsets.empty())
  {
    error = "Y4M file holds no complete frame";
    return false;
  }
  return true;
}

bool YuvLumaReader::openRaw(const std::string &path,
                            const int          width,
                            const int          height,
                            const int          bitDepth,
                            std::string &      error)
{
  if (width <= 0 || height <= 0 || (bitDepth != 8 && bitDepth != 10))
  {
    error = "raw input needs --size WxH and --bitdepth 8 or 10";
    return false;
  }
  this->file.open(path, std::ios::binary | std::ios::ate);
  if (!this->file)
  {
    error = "cannot open " + path;
    return false;
  }
  this->w     = width;
  this->h     = height;
  this->depth = bitDepth;

  const auto size  = std::int64_t(this->file.tellg());
  const auto bytes = std::int64_t(this->frameBytes());
  for (std::int64_t at = 0; at + bytes <= size; at += bytes)
    this->frameOffsets.push_back(at);
  if (this->frameOffsets.empty())
  {
    error = "the file is smaller than one " + std::to_string(width) + "x" +
            std::to_string(height) + " frame";
    return false;
  }
  return true;
}

MePlane YuvLumaReader::readLuma(const int k, const int pad, std::string &error)
{
  if (k < 0 || k >= this->frameCount())
  {
    error = "frame " + std::to_string(k) + " is outside the file (" +
            std::to_string(this->frameCount()) + " frames)";
    return {};
  }
  const std::size_t samples = std::size_t(this->w) * std::size_t(this->h);
  this->file.clear();
  this->file.seekg(this->frameOffsets[std::size_t(k)]);

  if (this->depth == 8)
  {
    std::vector<std::uint8_t> luma(samples);
    if (!this->file.read(reinterpret_cast<char *>(luma.data()), std::streamsize(samples)))
    {
      error = "short read in frame " + std::to_string(k);
      return {};
    }
    return MePlane::fromLuma8(luma.data(), this->w, this->h, this->w, pad);
  }

  // 10-bit samples are stored little endian in two bytes.
  std::vector<std::uint8_t> raw(samples * 2);
  if (!this->file.read(reinterpret_cast<char *>(raw.data()), std::streamsize(raw.size())))
  {
    error = "short read in frame " + std::to_string(k);
    return {};
  }
  std::vector<std::uint16_t> luma(samples);
  for (std::size_t i = 0; i < samples; ++i)
    luma[i] = std::uint16_t(raw[2 * i] | (raw[2 * i + 1] << 8));
  return MePlane::fromLuma10AsShifted8(luma.data(), this->w, this->h, this->w, pad);
}

} // namespace bda::me
