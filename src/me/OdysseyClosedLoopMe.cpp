#include "OdysseyClosedLoopMe.h"

#include "MeCost.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <tuple>

namespace bda::me
{

namespace
{

constexpr int kSb     = 64;
constexpr int kPad    = 64; // NUM_PAD_PIXELS in clamp_search_range()
constexpr int kRound0 = 3;  // InterRound0
constexpr int kRound1 = 11; // InterRound1, single reference

// encoder/av1/inter.h: av1_sub_pel_filters_8 (regular) and av1_sub_pel_filters_4 (4-tap regular).
constexpr int kRegular8[16][8] = {
    {0, 0, 0, 128, 0, 0, 0, 0},     {0, 2, -6, 126, 8, -2, 0, 0},   {0, 2, -10, 122, 18, -4, 0, 0},
    {0, 2, -12, 116, 28, -8, 2, 0}, {0, 2, -14, 110, 38, -10, 2, 0}, {0, 2, -14, 102, 48, -12, 2, 0},
    {0, 2, -16, 94, 58, -12, 2, 0}, {0, 2, -14, 84, 66, -12, 2, 0}, {0, 2, -14, 76, 76, -14, 2, 0},
    {0, 2, -12, 66, 84, -14, 2, 0}, {0, 2, -12, 58, 94, -16, 2, 0}, {0, 2, -12, 48, 102, -14, 2, 0},
    {0, 2, -10, 38, 110, -14, 2, 0}, {0, 2, -8, 28, 116, -12, 2, 0}, {0, 0, -4, 18, 122, -10, 2, 0},
    {0, 0, -2, 8, 126, -6, 2, 0}};
constexpr int kRegular4[16][8] = {
    {0, 0, 0, 128, 0, 0, 0, 0},    {0, 0, -4, 126, 8, -2, 0, 0},   {0, 0, -8, 122, 18, -4, 0, 0},
    {0, 0, -10, 116, 28, -6, 0, 0}, {0, 0, -12, 110, 38, -8, 0, 0}, {0, 0, -12, 102, 48, -10, 0, 0},
    {0, 0, -14, 94, 58, -10, 0, 0}, {0, 0, -12, 84, 66, -10, 0, 0}, {0, 0, -12, 76, 76, -12, 0, 0},
    {0, 0, -10, 66, 84, -12, 0, 0}, {0, 0, -10, 58, 94, -14, 0, 0}, {0, 0, -10, 48, 102, -12, 0, 0},
    {0, 0, -8, 38, 110, -12, 0, 0}, {0, 0, -6, 28, 116, -10, 0, 0}, {0, 0, -4, 18, 122, -8, 0, 0},
    {0, 0, -2, 8, 126, -4, 0, 0}};

// ROUND_POWER_OF_TWO (ody_base.h:48): an arithmetic shift, so negative sums round towards -inf + 1/2.
int round2(int value, int n) { return (value + ((1 << n) >> 1)) >> n; }

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::uint8_t pixelClamped(const MePlane &p, int x, int y)
{
  return p.at(clampInt(x, 0, p.width() - 1), clampInt(y, 0, p.height() - 1));
}

BlockSize sizeOf(int size)
{
  switch (size)
  {
  case 8:
    return BlockSize::Blk8;
  case 16:
    return BlockSize::Blk16;
  case 32:
    return BlockSize::Blk32;
  default:
    return BlockSize::Blk64;
  }
}

// me_update_blk_best() (me.c:572): a tie with the best goes to the second best.
struct Best
{
  std::int64_t cost = std::numeric_limits<std::int32_t>::max();
  std::int64_t cost2 = std::numeric_limits<std::int32_t>::max();
  MotionVector mv, mv2;

