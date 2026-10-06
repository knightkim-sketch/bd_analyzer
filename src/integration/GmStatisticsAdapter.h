// Global motion windows as overlay statistics, on an item's motion estimation container.
//
// The same container the ME panel draws into (playlistItem::getMotionEstimationData()), with ids of
// its own. 300 and up: ME takes 200 + 16 per algorithm slot, so a fourth slot would already reach
// 263, and 250 would collide with it.
//
// Three types per frame:
//   accepted  the vector odyssey would use - a rejected axis is 0 - drawn bright
//   raw       the search's answer for windows where an axis was rejected, drawn grey, so a
//             rejection can be seen next to what it rejected
//   window    the 4x4 grid; the block value is the number of accepted axes (0..2)
//
// Clicking a window lists all three in the Block Info pane, since that pane walks every type with
// data (StatisticsData::getBlockInfoAt).
#pragma once

#include <statistics/StatisticsData.h>

#include "me/GlobalMotion.h"

namespace bda::integration
{

inline constexpr int kGmStatTypeBase   = 300;
inline constexpr int kGmAcceptedTypeId = kGmStatTypeBase;
inline constexpr int kGmRawTypeId      = kGmStatTypeBase + 1;
inline constexpr int kGmWindowTypeId   = kGmStatTypeBase + 2;

/* Register the three types if they are missing. True when anything was added - the caller then
 * rebuilds the statistics controls, which is the only time it should.
 */
bool syncGmStatTypes(stats::StatisticsData &data);

void clearGmStatTypes(stats::StatisticsData &data);

/* Replace the GM blocks with one frame's windows, at source resolution. width / height are the
 * source picture's: the last column and row of windows take the pixels the downsampling dropped.
 * The caller sets the container's frame index first - setFrameIndex() clears the cache when it
 * moves.
 */
void fillGmStatistics(stats::StatisticsData &  data,
                      const me::GmFrameResult &frame,
                      int                      width,
                      int                      height);

} // namespace bda::integration
