#include "BdRateCollector.h"

#include <QPoint>
#include <limits>
#include <QRect>

#include "playlistitem/playlistItemCompressedVideo.h"

namespace bda::integration
{

std::vector<bdrate::RatePoint> curveFor(const std::vector<BdRateSample> &points)
{
  std::vector<bdrate::RatePoint> curve;
  curve.reserve(points.size());
  for (const auto &sample : points)
  {
    // sampleCount is what tells an untouched slot apart from a superblock that really cost nothing.
    if (sample.sampleCount <= 0)
      continue;
    if (const auto psnr = sample.psnr())
      curve.push_back({bdRateAxisRate(sample.bits), *psnr});
  }
  return curve;
}

BdRateFrameData collectBdRateFrame(const std::vector<BdRateGroup> &groups, int frameIdx)
{
  BdRateFrameData data;
  data.frameIdx = frameIdx;
  if (groups.empty())
  {
    data.error = "No groups added yet. Select the streams of one curve and press SB BD-rate.";
    return data;
  }
  if (frameIdx < 0)
  {
    data.error = "No frame is being shown.";
    return data;
  }

  const auto frameSize = groups.front().frameSize;
  const int  grid      = int(groups.front().superblockSize);
  if (grid <= 0)
  {
    data.error = "The superblock size is not known yet.";
    return data;
  }

  data.frameTotals.assign(groups.size(), {});

  for (std::size_t g = 0; g < groups.size(); ++g)
  {
    const auto &group = groups[g];
    data.frameTotals[g].assign(group.points.size(), {});

    for (std::size_t p = 0; p < group.points.size(); ++p)
    {
      auto *item = group.points[p].item;
      if (!item)
      {
        data.error = QString("\"%1\" lost one of its streams - it was removed from the playlist.")
                         .arg(group.name);
        return data;
      }

      /* sb_bitcount is only gathered while something asks for it, and nothing asks on a stream the
       * user is not looking at. Switching it on here is what makes the other groups' streams
       * report anything at all.
       */
      item->setBlockInfoRequested(true);

      /* Decode the frame on this stream, here, synchronously.
       *
       * Only the item the view is showing gets decoded by anything else, so without this the other
       * groups' streams report no bit counts at all - which is exactly how the first version of
       * this came back with one point out of four.
       *
       * On the GUI thread on purpose. loadFrame() carries a Q_ASSERT that it is not the main
       * thread (compiled out in this build), written for the caching threads it was designed for.
       * A frame costs about 25 ms at 1080p, measured, so a handful of streams is a few tenths of a
       * second and keeping the decoder on one thread avoids racing the view that is also driving
       * it. The sequence sweep will slice this over a timer for the same reason.
       *
       * emitSignals is off: a "loading complete" here would repaint the view, and a repaint that
       * leads back to a collection is the feedback loop the ME panel already had to be rescued
       * from.
       */
      item->loadFrame(frameIdx, false, true, false);

      const auto bits = item->getSuperblockBits(frameIdx);
      if (bits.empty())
      {
        /* Decoded but the statistics are not there. Happens while a frame cannot be decoded, and
         * resolves by asking again - which is what `pending` means; the window retries.
         */
        item->requestPixelStatistics(frameIdx);
        data.pending = true;
        continue;
      }

      item->requestPixelStatistics(frameIdx);

      BdRateSample total;
      for (const auto &superblock : bits)
      {
        const auto stats = item->getPixelBlockStats(superblock.rect.topLeft(), frameIdx);
        if (!stats || stats->sse < 0.0)
        {
          // The SSE for this frame is still being computed. Come back rather than report a total
          // that is missing blocks.
          data.pending = true;
          continue;
        }

        /* Clipped to the picture: a superblock at the right or bottom edge covers fewer samples,
         * and dividing its error over the nominal 64x64 would report a PSNR it did not earn.
         */
        const auto clipped =
            superblock.rect.intersected(QRect(0, 0, int(frameSize.width), int(frameSize.height)));
        const auto samples = std::int64_t(clipped.width()) * clipped.height();
        if (samples <= 0)
          continue;

        const BdRateSample sample{double(superblock.bits), stats->sse, samples};

        const BdRateSbKey key{superblock.rect.left() / grid, superblock.rect.top() / grid};
        auto             &cell = data.perSuperblock[key];
        if (cell.empty())
          cell.assign(groups.size(), {});
        if (cell[g].empty())
          cell[g].assign(group.points.size(), {});
        cell[g][p] = sample;

        total.bits += sample.bits;
        total.sse = (total.sse < 0.0 ? 0.0 : total.sse) + sample.sse;
        total.sampleCount += sample.sampleCount;
      }
      data.frameTotals[g][p] = total;
    }
  }

  return data;
}

// ---------------------------------------------------------------------------------------------
// BdRateSequenceSweeper
// ---------------------------------------------------------------------------------------------

BdRateSequenceSweeper::BdRateSequenceSweeper(QObject *parent) : QObject(parent)
{
  /* Zero interval: the slice runs whenever the event loop is otherwise idle, so the sweep goes as
   * fast as it can while the window stays responsive. One frame per tick keeps the longest block
   * of work at about 25 ms, which is below what reads as a freeze.
   */
  this->timer.setInterval(0);
  connect(&this->timer, &QTimer::timeout, this, &BdRateSequenceSweeper::step);
}

void BdRateSequenceSweeper::start(const std::vector<BdRateGroup> &groups)
{
  this->cancel();

  this->groups = groups;
  this->result = {};
  if (this->groups.empty())
  {
    this->result.error = "No groups to sweep.";
    emit this->finished();
    return;
  }

  /* The range every stream has in common. They are encodes of one clip so they should agree, but a
   * truncated encode would otherwise be read as "this stream cost nothing" for the missing frames.
   */
  int first = std::numeric_limits<int>::min();
  int last  = std::numeric_limits<int>::max();
  int points = 0;
  for (const auto &group : this->groups)
    for (const auto &point : group.points)
    {
      if (!point.item)
      {
        this->result.error =
            QString("\"%1\" lost one of its streams.").arg(group.name);
        emit this->finished();
        return;
      }
      const auto range = point.item->properties().startEndRange;
      first            = std::max(first, range.first);
      last             = std::min(last, range.second);
      ++points;
    }

  if (points == 0 || last < first)
  {
    this->result.error = "The selected streams have no frames in common.";
    emit this->finished();
    return;
  }

  this->frameFirst        = first;
  this->frameLast         = last;
  this->result.firstFrame = first;
  this->result.lastFrame  = last;
  this->result.totals.assign(this->groups.size(), {});
  for (std::size_t g = 0; g < this->groups.size(); ++g)
    this->result.totals[g].assign(this->groups[g].points.size(), {});

  this->groupCursor  = 0;
  this->pointCursor  = 0;
  this->frameCursor  = first;
  this->frameRetries = 0;
  this->totalSteps   = points * (last - first + 1);
  this->stepsDone    = 0;
  this->active       = true;
  this->timer.start();
}

void BdRateSequenceSweeper::cancel()
{
  this->timer.stop();
  this->active = false;
}

double BdRateSequenceSweeper::progress() const
{
  if (this->totalSteps <= 0)
    return 0.0;
  return std::min(1.0, double(this->stepsDone) / double(this->totalSteps));
}

QString BdRateSequenceSweeper::statusText() const
{
  if (!this->result.error.isEmpty())
    return this->result.error;
  if (this->totalSteps <= 0)
    return "Nothing swept yet.";
  if (this->active)
  {
    const auto &group = this->groups[std::min(this->groupCursor, this->groups.size() - 1)];
    return QString("Sweeping frames %1-%2: %3%, on \"%4\"")
        .arg(this->frameFirst)
        .arg(this->frameLast)
        .arg(int(this->progress() * 100.0))
        .arg(group.name);
  }
  return QString("Swept frames %1-%2 (%3 of %4 stream-frames).")
      .arg(this->frameFirst)
      .arg(this->frameLast)
      .arg(this->stepsDone)
      .arg(this->totalSteps);
}

void BdRateSequenceSweeper::step()
{
  if (!this->active)
    return;

  if (this->groupCursor >= this->groups.size())
  {
    this->cancel();
    emit this->finished();
    return;
  }

  const auto &group = this->groups[this->groupCursor];
  if (this->pointCursor >= group.points.size())
  {
    ++this->groupCursor;
    this->pointCursor  = 0;
    this->frameCursor  = this->frameFirst;
    this->frameRetries = 0;
    return;
  }

  auto *item = group.points[this->pointCursor].item;
  if (!item)
  {
    this->result.error = QString("\"%1\" lost one of its streams mid sweep.").arg(group.name);
    this->cancel();
    emit this->finished();
    return;
  }

  item->setBlockInfoRequested(true);
  item->loadFrame(this->frameCursor, false, true, false);
  item->requestPixelStatistics(this->frameCursor);

  const auto bits  = item->getSuperblockBits(this->frameCursor);
  const auto grid  = int(group.superblockSize);
  const auto width = int(group.frameSize.width);
  const auto height = int(group.frameSize.height);

  bool ready = !bits.empty();
  if (ready)
  {
    // Every superblock needs its SSE before this frame can be added, or the totals would be short.
    for (const auto &superblock : bits)
    {
      const auto stats = item->getPixelBlockStats(superblock.rect.topLeft(), this->frameCursor);
      if (!stats || stats->sse < 0.0)
      {
        ready = false;
        break;
      }
    }
  }

  if (!ready)
  {
    /* The SSE is computed off this thread, so the first look at a frame usually misses it. Come
     * back on the next tick rather than blocking - but not forever: a frame that cannot be decoded
     * would otherwise stop the sweep here.
     */
    if (++this->frameRetries < 200)
      return;
    this->frameRetries = 0;
    ++this->frameCursor;
    ++this->stepsDone;
    if (this->frameCursor > this->frameLast)
    {
      ++this->pointCursor;
      this->frameCursor = this->frameFirst;
    }
    emit this->progressed();
    return;
  }

  for (const auto &superblock : bits)
  {
    const auto stats = item->getPixelBlockStats(superblock.rect.topLeft(), this->frameCursor);
    const auto clipped = superblock.rect.intersected(QRect(0, 0, width, height));
    const auto samples = std::int64_t(clipped.width()) * clipped.height();
    if (samples <= 0 || !stats)
      continue;

    const BdRateSbKey key{superblock.rect.left() / grid, superblock.rect.top() / grid};
    auto             &cell = this->result.perSuperblock[key];
    if (cell.empty())
    {
      cell.assign(this->groups.size(), {});
      for (std::size_t g = 0; g < this->groups.size(); ++g)
        cell[g].assign(this->groups[g].points.size(), {});
    }

    /* Accumulated over the range: bits summed, SSE summed, samples summed. The PSNR is then taken
     * from the totals, which is the sequence PSNR - averaging the per frame dB values instead is a
     * different and wrong number.
     */
    auto &slot = cell[this->groupCursor][this->pointCursor];
    slot.bits += double(superblock.bits);
    slot.sse = (slot.sse < 0.0 ? 0.0 : slot.sse) + stats->sse;
    slot.sampleCount += samples;

    auto &total = this->result.totals[this->groupCursor][this->pointCursor];
    total.bits += double(superblock.bits);
    total.sse = (total.sse < 0.0 ? 0.0 : total.sse) + stats->sse;
    total.sampleCount += samples;
  }

  this->frameRetries = 0;
  ++this->frameCursor;
  ++this->stepsDone;
  if (this->frameCursor > this->frameLast)
  {
    ++this->pointCursor;
    this->frameCursor = this->frameFirst;
  }

  // Redrawing on every frame would spend the sweep on painting; once a second is plenty to watch.
  if (this->stepsDone % 25 == 0)
    emit this->progressed();
}

} // namespace bda::integration
