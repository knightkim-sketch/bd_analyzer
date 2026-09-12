// Regression: what a compressed item says about its own length, and how much statistics one frame
// of it may produce.
//
// Two things that were wrong, both found by decoding a real 150 frame clip end to end:
//
//   * The FFmpeg path handed back the frame *count* as the last frame *index*, so a 150 frame clip
//     reported a range of 0..150 and "Num POCs 151". The extra index decodes nothing: seeking to it
//     shows no picture, and the BD-rate sweep spent its retry budget on it every time. The annex B
//     path in the same file already subtracted one.
//
//   * The transform size statistics are filled by stepping over the coding block in transform sized
//     steps, with the step read out of a 19 entry table indexed by a byte from the decoder's block
//     data. That byte is not always in range - the partition walk reaches positions whose Av1Block
//     was never written - and past the table lies whatever the linker put there. Read a zero and
//     the loop steps by zero and never ends, appending a statistics entry per iteration until
//     operator new throws. It showed up as std::bad_alloc part way through a sequence, not as a
//     hang, which is why the invariant checked here is a *bound*: no statistic type can hold more
//     entries than the picture has 4x4 blocks.
#include <QApplication>
#include <QSettings>
#include <iostream>
#include <string>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
void settle(int ms)
{
  for (int i = 0; i < ms / 10; ++i)
  {
    QCoreApplication::processEvents();
    usleep(10000);
  }
}

//!< Reaches the item's own statistics container, which is where the decoder writes.
class Probe : public playlistItemCompressedVideo
{
public:
  using playlistItemCompressedVideo::playlistItemCompressedVideo;
  stats::StatisticsData &stats() { return this->statisticsData; }
};
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 35-frame-range-and-statistics-bounds <stream>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  auto *item = new Probe(argv[1], 0, InputFormat::Libav, decoder::DecoderEngine::Dav1d);
  settle(500);
  item->setBlockInfoRequested(true);

  const auto range = item->properties().startEndRange;
  const auto size  = item->getRawFrameSize();
  std::cout << "  range " << range.first << ".." << range.second << ", " << size.width << "x"
            << size.height << std::endl;
  check(range.first == 0 && range.second > 0, "the item reports a frame range");

  /* The last index has to be a real frame, and one past it must not be. A decoded frame of an AV1
   * stream reports superblocks; the invented index reports none.
   */
  item->loadFrame(range.second, false, true, false);
  settle(300);
  const auto atLast = item->getSuperblockBits(range.second);
  std::cout << "  superblocks at the last index (" << range.second << "): " << atLast.size()
            << std::endl;
  check(!atLast.empty(), "the last index of the range is a frame that decodes");

  item->loadFrame(range.second + 1, false, true, false);
  settle(300);
  check(item->getSuperblockBits(range.second + 1).empty(),
        "and one past the end is not");

  /* Every frame, because the runaway needs a block the partition walk reaches but the decoder never
   * wrote, and which frame that is depends on the stream.
   */
  const auto blocks4x4 = std::size_t((size.width + 3) / 4) * std::size_t((size.height + 3) / 4);
  std::size_t worst = 0;
  int         worstFrame = -1, worstType = -1;
  for (int frameIdx = range.first; frameIdx <= range.second; ++frameIdx)
  {
    item->loadFrame(frameIdx, false, true, false);
    settle(10);
    auto &data = item->stats();
    for (const auto &type : data.getStatisticsTypes())
    {
      if (!data.hasDataForTypeID(type.typeID))
        continue;
      const auto entries = data.at(type.typeID).valueData.size();
      if (entries > worst)
      {
        worst      = entries;
        worstFrame = frameIdx;
        worstType  = type.typeID;
      }
    }
  }
  std::cout << "  most entries in one type: " << worst << " (frame " << worstFrame << ", type "
            << worstType << "), 4x4 blocks in the picture: " << blocks4x4 << std::endl;
  check(worst > 0, "statistics were collected at all");
  check(worst <= blocks4x4,
        "no statistic type holds more entries than the picture has 4x4 blocks");

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
