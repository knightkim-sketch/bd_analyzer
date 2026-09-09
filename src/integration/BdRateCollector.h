// Collecting the (bits, PSNR) points a BD-rate needs, out of the playlist items.
//
// Part of the src/integration boundary: this drives the decoder and the pixel statistics, and hands
// plain numbers to src/bdrate for the maths.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

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
 *
 * The rate axis carries **bits + 1**, uniformly, which is the usual convention for per-block
 * rate-distortion analysis. A skipped superblock genuinely costs 0 bits and log10(0) has no value,
 * so without it every skipped block would drop out of its curve - and "reached that quality for
 * nothing" is the most interesting thing such a block has to say. Applied to every point rather
 * than only to the zeros, because patching just the zeros would put a step in the axis; on frame
 * and sequence totals, which run to thousands of bits, the shift is not measurable. The tables
 * keep showing the bits that were actually spent, and the plot labels its axis accordingly.
 */
std::vector<bdrate::RatePoint> curveFor(const std::vector<BdRateSample> &points);

//!< The rate a measured bit count contributes on the curve. See curveFor().
inline double bdRateAxisRate(double bits) { return bits + 1.0; }

//!< Bits and error accumulated over a frame range, rather than for one frame.
struct BdRateSequenceData
{
  int firstFrame{-1};
  int lastFrame{-1};

  std::vector<std::vector<BdRateSample>>                        totals;
  std::map<BdRateSbKey, std::vector<std::vector<BdRateSample>>> perSuperblock;

  QString error;

  bool usable() const { return this->error.isEmpty() && !this->totals.empty(); }
};

/* Walks every frame of every stream, a slice at a time, on the GUI thread.
 *
 * Not a worker thread, and that is a measurement rather than a preference: a frame costs about
 * 25 ms at 1080p, so 8 streams over 300 frames is around a minute, and slicing it over a timer
 * keeps the window responsive without a second thread touching the decoders. The decoders are
 * already driven from this thread by the view, and two drivers on one decoder is a problem this
 * avoids rather than solves.
 *
 * One stream is finished before the next is started. The decoder is built for sequential access;
 * interleaving the streams frame by frame would make every step a seek.
 */
class BdRateSequenceSweeper : public QObject
{
  Q_OBJECT

public:
  explicit BdRateSequenceSweeper(QObject *parent = nullptr);

  //!< Restarts from the beginning, abandoning anything in flight.
  void start(const std::vector<BdRateGroup> &groups);
  void cancel();

  bool running() const { return this->active; }
  //!< 0..1, by (stream, frame) pairs done.
  double progress() const;
  //!< What has been gathered so far. Usable while running - a partial sweep still plots.
  const BdRateSequenceData &data() const { return this->result; }
  QString                   statusText() const;

signals:
  void progressed();
  void finished();

private:
  void step();

  std::vector<BdRateGroup> groups;
  BdRateSequenceData       result;

  bool active{};
  // Where the sweep is: which point of which group, and which frame of it.
  std::size_t groupCursor{};
  std::size_t pointCursor{};
  int         frameCursor{};
  int         frameFirst{};
  int         frameLast{};
  /* How many times the current frame has been retried because its SSE had not arrived. Bounded so
   * a frame whose statistics never turn up cannot stall the whole sweep.
   */
  int         frameRetries{};

  int totalSteps{};
  int stepsDone{};

  QTimer timer;
};

} // namespace bda::integration
