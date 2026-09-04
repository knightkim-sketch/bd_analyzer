#include "MeCost.h"

#include <cstdlib>

#include "OdysseyMvRateLut.h"

namespace bda::me
{
namespace
{

// divide_round() from odyssey: round-half-up on the magnitude, sign preserved.
int divideRound(int value, int divisor)
{
  if (divisor == 0)
    return 0;
  const int abs = value < 0 ? -value : value;
  const int q   = (abs + divisor / 2) / divisor;
  return value < 0 ? -q : q;
}

int rateFromLut(std::int32_t magnitude)
{
  // ods_me_get_mvcost(): clamp first, then pick the table by magnitude.
  const std::int32_t m = magnitude < 1919 ? magnitude : 1919;
  if (m < 128)
    return kOdysseyMvdRateLutLv1[m];
  if (m < 384)
    return kOdysseyMvdRateLutLv2[(m - 128) >> 1];
  if (m < 896)
    return kOdysseyMvdRateLutLv3[(m - 384) >> 2];
  return kOdysseyMvdRateLutLv4[(m - 896) >> 3];
}

} // namespace

std::int64_t blockSad(const MePlane &src,
                      int             srcX,
                      int             srcY,
                      const MePlane  &ref,
                      int             refX,
                      int             refY,
                      int             width,
                      int             height,
                      int             rowStep)
{
  if (rowStep < 1)
    rowStep = 1;

  std::int64_t sum = 0;
  for (int y = 0; y < height; y += rowStep)
  {
    const auto *s = &src.at(srcX, srcY + y);
    const auto *r = &ref.at(refX, refY + y);
    for (int x = 0; x < width; ++x)
      sum += std::abs(static_cast<int>(s[x]) - static_cast<int>(r[x]));
  }
  return sum;
}

std::int64_t blockSse(const MePlane &src,
                      int             srcX,
                      int             srcY,
                      const MePlane  &ref,
                      int             refX,
                      int             refY,
                      int             width,
                      int             height,
                      int             rowStep)
{
  if (rowStep < 1)
    rowStep = 1;

  std::int64_t sum = 0;
  for (int y = 0; y < height; y += rowStep)
  {
    const auto *s = &src.at(srcX, srcY + y);
    const auto *r = &ref.at(refX, refY + y);
    for (int x = 0; x < width; ++x)
    {
      const std::int64_t d = static_cast<int>(s[x]) - static_cast<int>(r[x]);
      sum += d * d;
    }
  }
  return sum;
}

int odysseyMvCost(std::int32_t mvdX, std::int32_t mvdY)
{
  const std::int32_t x = mvdX < 0 ? -mvdX : mvdX;
  const std::int32_t y = mvdY < 0 ? -mvdY : mvdY;
  return rateFromLut(x) + rateFromLut(y);
}

std::int64_t odysseyOpenLoopRateTerm(int mvdX, int mvdY, int mvRateWeight)
{
  /* ods_me_full_search_area():
   *   scaled_rate = mv_rate_weight * ods_me_get_mvcost(mvd);
   *   scaled_rate = divide_round(scaled_rate, 6);      // to the 8x8 basis
   *   cost       += (scaled_rate * scaled_rate) << 6;  // 8x8 -> 64x64
   *
   * The rate is squared because the distortion here is SSE, not SAD - adding a linear rate to a
   * squared distortion would weight it differently at every QP. Note odyssey divides by the
   * literal 6, not by 64; that is its code, kept as is.
   */
  const std::int64_t scaled = divideRound(mvRateWeight * odysseyMvCost(mvdX, mvdY), 6);
  return (scaled * scaled) << 6;
}

std::int64_t odysseyVbsRateTerm(int mvdFromCentreX, int mvdFromCentreY)
{
  /* ods_me_full_search_vbs(), openloop_me.c:871-881:
   *   mvd    = the offset from the search centre, not from the predictor
   *   mrate  = divide_round(ods_me_get_mvcost(mvd), 8)
   *   cost  += mrate * mrate                       // per 8x8, no << 6
   */
  const std::int64_t mrate = divideRound(odysseyMvCost(mvdFromCentreX, mvdFromCentreY), 8);
  return mrate * mrate;
}

std::int64_t odysseyClosedLoopRateTerm(int mvdX, int mvdY, BlockSize size)
{
  const std::int64_t rate = odysseyMvCost(mvdX, mvdY);
  switch (size)
  {
  case BlockSize::Blk8:
    return (rate + 32) >> 6;
  case BlockSize::Blk16:
    return (rate + 8) >> 4;
  case BlockSize::Blk32:
    return (rate + 2) >> 2;
  case BlockSize::Blk64:
    return rate;
  }
  return rate;
}

std::int64_t commonMetricSad(const MePlane      &src,
                             const MePlane      &ref,
                             const MeBlock      &block,
                             const MotionVector &mvEighthPel)
{
  const int size = blockSizeInPixels(block.size);

  /* Whole-pixel part only. Arithmetic shift would floor negatives towards minus infinity, which
   * would bias every negative vector by a pixel, so divide towards zero explicitly.
   */
  const int dx = mvEighthPel.x / 8;
  const int dy = mvEighthPel.y / 8;

  return blockSad(src, block.x, block.y, ref, block.x + dx, block.y + dy, size, size, 1);
}

} // namespace bda::me