  void update(std::int64_t c, MotionVector v)
  {
    if (c < this->cost)
    {
      this->cost2 = this->cost, this->mv2 = this->mv;
      this->cost = c, this->mv = v;
    }
    else if (c < this->cost2)
      this->cost2 = c, this->mv2 = v;
  }
};

/* search_pts_full (quarter-pel) and epel_search_pts_lut (eighth-pel): 8 x 8 points, y outer. The
 * eighth-pel window leans towards the second best: [-3, 4] on that axis when it lies on the
 * positive side, [-4, 3] otherwise.
 */
MotionVector subpelPoint(int i, bool posX, bool posY)
{
  const int ix = i % 8, iy = i / 8;
  return {ix + (posX ? -3 : -4), iy + (posY ? -3 : -4)};
}

} // namespace

void odysseyPredictBlock(const MePlane &ref, int x, int y, int size, MotionVector mv, bool regular8Tap,
                         std::uint8_t *out)
{
  // mv_16pel = mv << 1; ipel = mv_16pel >> 4; spel = mv_16pel & 15 (predict_inter_block_plane).
  const int mx = mv.x * 2, my = mv.y * 2;
  const int ix = mx >> 4, iy = my >> 4, sx = mx & 15, sy = my & 15;
  const auto &kh = regular8Tap ? kRegular8[sx] : kRegular4[sx];
  const auto &kv = regular8Tap ? kRegular8[sy] : kRegular4[sy];
  const int x0 = x + ix, y0 = y + iy;

  // Horizontal pass into rows y0 - 3 .. y0 + size + 4 (only the middle `size` rows without sy).
  const int rows = size + 7;
  int       tmp[(kSb + 7) * kSb];
  for (int r = 0; r < rows; ++r)
  {
    const int ry = y0 - 3 + r;
    for (int c = 0; c < size; ++c)
    {
      const int cx = x0 + c;
      int       h;
      if (sx == 0)
        h = pixelClamped(ref, cx, ry) << (7 - kRound0);
      else
      {
        int sum = 0;
        for (int t = 0; t < 8; ++t)
          sum += pixelClamped(ref, cx - 3 + t, ry) * kh[t];
        h = round2(sum, kRound0);
      }
      tmp[r * size + c] = h;
    }
  }
  for (int r = 0; r < size; ++r)
    for (int c = 0; c < size; ++c)
    {
      int v;
      if (sy == 0)
        v = round2(tmp[(r + 3) * size + c] << 7, kRound1);
      else
      {
        int sum = 0;
        for (int t = 0; t < 8; ++t)
          sum += tmp[(r + t) * size + c] * kv[t];
        v = round2(sum, kRound1);
      }
      out[r * size + c] = std::uint8_t(clampInt(v, 0, 255));
    }
}

std::int64_t odysseyClosedLoopSad(const MePlane &src, const MePlane &ref, int x, int y, int size,
                                  MotionVector mv, bool regular8Tap)
{
  std::uint8_t pred[kSb * kSb];
  odysseyPredictBlock(ref, x, y, size, mv, regular8Tap, pred);
  std::int64_t sad = 0;
  for (int r = 0; r < size; ++r)
    for (int c = 0; c < size; ++c)
      sad += std::abs(int(pixelClamped(src, x + c, y + r)) - int(pred[r * size + c]));
  return sad;
}

OdyClBlock odysseyClosedLoopSearchBlock(const MePlane &src, const MePlane &ref, int x, int y, int size,
                                        MotionVector centreMv, const OdyClOptions &options)
{
  OdyClBlock out;
  // center = (mv_8th << 1) >> SUBPEL_BITS: floor of the full-pel part.
  out.centreX = (centreMv.x * 2) >> 4;
  out.centreY = (centreMv.y * 2) >> 4;

  // clamp_search_range(), mcts off.
  const int minW = -(options.searchWidth / 2), maxW = options.searchWidth + minW - 1;
  const int minH = -(options.searchHeight / 2), maxH = options.searchHeight + minH - 1;
  const int loX = -(x + kPad), hiX = (src.width() - x) - size + kPad - 1;
  const int loY = -(y + kPad), hiY = (src.height() - y) - size + kPad - 1;
  out.minMvdX = clampInt(minW + out.centreX, loX, hiX) - out.centreX;
  out.maxMvdX = clampInt(maxW + out.centreX, loX, hiX) - out.centreX;
  out.minMvdY = clampInt(minH + out.centreY, loY, hiY) - out.centreY;
  out.maxMvdY = clampInt(maxH + out.centreY, loY, hiY) - out.centreY;

  const bool hp       = options.allowHighPrecisionMv;
  const auto blkSize  = sizeOf(size);
  Best       best;
  for (int dx = out.minMvdX; dx <= out.maxMvdX; ++dx)
    for (int dy = out.minMvdY; dy <= out.maxMvdY; ++dy)
    {
      const MotionVector v{out.centreX + dx, out.centreY + dy};
      const auto cost = odysseyClosedLoopSad(src, ref, x, y, size, MotionVector::fromFullPel(v.x, v.y), hp) +
                        odysseyClosedLoopRateTerm(dx, dy, blkSize);
      best.update(cost, v);
    }
  out.intBest   = best.mv;
  out.intSecond = best.mv2;
  out.intCost   = best.cost;

  // closed_loop_subpel_search(): eighth-pel only when the two best are neighbours.
  const int  ddx  = best.mv.x - best.mv2.x, ddy = best.mv.y - best.mv2.y;
  out.eighthPel   = hp && std::abs(ddx) <= 1 && std::abs(ddy) <= 1;
  const int scale = out.eighthPel ? 1 : 2;
  const bool posX = out.eighthPel && ddx < 0, posY = out.eighthPel && ddy < 0;

  const MotionVector centre = MotionVector::fromFullPel(best.mv.x, best.mv.y);
  MotionVector       bestMv = centre;
  std::int64_t       minSad = best.cost;
  for (int i = 0; i < 64; ++i)
  {
    const auto         p = subpelPoint(i, posX, posY);
    const MotionVector v{centre.x + p.x * scale, centre.y + p.y * scale};
    const auto         sad = odysseyClosedLoopSad(src, ref, x, y, size, v, hp);
    if (sad < minSad)
      minSad = sad, bestMv = v;
  }
  out.mv  = bestMv;
  out.sad = minSad;
  return out;
}

bool OdysseyClosedLoopMe::canRun(const MePictureSet &pictures, std::string *reason) const
{
  if (pictures.current.empty() || pictures.reference.empty())
  {
    if (reason)
      *reason = "missing current or reference picture";
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

MeFrameResult OdysseyClosedLoopMe::estimateFrame(const MePictureSet &pictures, const MeParams &params)
{
  MeFrameResult result;
  result.frameIdx    = pictures.frameIdx;
  result.refFrameIdx = pictures.refFrameIdx;
  result.algorithm   = Algorithm::OdysseyClosedLoop;
  if (std::string why; !this->canRun(pictures, &why))
  {
    result.error = why;
    return result;
  }

  // The seed's vectors by (size, x, y): each block starts from its own co-located block.
  std::map<std::tuple<int, int, int>, MotionVector> seed;
  if (params.seed != nullptr)
    for (const auto &b : params.seed->blocks)
      seed[{int(b.block.size), b.block.x, b.block.y}] = b.mv;

  OdyClOptions options;
  options.allowHighPrecisionMv = params.allowHighPrecisionMv;

  const int sbCols = (pictures.current.width() + kSb - 1) / kSb;
  const int sbRows = (pictures.current.height() + kSb - 1) / kSb;
  if (params.progress != nullptr)
    params.progress->begin(sbCols * sbRows);

  for (int sbY = 0; sbY < sbRows; ++sbY)
    for (int sbX = 0; sbX < sbCols; ++sbX)
    {
      if (params.cancel != nullptr && params.cancel->cancelled())
      {
        result.cancelled = true;
        return result;
      }
      if (params.progress != nullptr)
        params.progress->advance();

      for (const int size : {8, 16, 32, 64})
      {
        const auto bs = sizeOf(size);
        if (!params.blockSizes.contains(bs))
          continue;
        for (int y = sbY * kSb; y < (sbY + 1) * kSb; y += size)
          for (int x = sbX * kSb; x < (sbX + 1) * kSb; x += size)
          {
            const auto it     = seed.find({int(bs), x, y});
            const auto centre = it == seed.end() ? MotionVector{} : it->second;
            const auto b      = odysseyClosedLoopSearchBlock(
                pictures.current, pictures.reference, x, y, size, centre, options);
            MeBlockResult r;
            r.block      = {x, y, bs};
            r.mv         = b.mv;
            r.nativeCost = b.sad;
            r.commonSad  = commonMetricSad(pictures.current, pictures.reference, r.block, r.mv);
            result.blocks.push_back(r);
          }
      }
    }
  return result;
}

} // namespace bda::me
