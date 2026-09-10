#include "assist/AssistContext.h"

#include <cmath>
#include <sstream>

namespace bda::assist
{

namespace
{

//!< Fixed precision, so the same measurement always renders the same string in tests and on screen.
std::string number(double value, int decimals)
{
  std::ostringstream out;
  out.precision(decimals);
  out << std::fixed << value;
  return out.str();
}

std::string integer(double value)
{
  std::ostringstream out;
  out << static_cast<long long>(value);
  return out.str();
}

/* One line of measurements for a region.
 *
 * The three ways a PSNR can be absent are written differently on purpose - "lossless", "no
 * original attached", and "not measured" mean different things to whoever reads the answer.
 */
std::string statsLine(const RegionStats &stats, bool hasOriginal)
{
  std::string text;
  text += "bits ";
  text += stats.bits ? integer(*stats.bits) : std::string("unknown");

  if (const auto psnr = stats.psnr())
    text += ", PSNR " + number(*psnr, 3) + " dB";
  else if (stats.lossless())
    text += ", lossless (SSE 0, PSNR undefined)";
  else if (!hasOriginal)
    text += ", PSNR unavailable (no original attached)";
  else
    text += ", PSNR not measured yet";

  return text;
}

} // namespace

std::optional<double> RegionStats::psnr() const
{
  if (!this->sse || !this->sampleCount || *this->sampleCount <= 0)
    return std::nullopt;
  if (*this->sse <= 0.0)
    return std::nullopt; // lossless: no finite PSNR. lossless() tells the two apart.

  const double peak = 255.0; // 8-bit. Deep streams are converted before the SSE is taken.
  return 10.0 * std::log10(peak * peak * static_cast<double>(*this->sampleCount) / *this->sse);
}

bool AssistContext::empty() const
{
  return !this->stream && !this->frameIdx && !this->frameStats && !this->superblock &&
         this->bdRate.empty();
}

std::string renderContext(const AssistContext &context)
{
  if (context.empty())
    return "## Context\n\nNothing is open in the analyzer.\n";

  const bool hasOriginal = context.stream && !context.stream->originalPath.empty();

  std::ostringstream out;
  out << "## Context\n";

  if (const auto &stream = context.stream)
  {
    out << "\n### Stream\n";
    out << "- file: " << stream->path << "\n";
    if (stream->width > 0 && stream->height > 0)
      out << "- resolution: " << stream->width << "x" << stream->height << "\n";
    /* A missing superblock size is not cosmetic: it means the sequence header has not been parsed,
     * which is exactly the condition BD-rate refuses on. Saying so lets the model explain that
     * refusal instead of treating the value as merely absent.
     */
    if (stream->superblockSize > 0)
      out << "- superblock: " << stream->superblockSize << "x" << stream->superblockSize << "\n";
    else
      out << "- superblock: unknown (sequence header not parsed yet)\n";
    if (!stream->codec.empty())
      out << "- codec: " << stream->codec << "\n";
    if (!stream->decoder.empty())
      out << "- decoder: " << stream->decoder << "\n";
    out << "- original: " << (hasOriginal ? stream->originalPath : "none attached") << "\n";
  }

  if (context.frameIdx)
    out << "\n### Frame\n- index: " << *context.frameIdx << "\n";

  if (const auto &stats = context.frameStats)
  {
    if (!context.frameIdx)
      out << "\n### Frame\n";
    out << "- whole frame: " << statsLine(*stats, hasOriginal) << "\n";
  }

  if (const auto &sb = context.superblock)
  {
    out << "\n### Clicked superblock\n";
    out << "- grid position: column " << sb->column << ", row " << sb->row << "\n";
    out << "- pixel position: (" << sb->pixelX << ", " << sb->pixelY << ")\n";
    out << "- " << statsLine(sb->stats, hasOriginal) << "\n";

    if (!sb->syntax.empty())
    {
      out << "\n#### Block syntax\n";
      for (const auto &entry : sb->syntax)
        out << "- " << entry.name << ": " << entry.value << "\n";
    }
  }

  if (!context.bdRate.empty())
  {
    out << "\n### BD-rate groups\n";
    for (const auto &[name, figure] : context.bdRate)
      out << "- " << name << ": " << figure << "\n";
  }

  return out.str();
}

} // namespace bda::assist
