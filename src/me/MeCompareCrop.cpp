#include "MeCompareCrop.h"

#include <algorithm>
#include <cstdlib>

namespace bda::me
{
namespace
{

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::uint8_t clip8(int v) { return static_cast<std::uint8_t>(clampInt(v, 0, 255)); }

// Rounded down, as upstream lays the planes out (PixelFormatYUV::bytesPerFrame).
int chromaWidth(const YuvFrameLayout &l) { return std::max(1, l.width / l.subX); }
int chromaHeight(const YuvFrameLayout &l) { return std::max(1, l.height / l.subY); }

// One sample of a plane at native depth. Coordinates are already clamped to the plane.
unsigned sampleAt(const std::uint8_t *frame,
                  const YuvFrameLayout &l,
                  std::int64_t          planeOffset,
                  int                   planeWidth,
                  int                   x,
                  int                   y)
{
  const bool         twoBytes = l.bitDepth > 8;
  const std::int64_t index    = std::int64_t(y) * planeWidth + x;
  if (!twoBytes)
    return frame[planeOffset + index];
  const auto *p = frame + planeOffset + index * 2;
  return l.bigEndian ? (unsigned(p[0]) << 8 | p[1]) : (unsigned(p[1]) << 8 | p[0]);
}

// Luma at 8 bit with edge repetition.
int luma8(const std::uint8_t *frame, const YuvFrameLayout &l, int x, int y)
{
  const int shift = l.bitDepth > 8 ? l.bitDepth - 8 : 0;
  x               = clampInt(x, 0, l.width - 1);
  y               = clampInt(y, 0, l.height - 1);
  return int(sampleAt(frame, l, l.offset[0], l.width, x, y) >> shift);
}

// Cb and Cr at 8 bit for a luma position, nearest chroma sample, edge repetition.
void chroma8(const std::uint8_t *frame, const YuvFrameLayout &l, int x, int y, int &cb, int &cr)
{
  const int shift = l.bitDepth > 8 ? l.bitDepth - 8 : 0;
  const int cw = chromaWidth(l), ch = chromaHeight(l);
  const int cx = clampInt(clampInt(x, 0, l.width - 1) / l.subX, 0, cw - 1);
  const int cy = clampInt(clampInt(y, 0, l.height - 1) / l.subY, 0, ch - 1);
  if (l.interleavedUV)
  {
    // Cb and Cr alternate, so a row of the shared plane is twice the chroma width.
    const int first  = int(sampleAt(frame, l, l.offset[1], cw * 2, cx * 2, cy) >> shift);
    const int second = int(sampleAt(frame, l, l.offset[1], cw * 2, cx * 2 + 1, cy) >> shift);
    cb               = l.crFirst ? second : first;
    cr               = l.crFirst ? first : second;
    return;
  }
  cb = int(sampleAt(frame, l, l.offset[1], cw, cx, cy) >> shift);
  cr = int(sampleAt(frame, l, l.offset[2], cw, cx, cy) >> shift);
}

} // namespace

std::string checkLayout(const YuvFrameLayout &l, std::size_t frameBytes)
{
  if (l.width <= 0 || l.height <= 0)
    return "the frame has no size";
  if (l.bitDepth < 8 || l.bitDepth > 16)
    return "bit depth " + std::to_string(l.bitDepth) + " is not supported (8 to 16)";
  if (l.subX < 0 || l.subY < 0 || (l.subX == 0) != (l.subY == 0))
    return "the chroma subsampling is not supported";

  const std::int64_t bytes = l.bitDepth > 8 ? 2 : 1;
  std::int64_t       end   = l.offset[0] + std::int64_t(l.width) * l.height * bytes;
  if (l.subX > 0)
  {
    const std::int64_t plane = std::int64_t(chromaWidth(l)) * chromaHeight(l) * bytes;
    end = std::max(end, l.interleavedUV ? l.offset[1] + 2 * plane
                                        : std::max(l.offset[1], l.offset[2]) + plane);
  }
  if (std::int64_t(frameBytes) < end)
    return "the frame is shorter than its format needs (truncated, or not loaded yet)";
  return {};
}

RgbImage cropRgb(const std::uint8_t *frame, const YuvFrameLayout &l, const CropRect &rect)
{
  RgbImage image;
  if (rect.w <= 0 || rect.h <= 0)
    return image;
  image.width  = rect.w;
  image.height = rect.h;
  image.rgb.resize(std::size_t(rect.w) * rect.h * 3);

  auto *out = image.rgb.data();
  for (int j = 0; j < rect.h; ++j)
    for (int i = 0; i < rect.w; ++i)
    {
      const int x = rect.x + i, y = rect.y + j;
      const int c = luma8(frame, l, x, y) - 16;
      int       r = c * 298, g = c * 298, b = c * 298; // 255/219 in 8.8 fixed point
      if (l.subX > 0)
      {
        int cb{}, cr{};
        chroma8(frame, l, x, y, cb, cr);
        const int d = cb - 128, e = cr - 128;
        // BT.709 limited range, 8.8 fixed point.
        r += 459 * e;
        g -= 55 * d + 136 * e;
        b += 541 * d;
      }
      *out++ = clip8((r + 128) >> 8);
      *out++ = clip8((g + 128) >> 8);
      *out++ = clip8((b + 128) >> 8);
    }
  return image;
}

LumaDiff lumaDiff(const std::uint8_t   *source,
                  const std::uint8_t   *reference,
                  const YuvFrameLayout &l,
                  const CropRect       &rect,
                  int                   dx,
                  int                   dy,
                  int                   gain)
{
  LumaDiff diff;
  if (rect.w <= 0 || rect.h <= 0)
    return diff;
  diff.image.width  = rect.w;
  diff.image.height = rect.h;
  diff.image.rgb.resize(std::size_t(rect.w) * rect.h * 3);

  auto *out = diff.image.rgb.data();
  for (int j = 0; j < rect.h; ++j)
    for (int i = 0; i < rect.w; ++i)
    {
      const int x = rect.x + i, y = rect.y + j;
      const int d = luma8(source, l, x, y) - luma8(reference, l, x + dx, y + dy);
      diff.sad += std::abs(d);
      diff.maxAbs   = std::max(diff.maxAbs, std::abs(d));
      const auto v  = clip8(128 + gain * d);
      *out++        = v;
      *out++        = v;
      *out++        = v;
    }
  return diff;
}

} // namespace bda::me
