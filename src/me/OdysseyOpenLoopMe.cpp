#include "OdysseyOpenLoopMe.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "MeCost.h"

namespace bda::me
{
namespace
{

constexpr int kSb = 64;

MotionVector medianOf3(const MotionVector &a, const MotionVector &b, const MotionVector &c)
{
  // ods_mv_median(): component-wise median of the three predictors.
  const auto med = [](int p, int q, int r) { return std::max(std::min(p, q), std::min(std::max(p, q), r)); };
  return {med(a.x, b.x, c.x), med(a.y, b.y, c.y)};
}

/* get_median_mv()'s selection, in the shape the source uses.
 *
 * The rules are not "median of whatever is available": with fewer than two neighbours it picks a
 * single one, and an unavailable A is substituted by D before anything else. Reproducing that
 * ordering matters, because it decides the predictor in exactly the frame-edge cases where the
 * search area is also clipped.
 */
MotionVector selectPredictor(const MotionVector &a,
                             const MotionVector &b,
                             const MotionVector &c,
                             const MotionVector &d,
                             bool                availA,
                             bool                availB,
                             bool                availC,
                             bool                availD)
{
  MotionVector mvA = a;
  bool         hasA = availA;
  if (!hasA)
  {
    hasA = availD;
    mvA  = d;
  }

  if (!availB && !availC)
    return hasA ? mvA : MotionVector{};
  if (hasA && !availB && !availC)
    return mvA;
  if (availB && !hasA && !availC)
    return b;
  if (availC && !hasA && !availB)
    return c;
  return medianOf3(mvA, b, c);
}

/* Position k (0..63) of an 8x8 candidate grid: raster, or the hardware's bit-interleaved walk
 * (bits from the top: y_h0 x_h0 y_h1 x_h1 y_h2 x_h2).
 */
std::pair<int, int> gridPosition(int k, bool hwOrder)
{
  if (!hwOrder)
    return {k % 8, k / 8};
  const int ix = ((k >> 4) & 1) << 2 | ((k >> 2) & 1) << 1 | (k & 1);
  const int iy = ((k >> 5) & 1) << 2 | ((k >> 3) & 1) << 1 | ((k >> 1) & 1);
  return {ix, iy};
}

/* odyssey's divide_round(value, bits) = (value + (1 << (bits - 1))) >> bits - its second argument is
 * a shift, not a divisor (common/include/ody_base.h:34). OdysseyOpenLoopMe's rate terms divide by
 * the literal instead (divideRound in MeCost); odysseyHw uses this one.
 */
int divideRoundBits(int value, int bits) { return (value + (1 << (bits - 1))) >> bits; }

// SIGNED_SHIFT_RIGHT: divide_round(|v|, s) with the sign put back.
int signedShiftRight(int v, int s)
{
  const int a = v < 0 ? -v : v;
  const int r = (a + (1 << (s - 1))) >> s;
  return v < 0 ? -r : r;
}

/* clamp_search_range() (ody_me_common.h:785-835, mcts off) as the search uses it: only the window
 * start matters, because ods_me_full_search_area/_vbs walk a fixed 8x8 grid from mv_delta_left/top.
 * So at a picture edge the whole window slides; candidates are not clamped one by one.
 */
int windowStart(int lo, int centre, int org, int picDim, int blk, int pad)
{
  const int allowedMin = -(org + pad);
  const int allowedMax = (picDim - org) - blk + pad - 1;
  return std::clamp(lo + centre, allowedMin, allowedMax) - centre;
}

struct Candidate
{
  MotionVector mv;
  std::int64_t cost = std::numeric_limits<std::int64_t>::max();
};

} // namespace

void OdysseyOpenLoopMe::NeighbourState::resetRow()
{
  // ods_me_nbmv_row_reset(): the left-neighbour pipeline is cleared at every superblock row, the
  // line buffer is not - it is what carries the row above.
  this->left = {};
  for (auto &d : this->leftDelay)
    d = {};
  this->above = {};
}

void OdysseyOpenLoopMe::NeighbourState::update(int sbCol, const MotionVector &b64Mv)
{
  /* ods_me_nbmv_update(). Two things happen here and the order is the point:
   *   - the line buffer entry for this column is read first (that is the row above) and only then
   *     overwritten with this superblock's vector, so next row sees this one;
   *   - the left neighbour is taken from three steps back in the pipeline, not from the superblock
   *     that just finished. That delay is hardware latency being modelled, and it changes which
   *     vector the predictor sees.
   */
  if (sbCol >= 0 && sbCol < static_cast<int>(this->lineBuffer.size()))
  {
    this->above                = this->lineBuffer[sbCol];
    this->lineBuffer[sbCol]    = b64Mv;
  }

  this->left          = this->leftDelay[2];
  this->leftDelay[2]  = this->leftDelay[1];
  this->leftDelay[1]  = this->leftDelay[0];
  this->leftDelay[0]  = b64Mv;
}

OdySearchPlanes odysseySearchPlanes(const MePlane &current, const MePlane &reference)
{
  /* The picture pipeline, in odyssey's order. The caller has already done the 10-bit shift if it
   * was needed (MePlane::fromLuma10AsShifted8) and padded the full-resolution planes to 64.
   *
   * half      = downscale_2d[MEANPOOL_2D](full, 2), padded to 32
   * halfToOrg = upscale_2d[BILINEAR_2D](half, 2)   - full size, but NOT the original picture
   */
  OdySearchPlanes planes;
  planes.srcHalf = current.downscaleOdysseyMeanpool(2, kOdysseyPadHalf);
  planes.refHalf = reference.downscaleOdysseyMeanpool(2, kOdysseyPadHalf);
  planes.srcUp   = planes.srcHalf.upscaleOdysseyBilinear(2, kOdysseyPadFull);
  planes.refUp   = planes.refHalf.upscaleOdysseyBilinear(2, kOdysseyPadFull);
  return planes;
}

OdySbResult odysseySearchSuperblock(const OdySearchPlanes &planes,
                                    int                    orgX,
                                    int                    orgY,
                                    const OdySbSearch     &search)
{
  const auto &srcHalf    = planes.srcHalf;
  const auto &refHalf    = planes.refHalf;
  const auto &srcUp      = planes.srcUp;
  const auto &refUp      = planes.refUp;
  const auto &candidates = search.candidates;
  const int   mvRateWeight = search.mvRateWeight;
  OdySbResult out;

  /* step 2 - centre decision on the HALF picture.
   *
   * 32x32 block (the superblock halved), each candidate refined by +-1 for nine points, and
   * the cost here is SAD plus a *linear* rate divided by two - not the SSE plus squared rate
   * that the later stages use. Using one cost for all stages changes which centre wins.
   */
  Candidate best;
  /* Every stage is corrected against the plane it searches, as clamp_search_range() does in
   * the encoder. The half and upscaled planes have different pads, so this cannot be hoisted.
   */
  const auto halfRange = clampSearchRange(
      refHalf, orgX / 2, orgY / 2, kSb / 2, kSb / 2, -1024, 1024, -1024, 1024);
  const auto clampHalf = [&halfRange](MotionVector mv) {
    mv.x = mv.x < halfRange.minDx ? halfRange.minDx : (mv.x > halfRange.maxDx ? halfRange.maxDx : mv.x);
    mv.y = mv.y < halfRange.minDy ? halfRange.minDy : (mv.y > halfRange.maxDy ? halfRange.maxDy : mv.y);
    return mv;
  };
  for (const auto &cand : candidates)
    for (int ry = -1; ry <= 1; ++ry)
      for (int rx = -1; rx <= 1; ++rx)
      {
        const MotionVector mv = search.odysseyHw ? MotionVector{cand.x + rx, cand.y + ry}
                                                : clampHalf({cand.x + rx, cand.y + ry});
        const auto         sad = blockSad(srcHalf,
                                  orgX / 2,
                                  orgY / 2,
                                  refHalf,
                                  orgX / 2 + mv.x,
                                  orgY / 2 + mv.y,
                                  kSb / 2,
                                  kSb / 2);
        const auto rate =
            search.odysseyHw
                ? divideRoundBits(odysseyMvCost(mv.x - search.centrePmv.x, mv.y - search.centrePmv.y), 2)
                : odysseyMvCost(mv.x - search.centrePmv.x, mv.y - search.centrePmv.y) / 2;
        if (const auto cost = sad + rate; cost < best.cost)
          best = {mv, cost};
      }

  out.centre = best.mv;
  // ods_me_search() resets pmv to the decided centre for the later stages (openloop_me.c:1139).
  const MotionVector stage3Pmv = search.stage3PmvIsCentre ? best.mv : search.centrePmv;
  const auto stage3Rate = [&](const MotionVector &mv) {
    if (!search.odysseyHw)
      return odysseyOpenLoopRateTerm(mv.x - stage3Pmv.x, mv.y - stage3Pmv.y, mvRateWeight);
    const std::int64_t scaled = divideRoundBits(
        mvRateWeight * odysseyMvCost(signedShiftRight(mv.x - signedShiftRight(stage3Pmv.x, 1), 1),
                                     signedShiftRight(mv.y - signedShiftRight(stage3Pmv.y, 1), 1)),
        6);
    return (scaled * scaled) << 6;
  };

  std::vector<MotionVector> halfCentres;

  if (search.forceZeroCentre)
  {
    halfCentres.push_back({});
  }
  else
  {
    /* step 3a - the stage odyssey labels "1/4".
     *
     * It does not build a quarter picture: it searches the HALF picture with an MV step of 2.
     * The range is split into a 2x2 grid of areas and the best of each is kept, giving up to
     * four centres for the next stage.
     */
    const int total = kOdyQuarterMaxSr;      // 32
    const int lo    = -(total / 2);          // -16
    std::array<Candidate, kOdyHalfCentres> areas;

    /* odysseyHw: the centre is pulled inside first (openloop_me.c:1166-1176), then each area's
     * window is placed by clamp_search_range() on the half picture (pad 64 / 2).
     */
    const int    ox = orgX >> 1, oy = orgY >> 1;
    const int    halfW = refHalf.width(), halfH = refHalf.height();
    MotionVector qc    = best.mv;
    if (search.odysseyHw)
    {
      if (qc.x + ox < -8)
        qc.x = -ox - 8;
      if (qc.y + oy < -8)
        qc.y = -oy - 8;
      if (ox + qc.x + 56 >= halfW + kSb / 2)
        qc.x = qc.x - ((ox + qc.x + 56) - (halfW + kSb / 2)) - 1;
      if (oy + qc.y + 56 >= halfH + kSb / 2)
        qc.y = qc.y - ((oy + qc.y + 56) - (halfH + kSb / 2)) - 1;
    }

    for (int ay = 0; ay < 2; ++ay)
      for (int ax = 0; ax < 2; ++ax)
      {
        auto     &area   = areas[static_cast<std::size_t>(ay) * 2 + ax];
        const int baseX  = lo + ax * (total / 2);
        const int baseY  = lo + ay * (total / 2);
        // Eight candidates per axis at a step of 2 - the fixed 8x8 = 64 pattern.
        for (int k = 0; k < 64; ++k)
          {
            const auto [ix, iy] = gridPosition(k, search.odysseyHw);
            const MotionVector mv =
                search.odysseyHw
                    ? MotionVector{qc.x + windowStart(baseX, qc.x, ox, halfW, kSb / 2, kOdysseyPadFull / 2) + ix * 2,
                                   qc.y + windowStart(baseY, qc.y, oy, halfH, kSb / 2, kOdysseyPadFull / 2) + iy * 2}
                    : clampHalf({best.mv.x + baseX + ix * 2, best.mv.y + baseY + iy * 2});
            const auto         sse = blockSse(srcHalf,
                                      orgX / 2,
                                      orgY / 2,
                                      refHalf,
                                      orgX / 2 + mv.x,
                                      orgY / 2 + mv.y,
                                      kSb / 2,
                                      kSb / 2);
            const auto cost = sse + stage3Rate(mv);
            if (cost < area.cost)
              area = {mv, cost};
          }
      }

    // odysseyHw: the four area winners are bubble-sorted by cost (stable) before step 3b.
    if (search.odysseyHw)
      std::stable_sort(areas.begin(), areas.end(), [](const Candidate &x, const Candidate &y) {
        return x.cost < y.cost;
      });
    for (const auto &a : areas)
      halfCentres.push_back(a.mv);
  }

  // step 3b - refine each centre on the half picture at step 1, range -4 .. +3.
  Candidate halfBest;
  for (const auto &centre : halfCentres)
    for (int k = 0; k < kOdyHalfMaxSr * kOdyHalfMaxSr; ++k)
      {
        const auto [ix, iy]   = gridPosition(k, search.odysseyHw);
        const int  dx         = ix - kOdyHalfMaxSr / 2;
        const int  dy         = iy - kOdyHalfMaxSr / 2;
        const MotionVector mv =
            search.odysseyHw
                ? MotionVector{centre.x + windowStart(-kOdyHalfMaxSr / 2, centre.x, orgX >> 1, refHalf.width(), kSb / 2, kOdysseyPadFull / 2) + ix,
                               centre.y + windowStart(-kOdyHalfMaxSr / 2, centre.y, orgY >> 1, refHalf.height(), kSb / 2, kOdysseyPadFull / 2) + iy}
                : clampHalf({centre.x + dx, centre.y + dy});
        const auto         sse = blockSse(srcHalf,
                                  orgX / 2,
                                  orgY / 2,
                                  refHalf,
                                  orgX / 2 + mv.x,
                                  orgY / 2 + mv.y,
                                  kSb / 2,
                                  kSb / 2);
        const auto cost = sse + stage3Rate(mv);
        if (cost < halfBest.cost)
          halfBest = {mv, cost};
      }

  /* step 4 - VBS on the upscaled picture.
   *
   * Centre is the half result doubled. The 16x16 candidate grid is walked as four passes of
   * 8x8 offset by one step each, which is how the hardware issues it; the set of positions is
   * the same but the *order* differs, and order decides ties.
   *
   * SSE is computed per 8x8 with a row stride of 2 (cost_stride), then summed up the ladder.
   */
  constexpr int kUnits = kSb / 8;
  auto &best8  = out.b8;
  auto &best16 = out.b16;
  auto &best32 = out.b32;
  auto &best64 = out.b64;

  const MotionVector vbsCentre{halfBest.mv.x * 2, halfBest.mv.y * 2};
  out.vbsCentre = vbsCentre;
  const auto upRange = clampSearchRange(refUp, orgX, orgY, kSb, kSb, -1024, 1024, -1024, 1024);
  const int          lo4 = -(kOdyFullMaxSr / 2); // -8
  const int vbsStartX = windowStart(lo4, vbsCentre.x, orgX, refUp.width(), kSb, kOdysseyPadFull);
  const int vbsStartY = windowStart(lo4, vbsCentre.y, orgY, refUp.height(), kSb, kOdysseyPadFull);

  for (int pass = 0; pass < 4; ++pass)
  {
    const int offX = pass & 1;
    const int offY = (pass >> 1) & 1;
    for (int k = 0; k < 64; ++k)
      {
        const auto [ix, iy] = gridPosition(k, search.odysseyHw);
        MotionVector mv{vbsCentre.x + lo4 + offX + ix * 2,
                        vbsCentre.y + lo4 + offY + iy * 2};
        if (search.odysseyHw)
        {
          mv = {vbsCentre.x + vbsStartX + offX + ix * 2, vbsCentre.y + vbsStartY + offY + iy * 2};
        }
        else
        {
          mv.x = mv.x < upRange.minDx ? upRange.minDx : (mv.x > upRange.maxDx ? upRange.maxDx : mv.x);
          mv.y = mv.y < upRange.minDy ? upRange.minDy : (mv.y > upRange.maxDy ? upRange.maxDy : mv.y);
        }

        /* The rate is charged once per 8x8 against the offset from the search centre, and the
         * larger sizes inherit it by summing those costs. So the cost of every size already
         * carries the rate before it is compared - there is no separate rate at 16/32/64.
         */
        const std::int64_t mrate =
            divideRoundBits(odysseyMvCost(mv.x - vbsCentre.x, mv.y - vbsCentre.y), 8);
        const auto rate8 = search.odysseyHw ? mrate * mrate
                                            : odysseyVbsRateTerm(mv.x - vbsCentre.x, mv.y - vbsCentre.y);

        std::array<std::int64_t, kUnits * kUnits> cost8{};
        for (int uy = 0; uy < kUnits; ++uy)
          for (int ux = 0; ux < kUnits; ++ux)
          {
            const int  x = orgX + ux * 8;
            const int  y = orgY + uy * 8;
            const auto v = blockSse(srcUp, x, y, refUp, x + mv.x, y + mv.y, 8, 8, 2, search.odysseyHw ? 2 : 1) + rate8;
            cost8[static_cast<std::size_t>(uy) * kUnits + ux] = v;
            auto &b = best8[static_cast<std::size_t>(uy) * kUnits + ux];
            if (v < b.cost)
              b = {v, mv};
          }

        std::array<std::int64_t, 16> cost16{};
        for (int qy = 0; qy < 4; ++qy)
          for (int qx = 0; qx < 4; ++qx)
          {
            std::int64_t v = 0;
            for (int y = 0; y < 2; ++y)
              for (int x = 0; x < 2; ++x)
                v += cost8[static_cast<std::size_t>(qy * 2 + y) * kUnits + (qx * 2 + x)];
            cost16[static_cast<std::size_t>(qy) * 4 + qx] = v;
            auto &b = best16[static_cast<std::size_t>(qy) * 4 + qx];
            if (v < b.cost)
              b = {v, mv};
          }

        std::int64_t total = 0;
        for (int hy = 0; hy < 2; ++hy)
          for (int hx = 0; hx < 2; ++hx)
          {
            std::int64_t v = 0;
            for (int y = 0; y < 2; ++y)
              for (int x = 0; x < 2; ++x)
                v += cost16[static_cast<std::size_t>(hy * 2 + y) * 4 + (hx * 2 + x)];
            auto &b = best32[static_cast<std::size_t>(hy) * 2 + hx];
            if (v < b.cost)
              b = {v, mv};
            total += v;
          }

        if (total < best64.cost)
          best64 = {total, mv};
      }
  }

  return out;
}

bool OdysseyOpenLoopMe::canRun(const MePictureSet &pictures, std::string *reason) const
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
  if (pictures.current.width() < 2 || pictures.current.height() < 2)
  {
    if (reason)
      *reason = "picture is too small to halve";
    return false;
  }
  return true;
}

