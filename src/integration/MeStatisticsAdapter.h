// The boundary between the Qt-free ME core (src/me) and the upstream statistics overlay.
//
// This is the only place in our tree that knows about both, which is the arrangement the
// repository's design rules ask for: src/me stays free of upstream types, and everything that has
// to touch YUView lives here.
//
// Why the overlay is reused rather than a new motion vector drawer being written:
//
//   The upstream overlay already draws vectors - AV1's own motion vectors go through it as
//   StatisticsType(24, "Motion Vector 0", 4) plus addBlockVector() - and it brings arrow heads,
//   zoom-aware scaling, per-type line style and colour, visibility toggles, opacity, uiGroup
//   grouping, the style dialog and CSV export with it. A second drawer would have to reimplement
//   all of that, and the point of this feature is to look at reproduced vectors *next to* the
//   bitstream's own. Two overlay systems on one canvas would drift apart on zoom, style and draw
//   order exactly when the comparison matters.
//
// So ME results are registered as ordinary statistics types, one per (algorithm, block size), and
// the ME panel's block-size checkboxes decide which of them exist.
#pragma once

#include <string>

#include "me/MeTypes.h"

namespace stats
{
class StatisticsData;
}

namespace bda::integration
{

/* Statistics type IDs for the ME overlays.
 *
 * Kept well clear of the decoders' own numbering: the dav1d path uses up to the twenties and adds
 * more with each patch, so a base of 200 leaves room for both to grow without a silent collision.
 * A collision would not fail loudly - it would merge two overlays into one type.
 */
inline constexpr int kMeStatTypeBase = 200;

int meVectorTypeId(me::Algorithm algorithm, me::BlockSize size);
int meCostTypeId(me::Algorithm algorithm, me::BlockSize size);

/* The common metric gets its own type rather than sharing the cost type.
 *
 * A statistics block carries one value, so the native cost and the common SAD cannot both live on
 * one type. Splitting them is also what makes the comparison usable: the common SAD is on the same
 * scale for every algorithm, so colouring by it across two overlays says something, while
 * colouring by native cost across algorithms says nothing.
 */
int meCommonSadTypeId(me::Algorithm algorithm, me::BlockSize size);

/* Make the registered ME types match `sizes` exactly: add what is missing, remove what is no
 * longer wanted, and drop the cached data of anything removed.
 *
 * The ME panel's checkboxes are the single source of truth for this. Registering all four sizes
 * and letting the statistics panel's own per-type toggles hide them would put the same switch in
 * two places, and a user turning a size off in one place and on in the other could not tell which
 * one won.
 */
bool syncMeStatTypes(stats::StatisticsData  &data,
                     me::Algorithm           algorithm,
                     const me::BlockSizeSet &sizes);

/* Remove every ME type of every algorithm, cached data included. For switching algorithms or
 * closing the item.
 */
void clearMeStatTypes(stats::StatisticsData &data);

/* Push a frame's worth of results into the overlay.
 *
 * Vectors go to the vector types, and both costs to the cost types: the native cost as the block
 * value, with the common metric carried alongside so the block info pane can show them together.
 * Only the sizes currently registered are written - a result for an unregistered size is dropped
 * rather than silently creating a type nobody asked for.
 */
void fillMeStatistics(stats::StatisticsData &data, const me::MeFrameResult &result);

/* Label for a type, so the panel and the block info pane agree on the wording. Exposed because
 * the native cost name differs per algorithm and the comparison is meaningless without it.
 */
std::string meVectorTypeName(me::Algorithm algorithm, me::BlockSize size);
std::string meCostTypeName(me::Algorithm algorithm, me::BlockSize size);
std::string meCommonSadTypeName(me::Algorithm algorithm, me::BlockSize size);
std::string meGroupName(me::Algorithm algorithm);

} // namespace bda::integration
