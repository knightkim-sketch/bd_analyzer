// Regression: UI Phase 3 (patch 0088) - frame timeline, shared block selection, Problems list,
// fit-to-frame colour range and the session file.
//
// Pinned here:
//   * The timeline has one bar per frame with the coded size the file says: for an IVF the bars add
//     up to the file minus its 32-byte header and 12 bytes per frame. A click goes to the frame;
//     a range adds up to the sum of its bars; with a second item the B difference is given.
//   * An Annex-B H.264 file gets sizes too (from its parser's frame ranges).
//   * A click in the view reaches the panels through one SelectionContext; a repeated click is
//     passed on, a jump to the same place is not.
//   * The Problems list turns a Find diff report into rows that lead to the frame and position.
//   * "Fit colour range to the frame" maps over the frame's own min..max and leaves the saved range.
//   * A .bdsession brings back the playlist and the perspective.
//
// Arguments: <av1 ivf> <annex-b h264>
#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QTreeWidget>
#include <iostream>
#include <numeric>
#include <string>
#include <unistd.h>

#include "integration/FrameTimeline.h"
#include "integration/Perspectives.h"
#include "integration/ProblemsWidget.h"
#include "integration/SelectionContext.h"
#include "statistics/StatisticsData.h"
#include "statistics/StatisticsDataPainting.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/views/SplitViewWidget.h"
#include "ui/widgets/PlaylistTreeWidget.h"

using namespace bda::integration;

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

