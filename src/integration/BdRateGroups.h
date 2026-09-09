// Grouping a playlist selection into a BD-rate curve.
//
// One group is one curve: the streams in it are operating points of the same encode, and BD-rate
// compares one group against the anchor. A selection becomes a group, so pressing the button twice
// with two different selections gives two curves.
//
// Part of the src/integration boundary - this is the half of the feature that knows about
// playlistItem. The maths is in src/bdrate and knows nothing.
#pragma once

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include <common/Typedef.h>

class playlistItem;
class playlistItemCompressedVideo;

namespace bda::integration
{

/* One operating point: one stream.
 *
 * The item is borrowed, and it can go away - the playlist owns it and the user can delete it while
 * the plot window is open. Every use re-checks it rather than assuming it is still there.
 */
struct BdRatePoint
{
  playlistItemCompressedVideo *item{};
  QString                      label; //!< File name, which is how the user recognises the QP point.
};

struct BdRateGroup
{
  QString                  name;
  std::vector<BdRatePoint> points;
  /* The original the PSNR is measured against. Taken from a raw item in the same selection when
   * there is one, so the user can drop the source in with the streams and not think about it.
   */
  QString                  originalPath;

  //!< Frame size and superblock size, which must agree across groups. See makeBdRateGroup().
  Size     frameSize;
  unsigned superblockSize{};
};

/* Why a selection could not become a group. Reported so the button can say what is missing rather
 * than doing nothing.
 */
enum class BdRateGroupError
{
  Ok,
  NoStreams,           //!< Nothing in the selection is a compressed stream.
  OnePointOnly,        //!< A single stream is a point, not a curve.
  NoOriginal,          //!< No raw item in the selection and none already attached.
  OriginalRejected,    //!< The raw item does not match the streams (size, pixel format).
  NoBitCounts,         //!< A stream is not decoded by the dav1d analyzer, so it has no sb_bitcount.
  MixedGeometry,       //!< The streams disagree on frame size or superblock size.
  GeometryMismatch     //!< This group disagrees with the groups already added.
};

struct BdRateGroupResult
{
  BdRateGroupError error{BdRateGroupError::Ok};
  BdRateGroup      group;
  QString          message; //!< Ready to show. Names the offending file where there is one.

  bool ok() const { return this->error == BdRateGroupError::Ok; }
};

/* Turn a playlist selection into a group.
 *
 * `existing` are the groups already in the model; a new group has to agree with them on frame size
 * and superblock size, because the whole feature indexes by superblock coordinate and two different
 * grids do not describe the same region.
 *
 * The original is attached to every stream in the group as a side effect - that is what makes an
 * SSE, and therefore a PSNR, available at all.
 */
BdRateGroupResult makeBdRateGroup(const QList<playlistItem *>     &selection,
                                  const std::vector<BdRateGroup>  &existing);

/* A name for a group, from the common prefix of its file names.
 *
 * Four encodes of one configuration are usually named for it - `fast_q30.ivf`, `fast_q35.ivf` and
 * so on - so the prefix is the configuration, which is exactly what the curve should be called.
 * Falls back to "Group N" when there is nothing in common, which happens when the files are named
 * only by QP.
 */
QString bdRateGroupName(const QStringList &fileNames, int groupIndex);

//!< Human readable form of the error, for the button's tooltip and the window's status line.
QString bdRateGroupErrorText(const BdRateGroupResult &result);

} // namespace bda::integration
