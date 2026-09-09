// Collecting the (bits, PSNR) points a BD-rate needs, out of the playlist items.
//
// Part of the src/integration boundary: this drives the decoder and the pixel statistics, and hands
// plain numbers to src/bdrate for the maths.
#pragma once

#include <QString>

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "bdrate/BdRateMath.h"
#include "integration/BdRateGroups.h"

namespace bda::integration
{

//!< What one stream spent and lost on one region of one frame.
struct BdRateSample
{
  double       bits{};
  double       sse{-1.0};
  std::int64_t sampleCount{};

  //!< PSNR, or nothing when the SSE is missing or the region is lossless.
  std::optional<double> psnr() const { return bdrate::psnrFromSse(this->sse, this->sampleCount); }
};

//!< Superblock coordinate on the shared grid: (column, row), not pixels.
using BdRateSbKey = std::pair<int, int>;

struct BdRateFrameData
{
  int frameIdx{-1};

  /* Indexed [group][point]. The frame totals: bits summed over the frame's superblocks, SSE summed
   * likewise, so the PSNR is the frame's and not an average of per-block PSNRs - averaging dB is
   * not the same number and is the usual way this gets reported wrongly.
   */
  std::vector<std::vector<BdRateSample>> frameTotals;

  //!< Per superblock, then [group][point].
  std::map<BdRateSbKey, std::vector<std::vector<BdRateSample>>> perSuperblock;

  /* True when a stream's SSE has been requested but has not arrived. The pixel statistics are
   * computed off the GUI thread, so the first collection of a frame usually comes back pending and
   * the caller retries. Not an error.
   */
  bool    pending{};
  QString error;

  bool usable() const { return this->error.isEmpty() && !this->frameTotals.empty(); }
};

/* Collect one frame for every point of every group.
 *
 * Synchronous as far as the decoder goes - one frame across a handful of streams is around 25 ms
 * each, measured, which is short enough to do in place. The SSE is the asynchronous half: this
 * requests it and reports `pending` when it is not there yet, rather than spinning a nested event
 * loop.
 *
 * Switches statistics collection on for every stream it touches, because sb_bitcount is only
 * gathered while something asks for it.
 */
BdRateFrameData collectBdRateFrame(const std::vector<BdRateGroup> &groups, int frameIdx);

/* The curve of one group over one region, ready for bdrate().
 *
 * Points with no PSNR (no original, or lossless) are dropped - the maths cannot use them and
 * inventing a value would move the answer.
 */
std::vector<bdrate::RatePoint> curveFor(const std::vector<BdRateSample> &points);

} // namespace bda::integration