qint64 sumBytes(const std::vector<FrameTimeline::Bar> &bars, int first, int last)
{
  qint64 sum = 0;
  for (int i = first; i <= last; ++i)
    sum += bars[i].bytes;
  return sum;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");
  if (argc < 3)
  {
    std::cout << "usage: " << argv[0] << " <av1 ivf> <annex-b h264>" << std::endl;
    return 2;
  }
  const QString ivf = argv[1], h264 = argv[2];

  // --- fit colour range to the frame (no window needed) -----------------------------------------
  {
    stats::StatisticsData data;
    data.setFrameSize(Size(64, 64));
    stats::StatisticsType type(1, "qindex", stats::color::ColorMapper({0, 255}, Color(0, 0, 0), Color(255, 255, 255)));
    type.hasValueData    = true;
    type.renderValueData = true;
    type.render          = true;
    data.addStatType(type);
    data.setFrameIndex(0);
    data[1].addBlockValue(0, 0, 32, 32, 100);
    data[1].addBlockValue(32, 0, 32, 32, 140);
    QImage   image(64, 64, QImage::Format_ARGB32);
    QPainter painter(&image);
    stats::paintStatisticsData(&painter, data, 0, 1.0);
    check(!data.getStatisticsTypes()[0].fittedRange, "a fixed range is not fitted");
    data.getStatisticsTypes()[0].fitRangeToFrame = true;
    stats::paintStatisticsData(&painter, data, 0, 1.0);
    const auto &fitted = data.getStatisticsTypes()[0];
    check(fitted.fittedRange && fitted.fittedRange->min == 100 && fitted.fittedRange->max == 140,
          "fit to frame uses the frame's min..max (100..140)");
    check(fitted.colorMapper.valueRange.min == 0 && fitted.colorMapper.valueRange.max == 255,
          "and leaves the saved range 0..255 alone");
    painter.end();
  }

  // --- Problems list from a Find diff report ----------------------------------------------------
  {
    ProblemsWidget   problems;
    StreamDiffReport report;
    report.blockCompared   = true;
    report.blocks.frameIdx = 3;
    report.blocks.sbSize   = 64;
    bda::diff::SbDiff sb;
    sb.sbRow = 1;
    sb.sbCol = 2;
    bda::diff::BlockDiff blk;
    blk.miRow = 18;
    blk.miCol = 33;
    blk.sbRow = 1;
    blk.sbCol = 2;
    blk.syntax.diffs.push_back({bda::diff::DiffKind{}, 0, 0, "mode", "NEWMV", "GLOBALMV"});
    sb.blocks.push_back(blk);
    sb.differingPositions = 4;
    report.blocks.differing.push_back(sb);
    report.reconCompared = true;
    ReconFrame frame;
    frame.frameIdx = 5;
    bda::diff::PlaneDiff luma;
    luma.differingSamples = 10;
    luma.firstX           = 7;
    luma.firstY           = 9;
    frame.diff.planes.push_back(luma);
    report.recon.frames.push_back(frame);
    problems.setDiffReport(report);
    check(problems.count() == 2, "one row for the differing SB and one for the differing frame");

    std::optional<StreamDiffLocation> asked;
    QObject::connect(&problems, &ProblemsWidget::locateRequested, [&](StreamDiffLocation where) {
      asked = where;
    });
    auto *list = problems.findChild<QTreeWidget *>("problemsList");
    emit list->itemActivated(list->topLevelItem(0), 0);
    check(asked && asked->frameIdx == 3 && asked->pixel == QPoint(33 * 4, 18 * 4),
          "the SB row leads to frame 3, MI(18, 33)");
    emit list->itemActivated(list->topLevelItem(1), 0);
    check(asked && asked->frameIdx == 5 && asked->pixel == QPoint(7, 9),
          "the frame row leads to frame 5, pixel (x, y) = (7, 9)");
    int itemA = 0, itemB = 0;
    problems.setDecoderError(&itemA, "a.ivf", "boom");
    check(problems.count() == 3, "a decoder error adds a row");
    auto *row = list->topLevelItem(2);
    problems.setDecoderError(&itemA, "a.ivf", "boom");
    check(problems.count() == 3 && list->topLevelItem(2) == row,
          "the same error again keeps its row (it is reported on every frame)");
    problems.setDecoderError(&itemB, "a.ivf", "boom");
    check(problems.count() == 4, "two items of the same name get a row each");
    problems.keepDecoderErrorsOf({&itemA});
    check(problems.count() == 3, "the row of an item that is gone is dropped");
    problems.setDecoderError(&itemA, "a.ivf", "");
    check(problems.count() == 2, "and clearing an error removes its row");
  }

  // --- the main window -------------------------------------------------------------------------
  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1600, 1000);
  w->show();
  QMetaObject::invokeMethod(w, "resetWindowLayout");
  w->loadFiles({ivf});
  settle(3000);

  auto *tree     = w->findChild<PlaylistTreeWidget *>();
  auto *timeline = w->findChild<FrameTimeline *>();
  auto *playback = w->findChild<PlaybackController *>();
  auto *view     = w->findChild<splitViewWidget *>("displaySplitView");
  check(timeline && playback && view, "the timeline, playback and view exist");
  check(w->findChild<QDockWidget *>("frameTimelineDock")->isVisible(), "the timeline dock is shown");
  if (!timeline || !playback || !view)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  const auto &bars = timeline->bars();
  const auto  n    = int(bars.size());
  check(n > 1 && n == playback->findChild<QSlider *>("frameSlider")->maximum() + 1,
        "one bar per frame (" + std::to_string(n) + ")");
  const auto total = sumBytes(bars, 0, n - 1);
  check(total + 32 + 12 * n == QFileInfo(ivf).size(),
        "the IVF bars add up to the file minus its headers");
  check(bars.front().type == FrameTimeline::Key, "the first frame is a key frame");

  emit timeline->seekRequested(n - 1);
  settle(500);
  check(playback->getCurrentFrame() == n - 1, "a timeline click goes to the frame");

  timeline->selectRange(1, n - 1);
  const auto summary = timeline->selectionSummary();
  check(summary && summary->bytes == sumBytes(bars, 1, n - 1),
        "a range adds up to the sum of its bars");
  check(summary && summary->largest >= 1 && summary->kbps > 0, "with its largest frame and bitrate");
  timeline->clearSelection();

  // --- the shared selection --------------------------------------------------------------------
  auto *selection = w->findChild<SelectionContext *>();
  const auto items = tree->getAllPlaylistItems();
  check(selection != nullptr && items.size() == 1, "one selection context");
  if (selection && items.size() == 1)
  {
    int emitted = 0;
    QObject::connect(selection, &SelectionContext::blockChanged, [&] { ++emitted; });
    emit view->blockSelected(items[0], QPoint(8, 8), 0);
    settle(200);
    check(selection->item() == items[0] && selection->pixel() == QPoint(8, 8) && emitted == 1,
          "a click in the view is the selection");
    emit view->blockSelected(items[0], QPoint(8, 8), 0);
    settle(100);
    check(emitted == 2, "the same click again is passed on (it brings the hex dump back)");
    selection->setBlock(items[0], QPoint(8, 8), 0, SelectionContext::Source::Jump);
    check(emitted == 2, "a jump to where the selection already is is not");
    selection->setBlock(items[0], QPoint(40, 8), 0, SelectionContext::Source::Jump);
    check(emitted == 3, "a jump elsewhere is");
  }

  // --- session ---------------------------------------------------------------------------------
  const auto sessionPath = QDir::temp().filePath("bda-regression-66.bdsession");
  QFile::remove(sessionPath);
  auto *perspectives = w->findChild<Perspectives *>();
  perspectives->apply("YUV compare");
  check(w->saveSession(sessionPath) && QFileInfo::exists(sessionPath), "Save Session writes the file");
  perspectives->apply("Default");
  tree->deletePlaylistItems(true);
  settle(300);
  check(tree->getAllPlaylistItems().isEmpty(), "the playlist is emptied");
  check(w->openSession(sessionPath), "Open Session reads it");
  settle(2000);
  const auto back = tree->getAllPlaylistItems();
  check(back.size() == 1 && QFileInfo(back[0]->properties().name).fileName() == QFileInfo(ivf).fileName(),
        "the item is back");
  check(perspectives->current() == "YUV compare", "and so is the perspective");
  check(w->findChild<QDockWidget *>("streamDiffDock")->isVisible(), "with its docks");
  QFile::remove(sessionPath);

  // --- Annex-B H.264 and a second item ---------------------------------------------------------
  tree->deletePlaylistItems(true);
  settle(300);
  w->loadFiles({h264});
  settle(3000);
  const auto &avcBars = timeline->bars();
  check(!avcBars.empty() && std::all_of(avcBars.begin(), avcBars.end(), [](const auto &b) {
          return b.bytes > 0;
        }),
        "an Annex-B H.264 file has a size for every frame (" + std::to_string(avcBars.size()) + ")");
  check(sumBytes(avcBars, 0, int(avcBars.size()) - 1) <= QFileInfo(h264).size(),
        "and they add up to no more than the file");

  // A copy: the playlist does not open the same file twice (28-playlist-no-duplicate-files).
  const auto copy = QDir::temp().filePath("bda-regression-66-copy.ivf");
  QFile::remove(copy);
  QFile::copy(ivf, copy);
  tree->deletePlaylistItems(true);
  settle(300);
  w->loadFiles({ivf, copy});
  settle(3000);
  const auto pair = tree->getAllPlaylistItems();
  if (pair.size() == 2)
  {
    timeline->currentSelectedItemsChanged(pair[0], pair[1]);
    timeline->selectRange(0, int(timeline->bars().size()) - 1);
    const auto ab = timeline->selectionSummary();
    check(ab && ab->diffPercentB && std::abs(*ab->diffPercentB) < 1e-9,
          "with a second item the B difference is given (0% for the same file)");
  }
  else
    check(false, "two items open");
  QFile::remove(copy);

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
