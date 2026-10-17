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

#include "me/GlobalModel.h"
#include "me/GlobalMotion.h"

namespace bda::integration
{

inline constexpr int kGmStatTypeBase   = 300;
inline constexpr int kGmAcceptedTypeId = kGmStatTypeBase;
inline constexpr int kGmRawTypeId      = kGmStatTypeBase + 1;
inline constexpr int kGmWindowTypeId   = kGmStatTypeBase + 2;
inline constexpr int kGmInlierTypeId   = kGmStatTypeBase + 3; //!< RANSAC model: 1 inlier, 0 outlier
inline constexpr int kGmModelTypeId    = kGmStatTypeBase + 4; //!< RANSAC model: its vector per 64x64
inline constexpr int kGmLastTypeId     = kGmModelTypeId;

/* Register the types if they are missing (the window grid and its vectors, and the RANSAC model's
 * inlier mask and vectors). True when anything was added - the caller then
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

/* Replace the model blocks with one frame's RANSAC model: the inlier mask on the blocks it was fitted
 * to, and the model's vector at the centre of every 64x64 block of the picture. A frame without a
 * model (switched off, or the fit failed) leaves both empty.
 */
void fillGmModelStatistics(stats::StatisticsData &  data,
                           const me::GmModelResult *model,
                           int                      width,
                           int                      height);

} // namespace bda::integration
