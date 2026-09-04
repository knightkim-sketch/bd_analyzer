#include "SvtIntegerMe.h"

#include <algorithm>
#include <array>
#include <limits>

#include "MeCost.h"

namespace bda::me
{
namespace
{

constexpr int kB64 = 64;

/* SVT rounds every level's search width up to a multiple of 8 because the SIMD SAD kernels cost
 * the same for widths 1..8 (motion_estimation.c:803, :894, :987). Dropping the rounding narrows
 * the search, so it is reproduced even though our area sizing is already an approximation.
 */
int roundSearchWidthUp(int width)
{
  return (width + 7) & ~0x07;
}

struct BestPoint
{
  int          dx   = 0;
  int          dy   = 0;
  std::int64_t cost = std::numeric_limits<std::int64_t>::max();
};

/* One HME level: exhaustive full-pel search of `area` around `centre`, SAD only.
 *
 * SAD only is not a simplification - SVT's ME really does compare on raw SAD with no MV rate
 * anywhere in the chain (check_00_center at motion_estimation.c:1105-1118). The rate only enters
 * at mode decision.
 */
BestPoint searchLevel(const MePlane &src,
                      const MePlane &ref,
                      int            blkX,
                      int            blkY,
                      int            blkW,
                      int            blkH,
                      int            centreX,
                      int            centreY,
                      int            area)
{
  const int halfW = roundSearchWidthUp(2 * area) / 2;

  /* Corrected against the picture and its border, the way each HME level corrects its own search
   * origin. Without this the coarse levels read past the end of the plane on small pictures - the
   * border is only as wide as the pad.
   */
  const auto range = clampSearchRange(ref,
                                      blkX,
                                      blkY,
                                      blkW,
                                      blkH,
                                      centreX - halfW,
                                      centreX + halfW,
                                      centreY - area,
                                      centreY + area);

  BestPoint best;
  if (range.empty())
    return best;

  for (int dy = range.minDy; dy <= range.maxDy; ++dy)
    for (int dx = range.minDx; dx <= range.maxDx; ++dx)
    {
      const auto cost = blockSad(src, blkX, blkY, ref, blkX + dx, blkY + dy, blkW, blkH);
      // Strictly-less keeps the first candidate on a tie, which is what SVT's loops do.
      if (cost < best.cost)
        best = {dx, dy, cost};
    }
  return best;
}

/* me_static_b64_bypass(): a block whose zero-MV SAD is already tiny is not searched at all.
 *
 * SVT's threshold is derived from picture statistics we do not have here, so this uses a fixed
 * mean-absolute-difference threshold instead. Reported through MeBlockResult::staticBypass so a
 * bypassed block is visibly "decided without searching" rather than looking like a search that
 * happened to return zero.
 */
constexpr std::int64_t kStaticBypassMadPerPixel = 2;

bool isStaticBlock(const MePlane &src, const MePlane &ref, int x, int y, int w, int h)
{
  const auto sad = blockSad(src, x, y, ref, x, y, w, h);
  return sad <= kStaticBypassMadPerPixel * static_cast<std::int64_t>(w) * h;
}

} // namespace

bool SvtIntegerMe::canRun(const MePictureSet &pictures, std::string *reason) const
{
  if (pictures.current.empty() || pictures.reference.empty())
  {
    if (reason)
      *reason = "no current or reference picture";
    return false;
  }
  if (pictures.current.width() != pictures.reference.width() ||
      pictures.current.height() != pictures.reference.height())
  {
    if (reason)
      *reason = "current and reference pictures differ in size";
    return false;
  }
  return true;
}

MeFrameResult SvtIntegerMe::estimateFrame(const MePictureSet &pictures, const MeParams &params)
{
  MeFrameResult result;
  result.frameIdx    = pictures.frameIdx;
  result.refFrameIdx = pictures.refFrameIdx;
  result.algorithm   = Algorithm::SvtIntegerMe;

  if (std::string why; !this->canRun(pictures, &why))
  {
    result.error = why;
    return result;
  }

  const auto &src = pictures.current;
  const auto &ref = pictures.reference;

  /* The HME pyramid, built the way SVT builds it: two cascaded step 2 downscales rather than one
   * step 4. buildSvtPyramid() carries the reasoning; getting this wrong changes the picture that
   * level 0 searches.
   */
  const auto srcPyr = buildSvtPyramid(src, kHmePyramidPad, kHmePyramidPad);
  const auto refPyr = buildSvtPyramid(ref, kHmePyramidPad, kHmePyramidPad);

  /* Scale the search areas with the frame interval. SVT does this through
   * svt_aom_get_scaled_picture_distance(); a reference further away needs a wider search because
   * the same motion covers more ground. The sign does not matter here - distance does - but the
   * sign is what decided which picture became the reference in the first place.
   */
  const int  distance = std::max(1, std::abs(params.frameInterval));
  const auto scale    = [distance](int area) { return std::min(area * distance, area * 4); };

  const int blocksX = (src.width() + kB64 - 1) / kB64;
  const int blocksY = (src.height() + kB64 - 1) / kB64;

  for (int by = 0; by < blocksY; ++by)
    for (int bx = 0; bx < blocksX; ++bx)
    {
      /* Between superblocks is the right granularity: fine enough that a frame change feels
       * immediate, coarse enough that the check is invisible next to a superblock's worth of SAD.
       */
      if (params.cancel != nullptr && params.cancel->cancelled())
      {
        result.cancelled = true;
        return result;
      }

      const int b64X = bx * kB64;
      const int b64Y = by * kB64;

      if (params.staticBypass && isStaticBlock(src, ref, b64X, b64Y, kB64, kB64))
      {
        for (int s = 0; s < kBlockSizeCount; ++s)
        {
          const auto size = static_cast<BlockSize>(s);
          if (!params.blockSizes.contains(size))
            continue;
          const int px = blockSizeInPixels(size);
          for (int y = b64Y; y < b64Y + kB64; y += px)
            for (int x = b64X; x < b64X + kB64; x += px)
            {
              MeBlockResult r;
              r.block        = {x, y, size};
              r.mv           = {};
              r.staticBypass = true;
              r.nativeCost   = blockSad(src, x, y, ref, x, y, px, px);
              r.commonSad    = r.nativeCost;
              result.blocks.push_back(r);
            }
        }
        continue;
      }

      int centreX = 0;
      int centreY = 0;

      if (!params.forceZeroCentre)
      {
        // Level 0, sixteenth resolution. Coordinates and vectors are in that resolution.
        const auto l0 = searchLevel(srcPyr.sixteenth,
                                    refPyr.sixteenth,
                                    b64X / 4,
                                    b64Y / 4,
                                    kB64 / 4,
                                    kB64 / 4,
                                    0,
                                    0,
                                    scale(kHmeL0Area));

        // Level 1, quarter resolution: the level 0 vector is worth twice as much here.
        const auto l1 = searchLevel(srcPyr.quarter,
                                    refPyr.quarter,
                                    b64X / 2,
                                    b64Y / 2,
                                    kB64 / 2,
                                    kB64 / 2,
                                    l0.dx * 2,
                                    l0.dy * 2,
                                    kHmeL1Area);

        // Level 2, full resolution.
        const auto l2 = searchLevel(
            src, ref, b64X, b64Y, kB64, kB64, l1.dx * 2, l1.dy * 2, kHmeL2Area);

        /* check_00_center(): the zero vector competes with whatever HME found, and wins ties.
         * This is what pulls static areas back to (0,0) instead of leaving them on a slightly
         * cheaper but arbitrary vector.
         */
        const auto zeroCost = blockSad(src, b64X, b64Y, ref, b64X, b64Y, kB64, kB64);
        if (zeroCost <= l2.cost)
        {
          centreX = 0;
          centreY = 0;
        }
        else
        {
          centreX = l2.dx;
          centreY = l2.dy;
        }
      }

      /* integer_search_b64(): one pass over the search area accumulating the 8x8 SADs, then the
       * larger sizes by summation - svt_ext_sad_calculation_8x8_16x16_c and
       * svt_ext_sad_calculation_32x32_64x64_c do exactly this. One search serves every block size,
       * which is why the block size mask filters the report rather than the work.
       */
      constexpr int kUnits = kB64 / 8; // 8 x 8 grid of 8x8 blocks
      struct Best
      {
        std::int64_t cost = std::numeric_limits<std::int64_t>::max();
        int          dx   = 0;
        int          dy   = 0;
      };
      std::array<Best, kUnits * kUnits> best8;
      std::array<Best, 16>              best16;
      std::array<Best, 4>               best32;
      Best                              best64;

      const int  sr    = roundSearchWidthUp(2 * kIntegerSearchSr) / 2;
      const auto range = clampSearchRange(ref,
                                          b64X,
                                          b64Y,
                                          kB64,
                                          kB64,
                                          centreX - sr,
                                          centreX + sr,
                                          centreY - kIntegerSearchSr,
                                          centreY + kIntegerSearchSr);
      for (int dy = range.minDy; dy <= range.maxDy; ++dy)
        for (int dx = range.minDx; dx <= range.maxDx; ++dx)
        {
          std::array<std::int64_t, kUnits * kUnits> sad8{};
          for (int uy = 0; uy < kUnits; ++uy)
            for (int ux = 0; ux < kUnits; ++ux)
            {
              const int x = b64X + ux * 8;
              const int y = b64Y + uy * 8;
              const auto v = blockSad(src, x, y, ref, x + dx, y + dy, 8, 8);
              sad8[uy * kUnits + ux] = v;
              auto &b                = best8[uy * kUnits + ux];
              if (v < b.cost)
                b = {v, dx, dy};
            }

          // 16x16 from four 8x8, 32x32 from four 16x16, 64x64 from four 32x32 - the same
          // summation ladder svt_ext_sad_calculation_* walks, so one candidate serves every size.
          std::array<std::int64_t, 16> sad16{};
          for (int qy = 0; qy < 4; ++qy)
            for (int qx = 0; qx < 4; ++qx)
            {
              std::int64_t v = 0;
              for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x)
                  v += sad8[(qy * 2 + y) * kUnits + (qx * 2 + x)];
              sad16[qy * 4 + qx] = v;
              auto &b            = best16[qy * 4 + qx];
              if (v < b.cost)
                b = {v, dx, dy};
            }

          std::int64_t total64 = 0;
          for (int hy = 0; hy < 2; ++hy)
            for (int hx = 0; hx < 2; ++hx)
            {
              std::int64_t v = 0;
              for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x)
                  v += sad16[(hy * 2 + y) * 4 + (hx * 2 + x)];
              auto &b = best32[hy * 2 + hx];
              if (v < b.cost)
                b = {v, dx, dy};
              total64 += v;
            }

          if (total64 < best64.cost)
            best64 = {total64, dx, dy};
        }

      const auto emit = [&](BlockSize size, int x, int y, const Best &b) {
        if (!params.blockSizes.contains(size))
          return;
        MeBlockResult r;
        r.block      = {x, y, size};
        r.mv         = MotionVector::fromFullPel(b.dx, b.dy);
        r.nativeCost = b.cost;
        r.commonSad  = commonMetricSad(src, ref, r.block, r.mv);
        result.blocks.push_back(r);
      };

      for (int uy = 0; uy < kUnits; ++uy)
        for (int ux = 0; ux < kUnits; ++ux)
          emit(BlockSize::Blk8, b64X + ux * 8, b64Y + uy * 8, best8[uy * kUnits + ux]);
      for (int qy = 0; qy < 4; ++qy)
        for (int qx = 0; qx < 4; ++qx)
          emit(BlockSize::Blk16, b64X + qx * 16, b64Y + qy * 16, best16[qy * 4 + qx]);
      for (int hy = 0; hy < 2; ++hy)
        for (int hx = 0; hx < 2; ++hx)
          emit(BlockSize::Blk32, b64X + hx * 32, b64Y + hy * 32, best32[hy * 2 + hx]);
      emit(BlockSize::Blk64, b64X, b64Y, best64);
    }

  return result;
}

} // namespace bda::me
