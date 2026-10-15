#include "Y4mWriter.h"

namespace bda::filter
{

std::int64_t Y4mFormat::lumaBytes() const
{
  return std::int64_t(this->width) * this->height * (this->bitDepth > 8 ? 2 : 1);
}

std::int64_t Y4mFormat::chromaBytes() const
{
  if (this->mono)
    return 0;
  return std::int64_t(this->width / 2) * (this->height / 2) * (this->bitDepth > 8 ? 2 : 1);
}

namespace
{
std::string colourTag(const Y4mFormat &format)
{
  if (format.mono)
    return "Cmono";
  return format.bitDepth > 8 ? "C420p10 XYSCSS=420P10" : "C420jpeg";
}
} // namespace

std::string Y4mWriter::header(const Y4mFormat &format)
{
  return "YUV4MPEG2 W" + std::to_string(format.width) + " H" + std::to_string(format.height) +
         " F" + std::to_string(format.fpsNumerator) + ":" + std::to_string(format.fpsDenominator) +
         " Ip A1:1 " + colourTag(format);
}

bool Y4mWriter::open(const std::string &path, const Y4mFormat &format, std::string &error)
{
  if (format.width <= 0 || format.height <= 0)
  {
    error = "No picture size";
    return false;
  }
  if (format.bitDepth != 8 && format.bitDepth != 10)
  {
    error = "Y4M output supports 8 and 10 bit only, not " + std::to_string(format.bitDepth);
    return false;
  }
  if (format.mono && format.bitDepth != 8)
  {
    // Cmono is 8 bit; there is no 10-bit mono tag that me::YuvLumaReader (or most readers) accept.
    error = "Y4M output for 4:0:0 is 8 bit only";
    return false;
  }
  if (!format.mono && (format.width % 2 || format.height % 2))
  {
    error = "4:2:0 needs an even picture size";
    return false;
  }
  this->file.open(path, std::ios::binary | std::ios::trunc);
  if (!this->file)
  {
    error = "Cannot write " + path;
    return false;
  }
  this->format = format;
  this->frames = 0;
  this->file << header(format) << '\n';
  return bool(this->file);
}

bool Y4mWriter::writeFrame(const std::uint8_t *y,
                           const std::uint8_t *u,
                           const std::uint8_t *v,
                           std::string        &error)
{
  this->file << "FRAME\n";
  this->file.write(reinterpret_cast<const char *>(y), this->format.lumaBytes());
  if (!this->format.mono)
  {
    this->file.write(reinterpret_cast<const char *>(u), this->format.chromaBytes());
    this->file.write(reinterpret_cast<const char *>(v), this->format.chromaBytes());
  }
  if (!this->file)
  {
    error = "Write failed after " + std::to_string(this->frames) + " frames (disk full?)";
    return false;
  }
  this->frames++;
  return true;
}

} // namespace bda::filter
