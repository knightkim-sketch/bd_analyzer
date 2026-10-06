// The decoder-driven steps of Find diff, shared by the Find diff window and the CLI tools.
//
// Step 4 (block syntax of one frame) and layer D (reconstructed pictures) both need a decoder, so
// they live here at the src/integration boundary rather than in the Qt-free src/diff core, which
// only does the comparing. They used to live in tools/cli/stream-diff-blocks.cpp; the window needs
// exactly the same walk, and two copies of "which display frame is this coded frame" is how one of
// them ends up answering for the frame next door.
//
// Both steps open their own playlist items rather than borrowing the ones in the playlist. Decoding
// a frame moves an item's decoder, and the viewer is driving the user's items from its own threads;
// a private item is the only way to read a frame without racing what is on screen.
//
// Coordinate convention: SB and MI are (row, col). Pixel positions are (x, y).
#pragma once

#include <QString>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "diff/BlockSyntaxDiff.h"
#include "diff/ReconDiff.h"

class QAbstractItemModel;

namespace bda::integration
{

using StepProgress = std::function<void(const std::string &)>;

/* (temporal unit, OBU index within it) -> display frame index.
 *
 * The temporal unit is the packet index the parser logged ("Global AVPacket Count"), which is what
 * step 3 counts; the OBU index counts from the temporal delimiter, as step 3 does.
 */
using CodedFrameId = std::pair<int, int>;
using DisplayMap   = std::map<CodedFrameId, int>;

/* Work out which display frame shows each coded frame, from a parsed packet model.
 *
 * These are not the same number and must not be assumed to be. A coded frame with show_frame = 0
 * is a hidden alternate reference that some later temporal unit puts on screen with
 * show_existing_frame. Measured on the stream this was written for: step 3 named TU 90 / OBU 2,
 * order_hint 26, hidden - and display index 90 shows order_hint 25, a different frame entirely.
 *
 * So the reference slots are simulated the way the decoder keeps them: a decoded frame is written
 * into every slot its refresh_frame_flags names, and show_existing_frame displays whatever is in
 * the slot it points at. A coded frame nothing ever shows is absent from the map.
 */
DisplayMap buildDisplayMap(const QAbstractItemModel &model);

//!< Parse the stream and look one coded frame up. -1 with error set when it is never displayed.
int resolveDisplayIndex(const QString &path, int tu, int obu, std::string &error);

struct BlockStepOptions
{
  int  sbSize{};        //!< 64 or 128; anything else probes it from the largest block.
  bool all{};           //!< Scan the whole frame instead of stopping at the first difference.
  const std::atomic<bool> *cancel{};
  StepProgress progress;
};

struct BlockStepResult
{
  std::string error; //!< Set when the frame could not be compared at all.
  bool        cancelled{};

  int  frameIdx{-1};
  int  width{}, height{};
  int  sbSize{};
  bool sbSizeGiven{};

  std::vector<bda::diff::SbDiff> differing; //!< Raster order.
  std::size_t comparedPositions{}, differingPositions{};
  std::size_t scannedSbs{}, sbTotal{};
  bool        stoppedEarly{};
  long        queriesA{}, queriesB{};
  double      coverageA{}, coverageB{}; //!< Percent of the whole frame's MI positions.
  long        staleA{}, staleB{};

  bool ok() const { return this->error.empty() && !this->cancelled; }

  //!< The first superblock in which a block really differs - the answer.
  const bda::diff::SbDiff *firstWithBlockDiff() const;
  /* The first superblock that differs only in superblock totals (sb_bitcount), when it comes
   * before the answer. Same decisions, different residual: real, but not where they parted ways.
   */
  const bda::diff::SbDiff *firstTotalsOnlyBefore() const;
};

/* Decode one display frame of both streams and compare it superblock by superblock.
 *
 * Walks in raster order and, unless options.all, stops at the first superblock whose blocks differ.
 * That is the least work an exact answer can cost: proving a superblock is the first to differ
 * needs every one before it compared, and nothing after it. Not a binary search - see the design
 * document, section 4.5: superblocks after the divergence can be identical again.
 */
BlockStepResult runBlockStep(const QString &          pathA,
                             const QString &          pathB,
                             int                      frameIdx,
                             const BlockStepOptions &options);

struct ReconFrame
{
  int                  frameIdx{};
  bda::diff::FrameDiff diff;
};

struct ReconStepOptions
{
  bool stopAtFirst{}; //!< Stop at the first frame whose pictures differ.
  const std::atomic<bool> *cancel{};
  StepProgress progress;
};

struct ReconStepResult
{
  std::string error;
  bool        cancelled{};

  bda::diff::FrameLayout  layout;
  int                     frameCount{}; //!< Display frames in the shorter stream.
  std::vector<ReconFrame> frames;       //!< Every compared frame, in display order.

  bool ok() const { return this->error.empty() && !this->cancelled; }
  //!< Index into frames of the first frame whose pictures differ, or -1.
  int firstDiffering() const;
};

/* Decode both streams frame by frame in display order and compare the reconstructed samples.
 *
 * The decoding is the item's own - the same path the viewer and Add Difference Sequence take. Only
 * the arithmetic is new: the difference item reports its MSE as formatted text and converts every
 * frame to RGB on the way, neither of which a per-frame table wants.
 */
ReconStepResult runReconStep(const QString &          pathA,
                             const QString &          pathB,
                             const ReconStepOptions &options);

//!< "Y 41.23 dB" style, or "identical" for a plane with no difference.
std::string formatPlanePsnr(const bda::diff::PlaneDiff &plane, unsigned bitDepth);

} // namespace bda::integration
