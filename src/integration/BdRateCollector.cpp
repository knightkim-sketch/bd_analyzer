#include "BdRateCollector.h"

#include <QPoint>
#include <QRect>

#include "playlistitem/playlistItemCompressedVideo.h"

namespace bda::integration
{

std::vector<bdrate::RatePoint> curveFor(const std::vector<BdRateSample> &points)
{
  std::vector<bdrate::RatePoint> curve;
  curve.reserve(points.size());
  for (const auto &sample : points)
    if (const auto psnr = sample.psnr(); psnr && sample.bits > 0.0)
      curve.push_back({sample.bits, *psnr});
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

} // namespace bda::integration
