#include "StabilizationWarp.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "MeFrameSource.h"

namespace bda::integration
{

namespace
{

bool layoutFor(const video::yuv::PixelFormatYUV &format, const Size &size, me::YuvFrameLayout &layout, QString *why)
{
  if (!yuvLayoutFromRawFormat(format, size, layout, why))
    return false;
  const bool mono = layout.subX == 0;
  if (layout.bigEndian || layout.interleavedUV || (!mono && (layout.subX != 2 || layout.subY != 2)) ||
      (layout.bitDepth != 8 && layout.bitDepth != 10) || layout.width % 2 != 0 || layout.height % 2 != 0)
  {
    if (why)
      *why = QString("Stabilisation takes planar 4:2:0 or 4:0:0, 8 or 10 bit little endian, even size. "
                     "This item's format is %1.")
                 .arg(QString::fromStdString(format.getName()));
    return false;
  }
  return true;
}

std::vector<std::uint16_t> readPlane(const std::uint8_t *data, std::size_t n, bool wide)
{
  std::vector<std::uint16_t> plane(n);
  if (wide)
    for (std::size_t i = 0; i < n; ++i)
      plane[i] = std::uint16_t(data[2 * i] | (data[2 * i + 1] << 8));
  else
    for (std::size_t i = 0; i < n; ++i)
      plane[i] = data[i];
  return plane;
}

void writePlane(std::uint8_t *data, const std::vector<std::uint16_t> &plane, bool wide)
{
  if (wide)
    for (std::size_t i = 0; i < plane.size(); ++i)
      data[2 * i] = std::uint8_t(plane[i] & 0xff), data[2 * i + 1] = std::uint8_t(plane[i] >> 8);
  else
    for (std::size_t i = 0; i < plane.size(); ++i)
      data[i] = std::uint8_t(plane[i]);
}

} // namespace

bool stabilizationSupported(const video::yuv::PixelFormatYUV &format, const Size &size, QString *why)
{
  me::YuvFrameLayout layout;
  return layoutFor(format, size, layout, why);
}

bool applyStabilizationWarp(QByteArray                        &raw,
                            const video::yuv::PixelFormatYUV &format,
                            const Size                       &size,
                            const StabilizationTable         &table,
                            int                               frameIndex)
{
  const auto it = table.warps.find(frameIndex);
  if (it == table.warps.end() || raw.isEmpty())
    return false;
  me::YuvFrameLayout layout;
  if (!layoutFor(format, size, layout, nullptr))
    return false;

  const bool        mono = layout.subX == 0;
  const bool        wide = layout.bitDepth > 8;
  const int         w = layout.width, h = layout.height;
  const std::size_t luma = std::size_t(w) * h, chroma = mono ? 0 : std::size_t(w / 2) * (h / 2);
  const std::size_t bytes = wide ? 2 : 1;
  const std::int64_t end =
      mono ? std::int64_t(luma * bytes) : std::max(layout.offset[1], layout.offset[2]) + std::int64_t(chroma * bytes);
  if (raw.size() < end)
    return false;

  auto *data = reinterpret_cast<std::uint8_t *>(raw.data());
  filter::Frame420u16 in;
  in.width  = w;
  in.height = h;
  in.y      = readPlane(data + layout.offset[0], luma, wide);
  if (!mono)
  {
    in.u = readPlane(data + layout.offset[1], chroma, wide);
    in.v = readPlane(data + layout.offset[2], chroma, wide);
  }
  // Centre siting, as Export Stabilized Y4M writes C420jpeg: the view shows exactly what it would write.
  const auto out = filter::warpFrame420(in, it->second, layout.bitDepth, filter::ChromaSiting::Centre);
  writePlane(data + layout.offset[0], out.y, wide);
  if (!mono)
  {
    writePlane(data + layout.offset[1], out.u, wide);
    writePlane(data + layout.offset[2], out.v, wide);
  }
  return true;
}

} // namespace bda::integration
