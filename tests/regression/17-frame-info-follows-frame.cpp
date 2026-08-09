// Regression: the pixel analysis must follow the displayed frame, not stay on the first one.
//
// The luma histogram and the per block pixel statistics both read videoHandler::currentFrameRawData.
// The view only loads those samples when it is drawing them, which it does at a high zoom factor,
// so on a normal zoom the buffer kept whatever frame happened to be loaded at startup: the histogram
// showed frame 0 forever and the block statistics sat on "computing...". Two things are needed and
// this checks both of them together:
//
//   * splitViewWidget::setRawValuesRequested() - the samples get loaded for every displayed frame
//     while the Frame Info pane is open, whatever the zoom.
//   * FrameInfoWidget must act on the view's second notification for the same frame. The first one
//     arrives before the frame is decoded, and the pane used to dismiss the second as "no change".
//
// Driven through MainWindow and PlaybackController so the whole chain is exercised, not the pieces.
#include <QApplication>
#include <QLabel>
#include <QSettings>
#include <iostream>
#include <unistd.h>

#include "playlistitem/playlistItem.h"
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

// The label carries the frame number only when the histogram actually has data.
QString histogramLabel(QMainWindow *w)
{
  for (auto *l : w->findChildren<QLabel *>())
    if (l->text().startsWith("Image attribute - luma histogram"))
      return l->text();
  return "(label not found)";
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 17-frame-info-follows-frame <video file>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  auto *w = new MainWindow(false);
  w->resize(1700, 1100);
  w->show();
  w->loadFiles({QString(argv[1])});
  settle(4000);

  std::cout << "after load: " << histogramLabel(w).toStdString() << std::endl;
  check(histogramLabel(w).contains("frame 0"), "the histogram is filled for the first frame");

  auto *playback = w->findChild<PlaybackController *>();
  auto *tree     = w->findChild<PlaylistTreeWidget *>();
  if (!playback || !tree || !tree->getSelectedItems()[0])
  {
    check(false, "the window came up with a selected item");
    std::cout << "FAIL" << std::endl;
    std::cout.flush();
    _exit(1);
  }
  auto *item = tree->getSelectedItems()[0];

  // Forwards, further forwards, then back: going back also has to re-read, not reuse frame 0.
  for (const int frameIdx : {5, 10, 2})
  {
    playback->setCurrentFrameAndUpdate(frameIdx);
    settle(3000);

    const auto label = histogramLabel(w);
    const auto bins  = item->getLumaHistogram(frameIdx);
    std::cout << "frame " << frameIdx << ": bins=" << bins.size() << "  label='"
              << label.toStdString() << "'" << std::endl;

    check(!bins.empty(),
          "the item has luma data for frame " + std::to_string(frameIdx) +
              " (the samples were loaded)");
    check(label.contains(QString("frame %1").arg(frameIdx)),
          "the histogram pane shows frame " + std::to_string(frameIdx));
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
