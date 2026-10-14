#include "OdysseyOpenLoopAdvMe.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>

#include "MeCost.h"
#include "OdysseyOpenLoopMe.h"

namespace bda::me
{
namespace
{

constexpr int kSb = 64;

// Candidate slots, openloop_me.c:152-175 (the #if 1 branch).
constexpr int kSlotA = 0, kSlotB = 1, kSlotC = 2, kSlotD = 3;
constexpr int kSlotTdmp0 = 4, kSlotTdmp3 = 7;
constexpr int kSlotTmv0 = 8, kSlotTmv7 = 15;
constexpr int kSlotZero = 16, kSlotMedian = 17;
constexpr int kSlotAPass = 18, kSlotCPass = 19, kSlotBPass = 20;
constexpr int kSlots = 21;

/* SIGNED_SHIFT_RIGHT (common/include/ody_base.h:636-660): not a plain shift but a rounded one,
 * symmetric about zero - divide_round(|v|, s) with the sign put back.
 */
int ssr(int v, int s)
{
  const int a = v < 0 ? -v : v;
  const int r = (a + (1 << (s - 1))) >> s;
  return v < 0 ? -r : r;
}

MotionVector ssr(const MotionVector &v, int s) { return {ssr(v.x, s), ssr(v.y, s)}; }

int median3(int p, int q, int r) { return std::max(std::min(p, q), std::min(std::max(p, q), r)); }

/* get_median_mv(..., bvalid = false) - the call ods_me_construct_candidates() makes
 * (openloop_me.c:579, "don't use cdd_a to get median"): D stands in for A.
 */
MotionVector medianPredictor(const MotionVector &b,
                             const MotionVector &c,
                             const MotionVector &d,
                             bool                availB,
                             bool                availC,
                             bool                availD)
{
  if (availD && !availB && !availC)
    return d;
  if (availB && !availD && !availC)
    return b;
  if (availC && !availD && !availB)
    return c;
  return {median3(d.x, b.x, c.x), median3(d.y, b.y, c.y)};
}

// One frame's pictures, built once and shared by every search that uses the frame.
struct FramePictures
{
  MePlane full;      //!< 8-bit luma, padded to 64
  MePlane half;      //!< meanpool half, padded to 32
  MePlane halfToOrg; //!< bilinear back up, padded to 64
};

// ods_me_nbmv_* state of one search (list, reference): the line buffer and the A pipeline.
struct Neighbours
{
  std::vector<MotionVector> line; //!< Quarter-pel at full resolution, one per SB column.
  MotionVector              a, d;
  MotionVector              before[3];

