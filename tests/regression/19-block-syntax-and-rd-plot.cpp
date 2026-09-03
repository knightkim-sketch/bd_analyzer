// Regression: the panes around an AV1 item with an original YUV attached.
//
// The bug this starts from: after loading an original, the SSE kept updating while the block syntax
// pane sat on the frame that had been clicked. The pane was wired to clicks and selection changes but
// not to the view's frame change, unlike the pixel side of the same click (see
// 17-frame-info-follows-frame). So this checks the three things that have to hold together:
//
//   * The block syntax pane re-reads the bitstream syntax of the clicked position for the frame on
//     screen, with no new click.
//   * The Rec/Org selector switches what the viewer draws, and switching it does NOT change the
//     numbers: the SSE compares the reconstruction against the original either way.
//   * The rate/distortion plot gets one point per superblock, x from the bitstream (sb_bitcount) and
//     y from the pixels (SSE) - the two halves come from different subsystems and must line up on the
//     same frame.
#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QTreeWidget>
#include <iostream>
#include <unistd.h>

#include "playlistitem/playlistItem.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/views/SplitViewWidget.h"
#include "ui/widgets/BlockInfoWidget.h"
#include "ui/widgets/PlaylistTreeWidget.h"
#include "ui/widgets/RateDistortionPlotWidget.h"

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
// The pane's status line ("Frame N · pixel (x, y)") plus its first syntax row.
QString blockPaneState(QWidget *w)
{
  auto *pane = w->findChild<BlockInfoWidget *>();
  if (!pane)
    return "(no pane)";
  QString status;
  for (auto *l : pane->findChildren<QLabel *>())
    if (!l->text().isEmpty())
    {
      status = l->text();
      break;
    }
  QString firstValue;
  if (auto *tree = pane->findChild<QTreeWidget *>(); tree && tree->topLevelItemCount() > 0)
  {
    auto *top  = tree->topLevelItem(0);
    firstValue = top->text(0);
    if (top->childCount() > 0)
      firstValue += " / " + top->child(0)->text(0) + "=" + top->child(0)->text(1);
  }
  return status + " | " + firstValue;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3)
  {
    std::cerr << "usage: 19-block-syntax-and-rd-plot <stream> <original.yuv>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  auto *w = new MainWindow(false);
  w->resize(1700, 1100);
  w->show();
  w->loadFiles({QString(argv[1])});
  settle(5000);

  auto *playback = w->findChild<PlaybackController *>();
  auto *tree     = w->findChild<PlaylistTreeWidget *>();
  auto *view     = w->findChild<splitViewWidget *>();
  auto *plot     = w->findChild<RateDistortionPlotWidget *>();
  auto *combo    = w->findChild<QComboBox *>("displaySourceComboBox");
  auto *item     = tree ? tree->getSelectedItems()[0] : nullptr;
  check(item && view && plot && combo && playback, "the window came up with all the panes");
  if (!(item && view && plot && combo && playback))
  {
    std::cout << "FAIL" << std::endl;
    std::cout.flush();
    _exit(1);
  }

  QString error;
  check(item->setOriginalYUVSource(QString(argv[2]), &error),
        "the original YUV is accepted: " + error.toStdString());
  // What the Load Org YUV button does after attaching, and what a selection change does.
  w->currentSelectedItemsChanged(item, nullptr);
  settle(500);

  // --- the block syntax pane follows the displayed frame ----------------------------------------
  playback->setCurrentFrameAndUpdate(2);
  settle(2500);
  {
    // A click in the middle of the view: press and release without moving selects a block.
    const auto  pos    = view->rect().center();
    const auto  global = view->mapToGlobal(pos);
    QMouseEvent press(
        QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(
        QEvent::MouseButtonRelease, pos, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view, &press);
    QApplication::sendEvent(view, &release);
  }
  settle(2500);
  const auto atFrame2 = blockPaneState(w);
  std::cout << "    frame 2: " << atFrame2.toStdString() << std::endl;
  check(atFrame2.contains("Frame 2"), "the block syntax pane shows the clicked frame");

  playback->setCurrentFrameAndUpdate(6);
  settle(3000);
  const auto atFrame6 = blockPaneState(w);
  std::cout << "    frame 6: " << atFrame6.toStdString() << std::endl;
  check(atFrame6.contains("Frame 6"),
        "the block syntax pane followed the displayed frame without a new click");
  check(atFrame2 != atFrame6, "and its contents changed with the frame");

  // --- the rate/distortion plot -----------------------------------------------------------------
  settle(3000);
  plot->refresh();
  settle(2000);
  const auto superblocks = item->getSuperblockBits(6);
  std::cout << "    superblocks with a bit count: " << superblocks.size() << std::endl;
  check(!superblocks.empty(), "the bitstream reports a bit count per superblock");
  auto withSse = 0u, withBits = 0u;
  for (const auto &superblock : superblocks)
  {
    if (superblock.bits > 0)
      ++withBits;
    const auto stats = item->getPixelBlockStats(superblock.rect.topLeft(), 6);
    if (stats && stats->sse >= 0.0)
      ++withSse;
  }
  std::cout << "    with bits: " << withBits << ", with SSE: " << withSse << std::endl;
  check(withBits > 0 && withSse == superblocks.size(),
        "every superblock has both coordinates of the plot");

  // The plot must actually draw them, not just be able to.
  QImage plotImage(plot->size(), QImage::Format_ARGB32);
  plot->render(&plotImage);
  const auto background  = plot->palette().color(QPalette::Base);
  auto       drawnPixels = 0;
  for (int y = 0; y < plotImage.height(); y++)
    for (int x = 0; x < plotImage.width(); x++)
      if (plotImage.pixelColor(x, y) != background)
        ++drawnPixels;
  check(drawnPixels > 200, "the plot pane draws axes and points");

  // --- the Rec/Org selector ---------------------------------------------------------------------
  check(combo->isEnabled(), "the Rec/Org selector is enabled once an original is attached");
  check(combo->count() == 2 && combo->itemText(0) == "Rec" && combo->itemText(1) == "Org",
        "it offers Rec and Org");
  check(item->getDisplaySource() == playlistItem::DisplaySource::Reconstruction,
        "it starts on the reconstruction");

  const auto renderView = [&]
  {
    settle(2000);
    QImage image(view->size(), QImage::Format_ARGB32);
    view->render(&image);
    return image;
  };
  const auto recImage = renderView();
  const auto sseOnRec = item->getPixelBlockStats(QPoint(70, 70), 6);

  combo->setCurrentIndex(1);
  settle(1000);
  check(item->getDisplaySource() == playlistItem::DisplaySource::Original,
        "picking Org switches the item's display source");
  const auto orgImage = renderView();
  check(recImage != orgImage, "the viewer draws the original instead of the reconstruction");

  const auto sseOnOrg = item->getPixelBlockStats(QPoint(70, 70), 6);
  check(sseOnRec && sseOnOrg && sseOnRec->sse == sseOnOrg->sse,
        "the SSE keeps comparing reconstruction against original while Org is shown");

  combo->setCurrentIndex(0);
  settle(1000);
  check(item->getDisplaySource() == playlistItem::DisplaySource::Reconstruction &&
            renderView() == recImage,
        "picking Rec brings the reconstruction back");

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
