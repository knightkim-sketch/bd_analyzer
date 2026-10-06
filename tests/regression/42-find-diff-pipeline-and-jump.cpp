// Regression: Find diff runs the whole pipeline and can put the application at what it found.
//
// Test 39 covers the header comparison and the refusals. This one covers what was wired after it:
//   * steps 3 and 4 run from the window - the first differing OBU payload, then the frame it codes
//     decoded on both sides and placed as SB(row, col) / MI(row, col);
//   * layer D compares the reconstructed pictures, and ticking it after a finished run adds only
//     that part;
//   * two byte-identical streams report no difference anywhere - the cheapest way to catch a
//     comparison that invents differences;
//   * jumping to a difference selects the streams in the order the Block info toggle names, moves
//     to the frame and selects the block, and the toggle re-shows the same place with the other
//     stream.
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QSettings>
#include <QTimer>

#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/StreamDiffWindow.h"
#include "playlistitem/playlistItemCompressedVideo.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/BlockInfoWidget.h"
#include "ui/widgets/PlaylistTreeWidget.h"

using bda::integration::StreamDiffWindow;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++failures;
}

void settle(int ms)
{
  for (int i = 0; i < ms / 10; ++i)
  {
    QCoreApplication::processEvents();
    usleep(10000);
  }
}

// The work runs on its own thread and reports back through a queued signal. A timeout rather than
// a wait: a hang must fail, not stall the suite.
bool waitForResult(StreamDiffWindow &window, int timeoutMs)
{
  QEventLoop loop;
  QTimer     poll;
  QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
  QObject::connect(&poll, &QTimer::timeout, [&] {
    if (!window.busy())
      loop.quit();
  });
  poll.start(50);
  loop.exec();
  return !window.busy();
}

