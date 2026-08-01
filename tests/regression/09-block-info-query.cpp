// Regression: clicking a pixel must yield that pixel's block geometry and mode information,
// starting from the state a user is actually in — no statistic type switched on for rendering.
//
// Three things this pins down, all of which were wrong or absent before:
//   * StatisticsType::render defaults to false, so nothing is collected at all and the query is
//     empty. setBlockInfoRequested(true) has to make collection happen.
//   * getValuesAt() skips types whose renderGrid is off; the block query must not, because the pane
//     shows what the block is, not what is being drawn.
//   * The dav1d fork emits every block 2-4 times, so the query has to deduplicate, and it must
//     report the coding block and the (smaller) transform block separately.
#include <QApplication>
#include <QSettings>
#include <QThread>
#include <iostream>
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
} // namespace

class Probe : public playlistItemCompressedVideo
{
public:
  using playlistItemCompressedVideo::playlistItemCompressedVideo;
  decoder::DecoderEngine engine() const { return this->decoderEngine; }
  size_t                 nrStatTypes() { return this->statisticsData.getStatisticsTypes().size(); }
  size_t                 nrRenderedTypes()
  {
    size_t n = 0;
    for (const auto &t : this->statisticsData.getStatisticsTypes())
      if (t.render)
        ++n;
    return n;
  }
  // Mirrors what getValuesAt() would report, to show the two queries differ.
  int nrValuesAt(const QPoint &p) { return this->statisticsData.getValuesAt(p).size(); }
  stats::StatisticsTypesVec &types() { return this->statisticsData.getStatisticsTypes(); }
};

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 09-block-info-query <av1 file>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  Probe item(argv[1], 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
  std::cout << "engine            : "
            << std::string(decoder::DecoderEngineMapper.getName(item.engine())) << std::endl;
  std::cout << "declared types    : " << item.nrStatTypes() << std::endl;
  std::cout << "rendered types    : " << item.nrRenderedTypes() << std::endl;

  if (item.engine() != decoder::DecoderEngine::Dav1d)
  {
    std::cout << "SKIP: needs the dav1d analyzer decoder for block statistics" << std::endl;
    std::cout.flush();
    _exit(0);
  }

  const QPoint pos(80, 70);
  const int    frameIdx = 5;

  // --- default state: nothing is rendered, so nothing is collected -------------------------
  item.loadFrame(frameIdx, false, true, false);
  const auto before = item.getBlockInfoAt(pos, frameIdx);
  check(item.nrRenderedTypes() == 0, "no statistic type is rendered by default");
  check(!before.isValid, "query is empty until block info is requested");

  // --- request block info: collection must start without rendering anything -----------------
  item.setBlockInfoRequested(true);
  // setBlockInfoRequested only signals; the loading thread does the work. Here we drive it
  // directly, which is what the signal ends up doing.
  item.loadFrame(frameIdx, false, true, false);

  const auto info = item.getBlockInfoAt(pos, frameIdx);
  std::cout << "--- after requesting block info ---" << std::endl;
  std::cout << "rendered types    : " << item.nrRenderedTypes() << std::endl;
  std::cout << "isValid           : " << info.isValid << std::endl;
  std::cout << "frameIndex        : " << info.frameIndex << std::endl;
  std::cout << "coding block      : " << info.codingBlockRect.x() << "," << info.codingBlockRect.y()
            << " " << info.codingBlockRect.width() << "x" << info.codingBlockRect.height()
            << std::endl;
  if (info.transformBlockRect)
    std::cout << "transform block   : " << info.transformBlockRect->x() << ","
              << info.transformBlockRect->y() << " " << info.transformBlockRect->width() << "x"
              << info.transformBlockRect->height() << std::endl;
  else
    std::cout << "transform block   : (same as coding block)" << std::endl;
  std::cout << "distinct rects    : " << info.allRects.size() << std::endl;
  std::cout << "entries           : " << info.entries.size() << std::endl;
  for (const auto &e : info.entries)
    std::cout << "    [" << e.typeID << "] " << e.typeName.toStdString() << " = "
              << e.valueText.toStdString() << (e.isPrimaryRect ? "" : "   (other rect)")
              << std::endl;

  check(item.nrRenderedTypes() == 0, "collection did not switch any overlay on");
  check(info.isValid, "block info is available after requesting it");
  check(info.frameIndex == frameIdx, "block info belongs to the frame we asked for");
  check(info.codingBlockRect.contains(pos), "the coding block contains the clicked pixel");
  check(!info.entries.empty(), "at least one statistic type reports a value");

  // Deduplication: the fork emits each block 2-4x, so without dedupe we would see far more
  // entries than types. Every (typeID, rect) pair must be unique.
  bool uniquePairs = true;
  for (size_t i = 0; i < info.entries.size(); ++i)
    for (size_t j = i + 1; j < info.entries.size(); ++j)
      if (info.entries[i].typeID == info.entries[j].typeID &&
          info.entries[i].rect == info.entries[j].rect)
        uniquePairs = false;
  check(uniquePairs, "entries are deduplicated");
  check(info.entries.size() <= item.nrStatTypes(), "no more entries than declared types");

  // The transform block, when reported, must be strictly inside the coding block.
  if (info.transformBlockRect)
  {
    check(info.codingBlockRect.contains(*info.transformBlockRect),
          "transform block lies inside the coding block");
    check(*info.transformBlockRect != info.codingBlockRect,
          "transform block is only reported when it differs");
  }

  // Every entry must carry a rect that covers the click.
  bool allContain = true;
  for (const auto &e : info.entries)
    if (!e.rect.contains(pos))
      allContain = false;
  check(allContain, "every entry's rect contains the clicked pixel");

  // --- the block query must ignore renderGrid, unlike getValuesAt ---------------------------
  for (auto &t : item.types())
    t.renderGrid = false;
  const auto withoutGrid = item.getBlockInfoAt(pos, frameIdx);
  std::cout << "--- renderGrid forced off ---" << std::endl;
  std::cout << "getValuesAt entries : " << item.nrValuesAt(pos) << std::endl;
  std::cout << "getBlockInfoAt      : " << withoutGrid.entries.size() << std::endl;
  check(withoutGrid.isValid && withoutGrid.entries.size() == info.entries.size(),
        "block query is unaffected by renderGrid");

  // --- outside the picture ------------------------------------------------------------------
  const auto outside = item.getBlockInfoAt(QPoint(10000, 10000), frameIdx);
  check(!outside.isValid, "position outside the picture yields no block");

  // --- stale frame --------------------------------------------------------------------------
  const auto wrongFrame = item.getBlockInfoAt(pos, frameIdx + 1);
  check(!wrongFrame.isValid, "asking for a frame the statistics do not belong to yields nothing");

  std::cout << "RESULT: " << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
