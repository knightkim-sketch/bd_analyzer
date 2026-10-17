#include "AffineWarp.h"

#include <algorithm>
#include <cmath>

namespace bda::filter
{

AffineParams chromaWarpParams(const AffineParams &luma, ChromaSiting siting)
{
  double ox = 0, oy = 0;
  if (siting == ChromaSiting::Left)
    ox = -0.5;
  else if (siting == ChromaSiting::TopLeft)
    ox = -0.5, oy = -0.5;
  // 2 qc + o = A (2 pc + o) + b  =>  qc = A pc + (A o + b - o) / 2
  AffineParams c = luma;
  c[0]           = (luma[2] * ox + luma[3] * oy + luma[0] - ox) / 2;
  c[1]           = (luma[4] * ox + luma[5] * oy + luma[1] - oy) / 2;
  return c;
}

void warpPlane(const std::uint16_t        *in,
               int                         width,
               int                         height,
               const AffineParams         &p,
               int                         bitDepth,
               std::vector<std::uint16_t> &out)
{
  out.assign(std::size_t(std::max(0, width)) * std::max(0, height), 0);
  if (width <= 0 || height <= 0)
    return;
  const int  maxValue = (1 << bitDepth) - 1;
  const auto at       = [&](int x, int y) {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return double(in[std::size_t(y) * width + x]);
  };
  for (int j = 0; j < height; ++j)
  {
    const double py = j + 0.5;
    for (int i = 0; i < width; ++i)
    {
      const double px = i + 0.5;
      // Source position in index units (sample k at k + 0.5 -> index k).
      const double sx = p[0] + p[2] * px + p[3] * py - 0.5;
      const double sy = p[1] + p[4] * px + p[5] * py - 0.5;
      const double fx = std::floor(sx), fy = std::floor(sy);
      const double ax = sx - fx, ay = sy - fy;
      const int    x0 = int(fx), y0 = int(fy);
      const double top = at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax;
      const double bot = at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax;
      const double v   = top * (1 - ay) + bot * ay;
      out[std::size_t(j) * width + i] = std::uint16_t(std::clamp(long(std::lround(v)), 0L, long(maxValue)));
    }
  }
}

Frame420u16 warpFrame420(const Frame420u16 &in, const AffineParams &lumaParams, int bitDepth, ChromaSiting siting)
{
  Frame420u16 out;
  out.width  = in.width;
  out.height = in.height;
  warpPlane(in.y.data(), in.width, in.height, lumaParams, bitDepth, out.y);
  if (!in.u.empty())
  {
    const int  cw = in.width / 2, ch = in.height / 2;
    const auto cp = chromaWarpParams(lumaParams, siting);
    warpPlane(in.u.data(), cw, ch, cp, bitDepth, out.u);
    warpPlane(in.v.data(), cw, ch, cp, bitDepth, out.v);
  }
  return out;
}

} // namespace bda::filter
