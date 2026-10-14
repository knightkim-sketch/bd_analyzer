// odyssey open-loop ME, advanced: the whole GF structure in odyssey's analysis order, with the
// temporal MV (TMV) candidates that a frame-pair estimator cannot have.
//
// What it adds over OdysseyOpenLoopMe (which is kept as it was):
//   * frames are analysed bottom-up through the GF groups (OdysseyGfStructure), each against the
//     references odyssey gives it - two-sided for B, one or two past ones for P;
//   * step 1 is ods_me_construct_candidates() as written (openloop_me.c:196-628): the 21 slots
//     A B C D | TDMP x4 | TMV x8 | ZERO | MEDIAN | A_PASS C_PASS B_PASS, de-duplicated, range
//     checked, cut to max_num_cands = 6 and clamped to the padding;
//   * the hardware neighbour pipeline: A is two superblocks back when a superblock is searched
//     against two references, four when against one (ods_me_nbmv_update);
//   * mv_rate_weight is 1 and steps 3a/3b charge rate against the decided centre, as
//     ods_me_search() does (openloop_me.c:580, :1138-1141).
//
// Decided by Knight where the source left room (design section 2):
//   * the references of one superblock are searched nearest first;
//   * TMV is stored per 16x16 block (8x8 on the half picture); TmvGrid::Sb64 keeps odyssey-
//     pristine's per-superblock 64x64 vector for comparing against its trace;
//   * an optional external centre MV (e.g. the Global motion window's) goes first in the list.
//
// TDMP is off, MCTF is not applied, one tile. Qt-free.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "MeTypes.h"
#include "MePlane.h"
#include "OdysseyGfStructure.h"

namespace bda::me
{

struct OdyAdvParams
{
  int gfInterval = 16;
  enum class TmvGrid
  {
    Block16, //!< Knight's decision: the co-located 16x16 block's vector.
    Sb64,    //!< odyssey-pristine 0fab39b0: the co-located superblock's 64x64 vector.
  } tmvGrid = TmvGrid::Block16;
  BlockSizeSet blockSizes = BlockSizeSet::all();
  int          maxCandidates = 6; //!< max_num_cands (lookahead.c:3427)

  /* An external centre for a superblock, in half-picture pixels, or nothing. Called for every
   * superblock search; it goes first in the candidate list.
   */
  std::function<std::optional<MotionVector>(int poc, int refPoc, int sbX, int sbY)> centre;

  /* Per-superblock search record, in the terms of odyssey's clist/dcent traces (half-picture
   * pixels), for comparing against them. Called after each superblock search when set.
   */
  struct SbTrace
  {
    int                       poc{}, list{}, refIdx{}, refPoc{}, sbX{}, sbY{};
    MotionVector              pmv;        //!< MEDIAN >> 3, as clist prints it
    std::vector<MotionVector> candidates; //!< After de-duplication, the cut and the clamp
    MotionVector              centre;     //!< The decided step-2 centre, as dcent prints it
    MotionVector              vbsCentre;  //!< Step-4 centre, full pixels (msear's first vector / 8)
  };
  std::function<void(const SbTrace &)> trace;

  const CancelToken *cancel   = nullptr;
  ProgressToken     *progress = nullptr; //!< Counts frames.
};

struct OdyAdvSearch
{
  int           list{};   //!< 0 or 1
  int           refIdx{}; //!< Index within the list (P has two list-0 references).
  int           refPoc{};
  MeFrameResult result;   //!< algorithm = OdysseyOpenLoopAdv, vectors in eighth-pel
};

struct OdyAdvFrame
{
  OdyFrameInfo              info;
  std::vector<OdyAdvSearch> searches; //!< In the order they were searched (nearest first).
};

struct OdyAdvResult
{
  std::map<int, OdyAdvFrame> frames; //!< Every analysed POC (the key frame has no searches).
  std::string                error;
  bool                       cancelled{};
};

/* 8-bit luma of display frame `poc`, padded to kOdysseyPadFull; an empty plane on failure. */
using OdyFrameLoader = std::function<MePlane(int poc)>;

/* Analyse frames [0, frameCount) in odyssey's order and stop after the group containing
 * `lastPoc` (frameCount - 1 for everything). Every frame before it is analysed too: its vectors
 * are the TMV candidates of the frames after it.
 */
OdyAdvResult estimateOdysseyOpenLoopAdv(const OdyFrameLoader &load,
                                        int                   frameCount,
                                        int                   lastPoc,
                                        const OdyAdvParams   &params);

} // namespace bda::me
