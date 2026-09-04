#include "MeFrameSource.h"

#include <statistics/PixelStatistics.h>

namespace bda::integration
{

me::MePlane lumaPlaneFromRawYuv(const QByteArray                 &rawYUV,
                                const video::yuv::PixelFormatYUV &format,
                                const Size                       &frameSize,
                                int                               pad)
{
  const auto reader = stats::makeLumaReader(rawYUV, format, frameSize);
  if (!reader)
    return {};

  const auto bitDepth = format.getBitsPerSample();
  const int  shift    = bitDepth > 8 ? static_cast<int>(bitDepth) - 8 : 0;

  me::MePlane plane(static_cast<int>(frameSize.width), static_cast<int>(frameSize.height), pad);
  if (plane.empty())
    return plane;

  for (unsigned y = 0; y < frameSize.height; ++y)
  {
    auto *out = plane.row(static_cast<int>(y));
    for (unsigned x = 0; x < frameSize.width; ++x)
      out[x] = static_cast<std::uint8_t>(reader->at(x, y) >> shift);
  }

  plane.padEdges();
  return plane;
}

} // namespace bda::integration
