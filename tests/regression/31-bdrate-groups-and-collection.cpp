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

  std::vector<bda::integration::BdRateGroup>  groups;
  //!< Kept so the sequence checks below can compare against the single frame.
  std::vector<bda::integration::BdRateSample> collectedFrameTotals;

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
    collectedFrameTotals  = data.frameTotals.front();
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

  /* --- bits + 1 on the rate axis ---------------------------------------------------------------
   *
   * A skipped superblock costs 0 bits and log10(0) has no value, so without the convention every
   * skipped block drops out of its curve - and "reached that quality for nothing" is the most
   * interesting thing such a block has to say. Applied to every point, not only the zeros, so the
   * axis has no step in it.
   */
  {
    check(bda::integration::bdRateAxisRate(0.0) == 1.0, "zero bits sit at 1 on the rate axis");
    check(bda::integration::bdRateAxisRate(1000.0) == 1001.0, "and every other point shifts too");

    std::vector<bda::integration::BdRateSample> samples;
    samples.push_back({0.0, 4096.0, 64 * 64});    // skipped, but coded to some quality
    samples.push_back({500.0, 1024.0, 64 * 64});
    const auto curve = bda::integration::curveFor(samples);
    check(curve.size() == 2, "a zero-bit sample stays on the curve");
    check(!curve.empty() && curve.front().rate == 1.0, "at rate 1");

    // A slot that was never filled is not a superblock that cost nothing.
    std::vector<bda::integration::BdRateSample> untouched(2);
    check(bda::integration::curveFor(untouched).empty(),
          "while an untouched slot contributes nothing");

    // Lossless has no finite PSNR and cannot be placed on the axis at all.
    std::vector<bda::integration::BdRateSample> lossless;
    lossless.push_back({100.0, 0.0, 64 * 64});
    check(bda::integration::curveFor(lossless).empty(), "and a lossless block is dropped");
  }

  /* --- the sequence sweep ------------------------------------------------------------------------
   *
   * Every frame of every stream, sliced over the event loop rather than run on a worker: a frame
   * costs about 25 ms at 1080p (measured), so slicing keeps the window responsive without a second
   * thread driving the same decoders the view drives.
   */
  {
    bda::integration::BdRateSequenceSweeper sweeper;
    bool                                    finished = false;
    QObject::connect(&sweeper,
                     &bda::integration::BdRateSequenceSweeper::finished,
                     [&finished]() { finished = true; });

    sweeper.start(groups);
    check(sweeper.running() || finished, "the sweep starts");
    for (int i = 0; i < 600 && !finished; ++i)
      settle(100);
    check(finished, "and finishes");
    check(!sweeper.running(), "leaving nothing running");

    const auto &sequence = sweeper.data();
    check(sequence.error.isEmpty(), "with no error: " + sequence.error.toStdString());
    check(sequence.usable(), "and something to plot");
    check(sequence.lastFrame > sequence.firstFrame,
          "over a range of frames (" + std::to_string(sequence.firstFrame) + "-" +
              std::to_string(sequence.lastFrame) + ")");

    const auto seqCurve = bda::integration::curveFor(sequence.totals.front());
    check(int(seqCurve.size()) == nStreams, "a point per stream");
    for (const auto &point : seqCurve)
      std::cout << "        seq bits " << point.rate << "  psnr " << point.psnr << " dB"
                << std::endl;

    /* Accumulated, so the sequence total has to exceed any single frame of it - this is what
     * catches an accumulator that was reset per frame, or a sweep that only ever did one.
     */
    const auto frameCurve = bda::integration::curveFor(collectedFrameTotals);
    if (!frameCurve.empty() && !seqCurve.empty())
    {
      auto seqSorted = seqCurve;
      std::sort(seqSorted.begin(), seqSorted.end(), [](const auto &a, const auto &b) {
        return a.rate < b.rate;
      });
      auto frameSorted = frameCurve;
      std::sort(frameSorted.begin(), frameSorted.end(), [](const auto &a, const auto &b) {
        return a.rate < b.rate;
      });
      check(seqSorted.back().rate > frameSorted.back().rate,
            "the sequence costs more bits than the single frame inside it");
    }

    bool monotone = true;
    auto sorted   = seqCurve;
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
      return a.rate < b.rate;
    });
    for (std::size_t i = 1; i < sorted.size(); ++i)
      if (sorted[i].psnr < sorted[i - 1].psnr)
        monotone = false;
    check(monotone, "and it rises, like the frame curve does");

    check(!sequence.perSuperblock.empty(),
          "superblocks accumulated too (" + std::to_string(sequence.perSuperblock.size()) + ")");

    // Cancelling has to stop it rather than leave a timer running behind the window.
    sweeper.start(groups);
    sweeper.cancel();
    check(!sweeper.running(), "cancelling stops the sweep");
    const auto doneAfterCancel = sweeper.progress();
    settle(400);
    check(sweeper.progress() == doneAfterCancel, "and nothing advances afterwards");
  }

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
