// Regression: a playlist selection becomes a BD-rate curve, and one frame of it collects.
//
// The maths is unit tested on its own (tests/unit/bdrate-math.cpp). This is the half that touches
// the playlist and the decoder, and every check here came from something that was wrong first:
//
//   * getSelectedItems() caps at two, so a group of four streams silently became two. A separate
//     n-item accessor exists for this.
//   * A stream only reports sb_bitcount for a frame it has decoded, and only the item the view is
//     showing gets decoded by anything else. Without the collector driving loadFrame() itself, a
//     four point group came back with one point - measured, not hypothetical.
//   * A superblock at the right or bottom edge covers fewer samples than the nominal 64x64, and
//     dividing its error over the nominal area reports a PSNR it did not earn.
//   * A raw item in the selection is the original, not another curve point.
//   * A single stream, a stream with no bit counts, and mismatched geometry are refused with a
//     reason rather than producing a curve that looks usable.
#include <QApplication>
#include <QFileInfo>
#include <QSettings>
#include <iostream>
#include <string>
#include <algorithm>
#include <unistd.h>

#include "integration/BdRateCollector.h"
#include "integration/BdRateGroups.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/PlaylistTreeWidget.h"

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
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 4)
  {
    // The harness passes the original plus at least two encodes of it.
    std::cout << "SKIP: needs <org.y4m> <a.ivf> <b.ivf> [c.ivf]" << std::endl;
    std::cout.flush();
    _exit(0);
  }

  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().remove("Autosaveplaylist");

  const QString org = argv[1];
  QStringList   files;
  for (int i = 2; i < argc; ++i)
    files << argv[i];
  const int nStreams = files.size();

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1200, 800);
  w->show();
  settle(500);
  w->loadFiles(QStringList(files) << org);
  settle(6000);

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  check(tree != nullptr, "the playlist came up");
  if (!tree)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
    tree->topLevelItem(i)->setSelected(true);
  settle(500);

  // --- n items, not two -------------------------------------------------------------------------
  {
    const auto all = tree->getAllSelectedItems();
    check(all.size() == nStreams + 1,
          "every selected item comes back (" + std::to_string(all.size()) + ")");
    check(tree->getSelectedItems()[1] != nullptr,
          "while the split view's two-item accessor still works");
  }

  std::vector<bda::integration::BdRateGroup> groups;

  // --- the selection becomes a group ------------------------------------------------------------
  {
    const auto result = bda::integration::makeBdRateGroup(tree->getAllSelectedItems(), groups);
    check(result.ok(), "the selection becomes a group: " + result.message.toStdString());
    if (!result.ok())
    {
      std::cout << "FAIL" << std::endl;
      return 1;
    }
    check(int(result.group.points.size()) == nStreams,
          "with one point per stream, and the raw file not among them");
    check(QFileInfo(result.group.originalPath).canonicalFilePath() ==
              QFileInfo(org).canonicalFilePath(),
          "the raw file in the selection is taken as the original");
    check(result.group.superblockSize > 0 && result.group.frameSize.width > 0,
          "and the grid is recorded, which is what later groups must match");
    check(!result.group.name.isEmpty(), "the group gets a name: " + result.group.name.toStdString());
    groups.push_back(result.group);
  }

  // --- a second group, and the refusals ---------------------------------------------------------
  {
    const auto second = bda::integration::makeBdRateGroup(tree->getAllSelectedItems(), groups);
    check(second.ok(), "a second selection becomes a second group");
    check(second.group.name != groups.front().name, "with a name of its own");

    QList<playlistItem *> single;
    single << tree->getAllSelectedItems().first();
    const auto one = bda::integration::makeBdRateGroup(single, {});
    check(one.error == bda::integration::BdRateGroupError::OnePointOnly,
          "one stream is a point, not a curve");
    check(!one.message.isEmpty(), "and it says so: " + one.message.toStdString());

    QList<playlistItem *> rawOnly;
    for (auto *item : tree->getAllSelectedItems())
      if (item->properties().name == org)
        rawOnly << item;
    const auto none = bda::integration::makeBdRateGroup(rawOnly, {});
    check(none.error == bda::integration::BdRateGroupError::NoStreams,
          "the original on its own is not a curve either");
  }

  // --- one frame collects for every point ------------------------------------------------------
  {
    auto *playback = w->findChild<PlaybackController *>();
    const int frameIdx = 2;
    if (playback)
      playback->setCurrentFrameAndUpdate(frameIdx);
    settle(1500);

    bda::integration::BdRateFrameData data;
    for (int attempt = 0; attempt < 80; ++attempt)
    {
      data = bda::integration::collectBdRateFrame(groups, frameIdx);
      if (!data.error.isEmpty() || !data.pending)
        break;
      settle(250);
    }
    check(data.error.isEmpty(), "collection reports no error: " + data.error.toStdString());
    check(!data.pending, "and finishes rather than staying pending");
    check(!data.perSuperblock.empty(), "superblocks were collected (" +
                                           std::to_string(data.perSuperblock.size()) + ")");

    /* The regression: every point of the group has to contribute, not just the stream the view
     * happens to be showing.
     */
    const auto frameCurve = bda::integration::curveFor(data.frameTotals.front());
    check(int(frameCurve.size()) == nStreams,
          "the frame curve has a point per stream (" + std::to_string(frameCurve.size()) + " of " +
              std::to_string(nStreams) + ")");

    /* A rate-distortion curve rises: more bits, more quality. Sorted by rate, the PSNRs must not
     * go down - if they do, the streams were paired with the wrong frames or the SSE came from
     * somewhere else.
     */
    auto sorted = frameCurve;
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
      return a.rate < b.rate;
    });
    bool monotone = true;
    for (std::size_t i = 1; i < sorted.size(); ++i)
      if (sorted[i].psnr < sorted[i - 1].psnr)
        monotone = false;
    check(monotone, "and it rises: more bits buy more PSNR");
    for (const auto &point : sorted)
      std::cout << "        bits " << point.rate << "  psnr " << point.psnr << " dB" << std::endl;

    // A group against itself is 0% by construction. This checks the wiring, not the maths.
    const auto self = bda::bdrate::bdRate(frameCurve, frameCurve);
    check(self.ok() && std::abs(self.percent) < 1e-6, "a group against itself is 0%");

    /* Edge superblocks: the sample count must be the clipped area, so the right and bottom columns
     * carry fewer samples than a full block when the picture is not a multiple of the grid.
     */
    const auto grid  = int(groups.front().superblockSize);
    const auto width = int(groups.front().frameSize.width);
    const auto height = int(groups.front().frameSize.height);
    if (width % grid != 0 || height % grid != 0)
    {
      std::int64_t nominal = 0, clipped = 0;
      for (const auto &[key, cell] : data.perSuperblock)
      {
        const auto &sample = cell.front().front();
        if (sample.sampleCount <= 0)
          continue;
        if (key.first * grid + grid > width || key.second * grid + grid > height)
          ++clipped;
        else
          ++nominal;
        if (sample.sampleCount == std::int64_t(grid) * grid)
          continue;
        // A partial block must be exactly the overlap with the picture.
        const auto w = std::min(grid, width - key.first * grid);
        const auto h = std::min(grid, height - key.second * grid);
        check(sample.sampleCount == std::int64_t(w) * h,
              "an edge superblock counts only the samples inside the picture");
        break;
      }
      check(clipped > 0, "the picture is not a multiple of the grid, so there are edge blocks");
    }
    else
      std::cout << "        (the picture is a multiple of the superblock grid - no edge case here)"
                << std::endl;
  }

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