  void resetRow()
  {
    this->a = this->d = {};
    for (auto &b : this->before)
      b = {};
  }
  void update(int sbX, const MotionVector &b64QuarterPel, bool twoReferences)
  {
    this->d       = this->line[std::size_t(sbX)];
    this->line[std::size_t(sbX)] = b64QuarterPel;
    if (!twoReferences)
    {
      this->a         = this->before[2];
      this->before[2] = this->before[1];
      this->before[1] = this->before[0];
    }
    else
      this->a = this->before[0];
    this->before[0] = b64QuarterPel;
  }
};

/* The static state of ods_me_construct_candidates() - the PASS vectors and the direction flags
 * live across superblocks, searches and frames in the source, so they live across the run here.
 */
struct PassState
{
  MotionVector a, b, c;
  bool         ref0List0{}, ref1List0{};
};

struct SearchSpec
{
  int list{}, refIdx{}, refPoc{};
};

} // namespace

OdyAdvResult estimateOdysseyOpenLoopAdv(const OdyFrameLoader &load,
                                        int                   frameCount,
                                        int                   lastPoc,
                                        const OdyAdvParams   &params)
{
  OdyAdvResult result;
  const auto   plan = planOdysseyGf(frameCount, params.gfInterval);
  if (plan.frames.empty())
  {
    result.error = "no frames";
    return result;
  }
  lastPoc               = std::clamp(lastPoc, 0, frameCount - 1);
  const int lastAnchor  = lastPoc == 0 ? 0 : plan.frames.at(lastPoc).groupAnchor;

  std::vector<int> order;
  for (int poc : plan.analysisOrder)
  {
    if (poc != 0 && plan.frames.at(poc).groupFirst > lastAnchor)
      break;
    order.push_back(poc);
  }
  if (params.progress != nullptr)
    params.progress->begin(int(order.size()));

  std::map<int, std::shared_ptr<FramePictures>> pictures;
  const auto picturesOf = [&](int poc) -> std::shared_ptr<FramePictures> {
    if (auto it = pictures.find(poc); it != pictures.end())
      return it->second;
    auto p  = std::make_shared<FramePictures>();
    p->full = load(poc);
    if (p->full.empty())
      return nullptr;
    /* odyssey resamples the picture extended to whole superblocks (edge rows and columns
     * repeated), so below and right of a picture that is not a multiple of 64 the half picture's
     * padding holds the downscaled extension, not the last half row repeated. The picture keeps
     * its own size for every range check; only the padding content follows odyssey (checked
     * pixel for pixel against its trace_anl_me cur_frame dump).
     */
    const int w = p->full.width(), h = p->full.height();
    const int aw = (w + kSb - 1) / kSb * kSb, ah = (h + kSb - 1) / kSb * kSb;
    MePlane   aligned(aw, ah, kOdysseyPadFull);
    for (int y = -kOdysseyPadFull; y < ah + kOdysseyPadFull; ++y)
      for (int x = -kOdysseyPadFull; x < aw + kOdysseyPadFull; ++x)
        aligned.at(x, y) = p->full.at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1));
    const auto alignedHalf = aligned.downscaleOdysseyMeanpool(2, kOdysseyPadHalf);
    const auto alignedUp   = alignedHalf.upscaleOdysseyBilinear(2, kOdysseyPadFull);
    p->half      = MePlane(w / 2, h / 2, kOdysseyPadHalf);
    p->halfToOrg = MePlane(w, h, kOdysseyPadFull);
    for (int y = -kOdysseyPadHalf; y < h / 2 + kOdysseyPadHalf; ++y)
      for (int x = -kOdysseyPadHalf; x < w / 2 + kOdysseyPadHalf; ++x)
        p->half.at(x, y) = alignedHalf.at(x, y);
    for (int y = -kOdysseyPadFull; y < h + kOdysseyPadFull; ++y)
      for (int x = -kOdysseyPadFull; x < w + kOdysseyPadFull; ++x)
        p->halfToOrg.at(x, y) = alignedUp.at(x, y);
    pictures[poc] = p;
    return p;
  };

  // Each analysed frame's stored vectors per list and superblock, quarter-pel at full resolution.
  std::map<int, std::array<std::vector<MotionVector>, 2>> stored;
  PassState pass;

  for (int poc : order)
  {
    if (params.progress != nullptr)
      params.progress->advance();
    if (params.cancel != nullptr && params.cancel->cancelled())
    {
      result.cancelled = true;
      return result;
    }

    const auto &info  = plan.frames.at(poc);
    auto       &frame = result.frames[poc];
    frame.info        = info;
    if (info.kind == OdyFrameKind::Key)
      continue;

    // The searches of one superblock, nearest reference first (list 0 first on a tie).
    std::vector<SearchSpec> specs;
    for (int list = 0; list < 2; ++list)
      for (int i = 0; i < int(info.refs[list].size()); ++i)
        specs.push_back({list, i, info.refs[list][std::size_t(i)]});
    std::stable_sort(specs.begin(), specs.end(), [poc](const SearchSpec &x, const SearchSpec &y) {
      return std::abs(poc - x.refPoc) < std::abs(poc - y.refPoc);
    });
    const bool twoRefs = specs.size() > 1; // num_ref_for_neighbor_mv_update

    const auto cur = picturesOf(poc);
    if (!cur)
    {
      result.error = "frame " + std::to_string(poc) + " could not be read";
      return result;
    }
    const int width = cur->full.width(), height = cur->full.height();
    const int sbCols = (width + kSb - 1) / kSb, sbRows = (height + kSb - 1) / kSb;

    std::vector<OdySearchPlanes> planes;
    std::vector<std::shared_ptr<FramePictures>> refs;
    std::vector<Neighbours> neighbours(specs.size());
    for (const auto &s : specs)
    {
      auto ref = picturesOf(s.refPoc);
      if (!ref)
      {
        result.error = "frame " + std::to_string(s.refPoc) + " could not be read";
        return result;
      }
      refs.push_back(ref);
      planes.push_back({cur->half, ref->half, cur->halfToOrg, ref->halfToOrg});
      OdyAdvSearch search;
      search.list                    = s.list;
      search.refIdx                  = s.refIdx;
      search.refPoc                  = s.refPoc;
      search.result.frameIdx         = poc;
      search.result.refFrameIdx      = s.refPoc;
      search.result.algorithm        = Algorithm::OdysseyOpenLoopAdv;
      frame.searches.push_back(std::move(search));
    }
    for (auto &n : neighbours)
      n.line.assign(std::size_t(sbCols), MotionVector{});
    auto &store = stored[poc];
    for (auto &s : store)
      s.assign(std::size_t(sbCols) * sbRows, MotionVector{});

    // hw_avail_* and friends (openloop_me.c:232-243, :291-294).
    const int  numProcess = sbCols << (twoRefs ? 1 : 0);
    const bool hwB        = twoRefs ? numProcess > 2 : numProcess > 3;
    const bool hwC        = twoRefs ? numProcess > 3 : numProcess > 4;
    const bool hwD        = twoRefs ? numProcess > 1 : numProcess > 2;
    const bool narrow     = sbCols < 256 / 64;

    for (int sbY = 0; sbY < sbRows; ++sbY)
    {
      for (auto &n : neighbours)
        n.resetRow(); // ods_me_nbmv_row_reset() resets every list and reference

      bool aValidRow = true;
      if (sbY >= (height >> 6))
        aValidRow = !(sbRows & 1);

      for (int sbX = 0; sbX < sbCols; ++sbX)
      {
        if (params.cancel != nullptr && params.cancel->cancelled())
        {
          result.cancelled = true;
          return result;
        }
        const int orgX = sbX * kSb, orgY = sbY * kSb;

        for (std::size_t si = 0; si < specs.size(); ++si)
        {
          const auto &spec   = specs[si];
          auto       &nb     = neighbours[si];
          const int   selRef = spec.list + spec.refIdx;
          const int   srcSub = poc - spec.refPoc;

          if (orgX == 0 && orgY == 0)
            (selRef == 0 ? pass.ref0List0 : pass.ref1List0) = srcSub >= 0;
          const bool firstA  = orgX == 2 * kSb && selRef == 0;
          const bool firstBC = orgX == 0 && selRef == 0;
          const bool sameDir = pass.ref0List0 == pass.ref1List0;
          const auto passOf  = [sameDir](const MotionVector &v) {
            return sameDir ? v : MotionVector{-v.x, -v.y};
          };

          std::array<MotionVector, kSlots> pre{};
          MotionVector                     mvB, mvC, mvD;
          bool                             availB = false, availC = false, availD = false;

          // Spatial: "A"
          if (((sbX >= 4 && !twoRefs) || (sbX >= 2 && twoRefs)) && aValidRow)
          {
            pre[kSlotA] = nb.a;
            if (twoRefs)
            {
              if (!firstA)
                pre[kSlotAPass] = passOf(pass.a);
              pass.a = nb.a;
            }
          }
          // "B"
          if (!narrow && sbY >= 1 && hwB)
          {
            mvB = pre[kSlotB] = nb.line[std::size_t(sbX)];
            availB            = true;
            if (twoRefs)
            {
              if (!firstBC)
                pre[kSlotBPass] = passOf(pass.b);
              pass.b = mvB;
            }
          }
          // "C"
          if (!narrow && sbY >= 1 && hwC && sbX + 1 < sbCols)
          {
            mvC = pre[kSlotC] = nb.line[std::size_t(sbX) + 1];
            availC            = true;
            if (twoRefs)
            {
              if (!firstBC)
                pre[kSlotCPass] = passOf(pass.c);
              pass.c = mvC;
            }
          }
          // "D"
          if (!narrow && sbY >= 1 && sbX >= 1 && hwD)
          {
            mvD = pre[kSlotD] = nb.d;
            availD            = true;
          }

          // Temporal (LATENCY_HIGH, openloop_me.c:404-430): already half-picture pixels after this.
          const auto &tmvFrames = info.tmvFrames[spec.list];
          for (std::size_t k = 0; k < tmvFrames.size() && k < 8; ++k)
          {
            const int t   = tmvFrames[k];
            const int rel = plan.frames.at(t).storedRef[spec.list];
            if (rel < 0)
              continue;
            const auto &src = stored.at(t)[std::size_t(spec.list)];
            if (src.empty())
              continue;
            const MotionVector q     = src[std::size_t(sbY) * sbCols + sbX];
            const int          scale = (srcSub << 5) / (t - rel);
            pre[std::size_t(kSlotTmv0) + k] = {ssr(ssr(q.x * scale, 5), 3), ssr(ssr(q.y * scale, 5), 3)};
          }

          pre[kSlotZero]       = {};
          const MotionVector pmv = medianPredictor(mvB, mvC, mvD, availB, availC, availD);
          pre[kSlotMedian]       = pmv;

          // Remove duplicated (openloop_me.c:586-625).
          std::array<bool, kSlots> dup{};
          for (int i = 0; i < kSlots; ++i)
          {
            dup[std::size_t(i)] = i >= kSlotTdmp0 && i <= kSlotTdmp3; // TDMP off
            if (!dup[std::size_t(i)] && (i < kSlotTmv0 || i > kSlotTmv7))
              pre[std::size_t(i)] = ssr(pre[std::size_t(i)], 3);
          }
          const int ax = orgX >> 1, ay = orgY >> 1, aw = width >> 1, ah = height >> 1, as = kSb >> 1;
          std::vector<MotionVector> cands;
          for (int i = 0; i < kSlots; ++i)
          {
            if (dup[std::size_t(i)])
              continue;
            const auto &c = pre[std::size_t(i)];
            if (ax + c.x > -as - 8 && ax + c.x + as < aw + as + 8 && ay + c.y > -as - 8 &&
                ay + c.y + as < ah + as + 8)
              cands.push_back(c);
            for (int j = i + 1; j < kSlots; ++j)
              if (!dup[std::size_t(j)] && pre[std::size_t(j)] == c)
                dup[std::size_t(j)] = true;
          }

          // Knight's centre MV goes first.
          if (params.centre)
            if (const auto c = params.centre(poc, spec.refPoc, sbX, sbY))
            {
              cands.erase(std::remove(cands.begin(), cands.end(), *c), cands.end());
              cands.insert(cands.begin(), *c);
            }

          const int keep = std::min({int(cands.size()), 16, params.maxCandidates});
          cands.resize(std::size_t(std::max(keep, 0)));
          if (cands.empty())
            cands.push_back({});

          // Clamp to the padding (ods_me_search, openloop_me.c:1092-1105).
          const int padW = ((width + kOdysseyPadFull) >> 1) - 1, padH = ((height + kOdysseyPadFull) >> 1) - 1;
          for (auto &c : cands)
          {
            if (ax + as + c.x + 1 > padW)
              c.x -= ax + as + c.x + 1 - padW;
            if (ax + c.x - 1 < -kOdysseyPadFull / 2)
              c.x += -kOdysseyPadFull / 2 - (ax + c.x - 1);
            if (ay + as + c.y + 1 > padH)
              c.y -= ay + as + c.y + 1 - padH;
            if (ay + c.y - 1 < -kOdysseyPadFull / 2)
              c.y += -kOdysseyPadFull / 2 - (ay + c.y - 1);
          }

          OdySbSearch search;
          search.candidates        = cands;
          search.centrePmv         = ssr(pmv, 3);
          search.stage3PmvIsCentre = true;
          search.mvRateWeight      = 1;
          search.odysseyHw         = true;
          const auto sb            = odysseySearchSuperblock(planes[si], orgX, orgY, search);
          if (params.trace)
            params.trace({poc, spec.list, spec.refIdx, spec.refPoc, sbX, sbY, search.centrePmv, cands, sb.centre, sb.vbsCentre});

          // Results, as OdysseyOpenLoopMe reports them.
          auto      &out  = frame.searches[si].result;
          const auto emit = [&](BlockSize size, int x, int y, const OdySbBest &b) {
            if (!params.blockSizes.contains(size))
              return;
            MeBlockResult r;
            r.block      = {x, y, size};
            r.mv         = MotionVector::fromFullPel(b.mv.x, b.mv.y);
            r.nativeCost = b.cost;
            r.commonSad  = commonMetricSad(cur->full, refs[si]->full, r.block, r.mv);
            out.blocks.push_back(r);
          };
          for (int i = 0; i < 64; ++i)
            emit(BlockSize::Blk8, orgX + (i % 8) * 8, orgY + (i / 8) * 8, sb.b8[std::size_t(i)]);
          for (int i = 0; i < 16; ++i)
            emit(BlockSize::Blk16, orgX + (i % 4) * 16, orgY + (i / 4) * 16, sb.b16[std::size_t(i)]);
          for (int i = 0; i < 4; ++i)
            emit(BlockSize::Blk32, orgX + (i % 2) * 32, orgY + (i / 2) * 32, sb.b32[std::size_t(i)]);
          emit(BlockSize::Blk64, orgX, orgY, sb.b64);

          // Stored for later TMV: the first reference of each list (inter_info[list][0]).
          if (spec.refIdx == 0)
          {
            const auto &mv = params.tmvGrid == OdyAdvParams::TmvGrid::Sb64 ? sb.b64.mv : sb.b16[0].mv;
            store[std::size_t(spec.list)][std::size_t(sbY) * sbCols + sbX] = {mv.x * 4, mv.y * 4};
          }

          // ods_me_nbmv_update(scale = 1): the 64x64 vector, quarter-pel at full resolution.
          nb.update(sbX, {sb.b64.mv.x * 4, sb.b64.mv.y * 4}, twoRefs);
        }
      }
    }
    // A P frame stores list 0 only (hl_update_reference_info: num_list = 1 on layer 1).
    if (info.layer == 1)
      store[1].clear();
  }
  return result;
}

} // namespace bda::me