MeFrameResult OdysseyOpenLoopMe::estimateFrame(const MePictureSet &pictures, const MeParams &params)
{
  MeFrameResult result;
  result.frameIdx    = pictures.frameIdx;
  result.refFrameIdx = pictures.refFrameIdx;
  result.algorithm   = Algorithm::OdysseyOpenLoop;

  if (std::string why; !this->canRun(pictures, &why))
  {
    result.error = why;
    return result;
  }

  const auto planes = odysseySearchPlanes(pictures.current, pictures.reference);

  /* mv_rate_weight comes out of ods_me_construct_candidates() in the encoder, where it is derived
   * from the order_hint distance. We take it straight from the frame interval: a reference further
   * away makes a given vector cheaper to justify, so the weight falls.
   */
  const int  distance     = std::max(1, std::abs(params.frameInterval));
  const int  mvRateWeight = std::max(1, 8 / distance);

  const int sbCols = (pictures.current.width() + kSb - 1) / kSb;
  const int sbRows = (pictures.current.height() + kSb - 1) / kSb;

  NeighbourState neighbours;
  neighbours.lineBuffer.assign(static_cast<std::size_t>(sbCols), MotionVector{});

  if (params.progress != nullptr)
    params.progress->begin(sbCols * sbRows);

  for (int sbY = 0; sbY < sbRows; ++sbY)
  {
    neighbours.resetRow();

    for (int sbX = 0; sbX < sbCols; ++sbX)
    {
      /* Between superblocks is the right granularity: fine enough that a frame change feels
       * immediate, coarse enough that the check is invisible next to a superblock's worth of SAD.
       */
      if (params.cancel != nullptr && params.cancel->cancelled())
      {
        result.cancelled = true;
        return result;
      }

      /* Counted on entry rather than on exit, so that a superblock which takes an early exit lower
       * down still counts. The consequence is that the counter reaches the total while the last
       * superblock is still being worked on - which is why "finished" is the arrival of the result,
       * not the counter hitting its total.
       */
      if (params.progress != nullptr)
        params.progress->advance();

      const int orgX = sbX * kSb;
      const int orgY = sbY * kSb;

      /* step 1 - centre candidates.
       *
       * Availability follows position rather than odyssey's pipeline counters; see the header for
       * what that costs. B is the superblock above, C the one above-right, D the line buffer entry
       * that the update is about to replace, A the pipeline-delayed left.
       */
      const bool availA = sbX > 0;
      const bool availB = sbY > 0;
      const bool availC = sbY > 0 && sbX + 1 < sbCols;
      const bool availD = sbY > 0;

      const MotionVector mvB = availB ? neighbours.lineBuffer[sbX] : MotionVector{};
      const MotionVector mvC =
          availC ? neighbours.lineBuffer[static_cast<std::size_t>(sbX) + 1] : MotionVector{};

      const auto pmv = params.forceZeroCentre
                           ? MotionVector{}
                           : selectPredictor(
                                 neighbours.left, mvB, mvC, neighbours.above, availA, availB, availC, availD);

      // The predictor is stored in eighth-pel; the search works in whole pixels.
      OdySbSearch search;
      search.candidates.push_back({});
      if (!params.forceZeroCentre && !pmv.isZero())
        search.candidates.push_back({pmv.x / 8, pmv.y / 8});
      search.centrePmv       = {pmv.x / 8, pmv.y / 8};
      search.mvRateWeight    = mvRateWeight;
      search.forceZeroCentre = params.forceZeroCentre;

      const auto sb = odysseySearchSuperblock(planes, orgX, orgY, search);
      const auto &best8  = sb.b8;
      const auto &best16 = sb.b16;
      const auto &best32 = sb.b32;
      const auto &best64 = sb.b64;
      using Best         = OdySbBest;
      constexpr int kUnits = kSb / 8;

      const auto emit = [&](BlockSize size, int x, int y, const Best &b) {
        if (!params.blockSizes.contains(size))
          return;
        MeBlockResult r;
        r.block      = {x, y, size};
        r.mv         = MotionVector::fromFullPel(b.mv.x, b.mv.y);
        r.nativeCost = b.cost;
        /* The common metric is measured against the ORIGINAL pictures, not the upscaled ones the
         * search ran on. That is the point of having it: it puts this algorithm and SVT's on the
         * same footing, and the preprocessing difference is exactly what would otherwise be
         * invisible.
         */
        r.commonSad = commonMetricSad(pictures.current, pictures.reference, r.block, r.mv);
        result.blocks.push_back(r);
      };

      for (int uy = 0; uy < kUnits; ++uy)
        for (int ux = 0; ux < kUnits; ++ux)
          emit(BlockSize::Blk8,
               orgX + ux * 8,
               orgY + uy * 8,
               best8[static_cast<std::size_t>(uy) * kUnits + ux]);
      for (int qy = 0; qy < 4; ++qy)
        for (int qx = 0; qx < 4; ++qx)
          emit(BlockSize::Blk16,
               orgX + qx * 16,
               orgY + qy * 16,
               best16[static_cast<std::size_t>(qy) * 4 + qx]);
      for (int hy = 0; hy < 2; ++hy)
        for (int hx = 0; hx < 2; ++hx)
          emit(BlockSize::Blk32,
               orgX + hx * 32,
               orgY + hy * 32,
               best32[static_cast<std::size_t>(hy) * 2 + hx]);
      emit(BlockSize::Blk64, orgX, orgY, best64);

      // The superblock's 64x64 vector is what feeds the neighbour state, in eighth-pel.
      neighbours.update(sbX, MotionVector::fromFullPel(best64.mv.x, best64.mv.y));
    }
  }

  return result;
}

} // namespace bda::me
