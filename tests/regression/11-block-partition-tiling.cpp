// Regression: the coding blocks reported for a frame must tile the picture exactly once.
//
// AV1 partitions a superblock without gaps or overlaps, so every pixel is covered by exactly one
// coding block. Two real bugs in the dav1d traversal broke that:
//
//   * PARTITION_T_RIGHT_SPLIT (VERT_B) passed (x, y + o) for its second sub block, which is inside
//     the full height left block. That reported one block twice over and never reported the
//     right-top one - measured as 64px of overlap plus 64px of hole per occurrence. In the Block
//     Info pane it showed up as two "Pred Mode" rows for one block.
//   * cacheStatistics started the recursion at BL_128X128 no matter the real superblock size and
//     bounded its loops with a pixel count while stepping in 4x4 units, so each block was emitted
//     1, 2 or 4 times depending on its distance to the picture border.
//
// The tiling check catches both classes and any future partition-geometry mistake; the emission
// count check pins the second one specifically.
#include <QApplication>
#include <QSettings>
#include <iostream>
#include <map>
#include <tuple>
#include <unistd.h>
#include <vector>

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
} // namespace

class Probe : public playlistItemCompressedVideo
{
public:
  using playlistItemCompressedVideo::playlistItemCompressedVideo;
  decoder::DecoderEngine             engine() const { return this->decoderEngine; }
  std::vector<stats::StatsItemValue> predModeBlocks()
  {
    if (!this->statisticsData.hasDataForTypeID(0))
      return {};
    return this->statisticsData.getFrameTypeData(0).valueData;
  }
  // How many "Pred Mode" entries the Block Info pane would list for this position.
  size_t panelEntriesAt(const QPoint &pos, int frameIdx)
  {
    const auto info = this->getBlockInfoAt(pos, frameIdx);
    size_t     n    = 0;
    for (const auto &e : info.entries)
      if (e.typeID == 0)
        ++n;
    return n;
  }
};

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 11-block-partition-tiling <av1 file>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  Probe item(argv[1], 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
  std::cout << "engine            : "
            << std::string(decoder::DecoderEngineMapper.getName(item.engine())) << std::endl;
  if (item.engine() != decoder::DecoderEngine::Dav1d)
  {
    std::cout << "SKIP: needs the dav1d analyzer decoder for block statistics" << std::endl;
    std::cout.flush();
    _exit(0);
  }

  item.setBlockInfoRequested(true);
  const auto size = item.getSize();

  // Frames 10 and 20 of the generated test stream are the ones that used a VERT_B partition, but
  // scan a range so the test keeps its value if the encoder output shifts.
  const int firstFrame = 1;
  const int lastFrame  = 20;

  long totalHoles = 0, totalOverlaps = 0, totalDuplicateEmissions = 0;
  int  framesChecked = 0, worstPanelEntries = 0;

  for (int frame = firstFrame; frame <= lastFrame; frame++)
  {
    item.loadFrame(frame, false, true, false);
    const auto blocks = item.predModeBlocks();
    if (blocks.empty())
      continue;
    ++framesChecked;

    std::map<std::tuple<int, int, int, int>, int> rects;
    for (const auto &b : blocks)
      ++rects[{b.pos[0], b.pos[1], b.size[0], b.size[1]}];
    for (const auto &[rect, count] : rects)
      totalDuplicateEmissions += count - 1;

    std::vector<int> cover(size_t(size.width()) * size.height(), 0);
    for (const auto &[rect, count] : rects)
    {
      const auto [rx, ry, rw, rh] = rect;
      for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++)
          if (x >= 0 && y >= 0 && x < size.width() && y < size.height())
            ++cover[size_t(y) * size.width() + x];
    }
    for (const auto c : cover)
    {
      if (c == 0)
        ++totalHoles;
      else if (c > 1)
        ++totalOverlaps;
    }
  }

  // Sample what the pane would show. Every pixel of one frame is affordable at this resolution.
  item.loadFrame(10, false, true, false);
  for (int y = 0; y < size.height(); y += 2)
    for (int x = 0; x < size.width(); x += 2)
      worstPanelEntries = std::max(worstPanelEntries, int(item.panelEntriesAt(QPoint(x, y), 10)));

  std::cout << "frames checked    : " << framesChecked << std::endl;
  check(framesChecked > 5, "statistics were collected for several frames");
  check(totalHoles == 0, "no pixel is left uncovered by the coding blocks (holes=" +
                             std::to_string(totalHoles) + ")");
  check(totalOverlaps == 0, "no pixel is covered by two coding blocks (overlaps=" +
                                std::to_string(totalOverlaps) + ")");
  check(totalDuplicateEmissions == 0, "no block is emitted more than once (duplicates=" +
                                          std::to_string(totalDuplicateEmissions) + ")");
  check(worstPanelEntries == 1, "the block info pane lists exactly one Pred Mode per block (worst=" +
                                    std::to_string(worstPanelEntries) + ")");

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
