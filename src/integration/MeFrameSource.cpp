#include "MeFrameSource.h"

#include <statistics/PixelStatistics.h>

#include <utility>

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

bool yuvLayoutFromRawFormat(const video::yuv::PixelFormatYUV &format,
                            const Size                       &frameSize,
                            me::YuvFrameLayout               &layout,
                            QString                          *whyNot)
{
  const auto refuse = [whyNot](const QString &reason) {
    if (whyNot)
      *whyNot = reason;
    return false;
  };
  if (!format.isValid() || !frameSize.isValid())
    return refuse("The item has no valid YUV format or size.");
  if (!format.isPlanar() || format.isBytePacking())
    return refuse("Packed YUV formats are not supported by the compare view.");
  const auto sub = format.getSubsampling();
  if (sub == video::yuv::Subsampling::UNKNOWN)
    return refuse("The chroma subsampling of this format is unknown.");

  layout           = {};
  layout.width     = static_cast<int>(frameSize.width);
  layout.height    = static_cast<int>(frameSize.height);
  layout.bitDepth  = static_cast<int>(format.getBitsPerSample());
  layout.bigEndian = format.isBigEndian();

  const std::int64_t bytes = layout.bitDepth > 8 ? 2 : 1;
  const std::int64_t luma  = std::int64_t(layout.width) * layout.height * bytes;
  layout.offset[0]         = 0;
  if (sub == video::yuv::Subsampling::YUV_400)
  {
    layout.subX = layout.subY = 0;
    return true;
  }
  layout.subX = format.getSubsamplingHor();
  layout.subY = format.getSubsamplingVer();
  const std::int64_t chroma =
      std::int64_t(layout.width / layout.subX) * (layout.height / layout.subY) * bytes;

  layout.interleavedUV = format.isUVInterleaved();
  layout.offset[1]     = luma;
  layout.offset[2]     = luma + chroma;
  const auto order     = format.getPlaneOrder();
  const bool vFirst =
      order == video::yuv::PlaneOrder::YVU || order == video::yuv::PlaneOrder::YVUA;
  if (layout.interleavedUV)
    layout.crFirst = vFirst;
  else if (vFirst)
    std::swap(layout.offset[1], layout.offset[2]);
  return true;
}

} // namespace bda::integration
