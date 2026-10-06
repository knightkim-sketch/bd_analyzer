#include "ReconDiff.h"

#include <cmath>

namespace bda::diff
{

std::size_t FrameLayout::bytesPerFrame() const
{
  std::size_t bytes = 0;
  for (const auto &p : this->planes)
    bytes += static_cast<std::size_t>(p.width) * static_cast<std::size_t>(p.height);
  return bytes * static_cast<std::size_t>(this->bytesPerSample());
}

bool FrameDiff::identical() const
{
  for (const auto &p : this->planes)
    if (!p.identical())
      return false;
  return true;
}

namespace
{

template <int BytesPerSample>
PlaneDiff comparePlane(const std::uint8_t *a,
                       const std::uint8_t *b,
                       const int           width,
                       const int           height,
                       const bool          bigEndian)
{
  PlaneDiff out;
  out.samples = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);

  const auto read = [bigEndian](const std::uint8_t *p) -> int {
    if constexpr (BytesPerSample == 1)
      return p[0];
    else
      return bigEndian ? (p[0] << 8) | p[1] : p[0] | (p[1] << 8);
  };

  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
    {
      const auto offset = (static_cast<std::size_t>(y) * width + x) * BytesPerSample;
      const auto d      = read(a + offset) - read(b + offset);
      if (d == 0)
        continue;
      out.sse += static_cast<std::uint64_t>(d * d);
      if (out.differingSamples++ == 0)
      {
        out.firstX = x;
        out.firstY = y;
      }
    }
  return out;
}

} // namespace

FrameDiff compareFrames(const std::uint8_t *a, const std::uint8_t *b, const FrameLayout &layout)
{
  FrameDiff   out;
  std::size_t offset = 0;
  for (const auto &plane : layout.planes)
  {
    if (layout.bytesPerSample() == 1)
      out.planes.push_back(
          comparePlane<1>(a + offset, b + offset, plane.width, plane.height, layout.bigEndian));
    else
      out.planes.push_back(
          comparePlane<2>(a + offset, b + offset, plane.width, plane.height, layout.bigEndian));
    offset += static_cast<std::size_t>(plane.width) * static_cast<std::size_t>(plane.height) *
              static_cast<std::size_t>(layout.bytesPerSample());
  }
  return out;
}

std::optional<double> psnr(const PlaneDiff &plane, const unsigned bitDepth)
{
  if (plane.sse == 0 || plane.samples == 0)
    return std::nullopt;
  const double peak = double((1u << bitDepth) - 1);
  const double mse  = double(plane.sse) / double(plane.samples);
  return 10.0 * std::log10(peak * peak / mse);
}

} // namespace bda::diff
