// Regression: the default grid must be the AV1 superblock size read from the sequence header.
//
// What this pins down:
//   * The superblock size only becomes known after the sequence header was pushed, so the item must
//     report 0 before the first frame is decoded and the real size afterwards. A one-shot "read it
//     when the item is selected" would always read 0.
//   * seq.sb128 == 0 must map to 64 and not to 128 (the test stream uses 64x64 superblocks, which is
//     what VQAnalyzer reports for it as "use 128x128 superblock = 0").
//   * No coding block in the statistics may be larger than the reported superblock size - that is
//     what makes the grid line up with the partitioning instead of cutting through blocks.
//   * A grid size the user picked from the menu must win over the bitstream default, and must keep
//     winning for the rest of the session.
#include <QApplication>
#include <QSettings>
#include <algorithm>
#include <iostream>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "ui/views/SplitViewWidget.h"

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
  // The largest coding block rect present in the collected statistics of the current frame.
  int largestBlockSize()
  {
    int largest = 0;
    for (const auto &type : this->statisticsData.getStatisticsTypes())
    {
      if (!this->statisticsData.hasDataForTypeID(type.typeID))
        continue;
      const auto data = this->statisticsData.getFrameTypeData(type.typeID);
      for (const auto &v : data.valueData)
      {
        largest = std::max(largest, static_cast<int>(v.size[0]));
        largest = std::max(largest, static_cast<int>(v.size[1]));
      }
    }
    return largest;
  }
};

// applyDefaultGridSizeFromItem is protected; the view is otherwise usable standalone because the
// default-grid path does not touch the playlist or playback pointers.
class ViewProbe : public splitViewWidget
{
public:
  using splitViewWidget::applyDefaultGridSizeFromItem;
  using splitViewWidget::fullCellGrid;
  using splitViewWidget::gridColor;
  using splitViewWidget::gridExtent;
  using splitViewWidget::regularGridSize;
  using splitViewWidget::setRegularGridSize;
};

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 10-superblock-grid-size <av1 file>" << std::endl;
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
    std::cout << "SKIP: only the dav1d decoder reads the sequence header" << std::endl;
    std::cout.flush();
    _exit(0);
  }

  // --- an item that does not report a size must not get a grid forced on it -------------------
  ViewProbe view;
  view.applyDefaultGridSizeFromItem(nullptr);
  check(view.regularGridSize == 0, "no grid is forced without an item");

  // The constructor already probes the file for the frame size, which pushes the sequence header, so
  // the superblock size is available without decoding a frame first. It stays 0 for decoders that do
  // not report it (FFmpeg), which is why the view retries instead of reading it once.
  check(item.getMaxBlockSize() == 64, "the superblock size is known right after construction");

  item.setBlockInfoRequested(true);
  item.loadFrame(3, false, true, false);
  const auto sbSize = item.getMaxBlockSize();
  std::cout << "superblock size   : " << sbSize << std::endl;
  check(sbSize == 64, "the test stream reports 64x64 superblocks (sb128 == 0)");

  const auto largest = item.largestBlockSize();
  std::cout << "largest block     : " << largest << std::endl;
  check(largest > 0, "statistics were collected");
  check(largest <= static_cast<int>(sbSize),
        "no coding block is larger than the reported superblock");

  view.applyDefaultGridSizeFromItem(&item);
  std::cout << "grid size         : " << view.regularGridSize << std::endl;
  check(view.regularGridSize == static_cast<int>(sbSize),
        "the grid defaults to the superblock size");
  check(view.gridColor() == QColor(0, 112, 255), "the bitstream grid uses the superblock colour");

  // --- the partial last superblock row/column is drawn at full size, like VQAnalyzer -----------
  // 176x144 with 64x64 superblocks is 2.75 x 2.25 cells, so 3x3 cells are drawn.
  check(view.fullCellGrid(), "the bitstream grid draws full cells");
  check(view.gridExtent(&item) == QSize(3, 3), "176x144 is covered by a 3x3 superblock grid");

  // --- a user choice wins and keeps winning ---------------------------------------------------
  view.setRegularGridSize(16, false, false);
  check(!view.fullCellGrid(), "a user grid stays inside the picture");
  check(view.regularGridSize == 16, "the user can override the default");
  check(view.gridColor() != QColor(0, 112, 255),
        "a user grid uses the configurable grid colour again");
  view.applyDefaultGridSizeFromItem(&item);
  check(view.regularGridSize == 16, "the bitstream default does not come back after a user choice");

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  // The decoder holds dlopen'ed libraries; skip global destructors like the other tests do.
  _exit(g_failures == 0 ? 0 : 1);
}