QString blockPaneStatus(BlockInfoWidget *pane)
{
  for (auto *label : pane->findChildren<QLabel *>())
    if (label->text().startsWith("Frame "))
      return label->text();
  return {};
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3)
  {
    std::cerr << "usage: 42-find-diff-pipeline-and-jump <streamA> <streamB>\n";
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerFindDiffJump");
  QCoreApplication::setApplicationName("bdAnalyzerFindDiffJump");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  const QString pathA = QString::fromUtf8(argv[1]);
  const QString pathB = QString::fromUtf8(argv[2]);

  // ---- the pipeline, without a main window -----------------------------------------------------
  {
    playlistItemCompressedVideo itemA(pathA, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
    playlistItemCompressedVideo itemB(pathB, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
    StreamDiffWindow            window;

    check(window.compare({&itemA, &itemB}).ok(), "two different encodes are accepted");
    check(waitForResult(window, 120000), "steps 1-4 finish");
    const auto &report = window.lastReport();

    check(report.payloadCompared, "step 3 compared the OBU payloads");
    check(!report.payload.identical(), "and found a differing payload");
    if (const auto *first = report.payload.first())
      std::cout << "        first differing payload: " << first->a.label() << std::endl;

    check(report.blockCompared, "step 4 ran on the frame step 3 named");
    const auto *sb = report.blocks.firstWithBlockDiff();
    check(sb != nullptr, "and placed a superblock whose blocks differ");
    if (sb)
    {
      std::cout << "        frame " << report.blocks.frameIdx << ", SB(" << sb->sbRow << ", "
                << sb->sbCol << "), MI(" << sb->blocks.front().miRow << ", "
                << sb->blocks.front().miCol << ")" << std::endl;
      check(sb->blocks.front().miRow / (report.blocks.sbSize / 4) == sb->sbRow &&
                sb->blocks.front().miCol / (report.blocks.sbSize / 4) == sb->sbCol,
            "the first block lies inside the superblock it is reported under");
    }
    check(!report.reconCompared, "pictures are not compared unless asked - it is the slow part");

    const auto first = window.firstDifference();
    check(first.has_value() && first->pixel.has_value(),
          "'Go to first difference' points at a block");
    if (first && sb)
      check(first->frameIdx == report.blocks.frameIdx, "in the frame step 4 compared");

    // Tick D after the run: only the missing part runs, and the earlier answer stays.
    const auto blocksBefore = report.blocks.differing.size();
    auto *     recon        = window.findChild<QCheckBox *>();
    for (auto *box : window.findChildren<QCheckBox *>())
      if (box->text().startsWith("Compare reconstructed"))
        recon = box;
    check(recon != nullptr && recon->isEnabled(), "the picture comparison can be ticked");
    if (recon)
    {
      recon->setChecked(true);
      check(waitForResult(window, 120000), "the picture comparison finishes");
      check(report.reconCompared, "and compared the pictures");
      check(report.recon.frameCount > 0 &&
                int(report.recon.frames.size()) == report.recon.frameCount,
            "every display frame was compared");
      check(report.recon.firstDiffering() >= 0, "two encodes at different quality differ in pixels");
      check(report.blocks.differing.size() == blocksBefore,
            "ticking it later did not throw away step 4's answer");
    }
  }

  // ---- identical streams: nothing may be reported ---------------------------------------------
  {
    const QString copy = QDir::tempPath() + "/bd-find-diff-copy-" +
                         QString::number(QCoreApplication::applicationPid()) + ".ivf";
    QFile::remove(copy);
    check(QFile::copy(pathA, copy), "a byte-identical copy of A was written");

    playlistItemCompressedVideo itemA(pathA, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
    playlistItemCompressedVideo itemC(copy, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
    StreamDiffWindow            window;
    for (auto *box : window.findChildren<QCheckBox *>())
      if (box->text().startsWith("Compare reconstructed"))
        box->setChecked(true);
    // Ticking before a comparison has anything to compare starts nothing.
    check(!window.busy(), "ticking the picture comparison with no streams starts nothing");

    check(window.compare({&itemA, &itemC}).ok(), "a file and its copy are accepted");
    check(waitForResult(window, 120000), "the comparison finishes");
    const auto &report = window.lastReport();
    check(report.headers.identical(), "headers identical");
    check(report.payloadCompared && report.payload.identical(), "every payload identical");
    check(!report.blockCompared, "no frame to decode, so step 4 does not run");
    check(report.reconCompared && report.recon.firstDiffering() < 0,
          "every picture identical");
    check(!window.firstDifference().has_value(), "and there is no first difference to go to");
    QFile::remove(copy);
  }

  // ---- jumping in the main window ------------------------------------------------------------
  {
    auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
    w->resize(1400, 900);
    w->show();
    w->loadFiles({pathA, pathB});
    settle(2000);

    auto *tree     = w->findChild<PlaylistTreeWidget *>();
    auto *playback = w->findChild<PlaybackController *>();
    auto *pane     = w->findChild<BlockInfoWidget *>();
    const auto items = tree ? tree->getAllPlaylistItems() : QList<playlistItem *>();
    check(items.size() == 2 && playback && pane, "both streams are in the playlist");
    if (items.size() != 2 || !playback || !pane)
    {
      std::cout << "RESULT: FAIL" << std::endl;
      return 1;
    }
    playlistItem *a = items[0]->properties().name == pathA ? items[0] : items[1];
    playlistItem *b = (a == items[0]) ? items[1] : items[0];
    tree->setSelectedItems(a, b);
    settle(300);

    QAction *findDiff = nullptr;
    for (auto *action : w->findChildren<QAction *>())
      if (action->text().startsWith("Find di&ff"))
        findDiff = action;
    check(findDiff != nullptr, "the View menu has Find diff");
    if (findDiff)
      findDiff->trigger();
    auto *window = w->findChild<StreamDiffWindow *>();
    check(window != nullptr, "the action opened the window");
    if (!window)
    {
      std::cout << "RESULT: FAIL" << std::endl;
      return 1;
    }
    check(waitForResult(*window, 120000), "the comparison finishes");
    const auto where = window->firstDifference();
    check(where.has_value() && where->pixel.has_value(), "there is a block to go to");

    if (where && where->pixel)
    {
      window->setBlockInfoStream(0);
      window->locate(*where);
      settle(1500);
      check(tree->getSelectedItems()[0] == a && tree->getSelectedItems()[1] == b,
            "Block info A: A is the first selection, B stays selected beside it");
      check(playback->getCurrentFrame() == where->frameIdx, "the frame is the one found");
      const auto status = blockPaneStatus(pane);
      std::cout << "        Block Info: " << status.toStdString() << std::endl;
      check(status.startsWith(QString("Frame %1 ").arg(where->frameIdx)),
            "the Block Info pane shows a block of that frame");

      // The toggle re-shows the same place with the other stream, without another jump.
      window->setBlockInfoStream(1);
      settle(1500);
      check(tree->getSelectedItems()[0] == b && tree->getSelectedItems()[1] == a,
            "Block info B: B becomes the first selection");
      check(playback->getCurrentFrame() == where->frameIdx, "at the same frame");
      check(blockPaneStatus(pane).startsWith(QString("Frame %1 ").arg(where->frameIdx)),
            "and the pane still shows a block there");

      // Syntax info on the other stream must not disturb the block side.
      window->setSyntaxInfoStream(0);
      settle(500);
      check(tree->getSelectedItems()[0] == b, "the syntax toggle leaves the block stream alone");
    }
  }

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  // MainWindow is deliberately leaked (see test 21); skip a teardown it was never built for.
  _exit(failures == 0 ? 0 : 1);
}
