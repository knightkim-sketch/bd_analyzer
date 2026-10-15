// Regression: the Info pane's Frame tab - the OBU tab's information for the frame on screen.
//
// What this pins down, on the libaom test stream (25 frames, -g 10, hidden alternate references
// shown later with show_existing_frame at display frames 2, 4, 6, 12, 14, 16, 22):
//   * the tabs read File, Frame, OBU;
//   * the tab follows the displayed frame: "YUView frame index" is the frame number;
//   * coding_order is the decode order of coded frames, hidden ones included, so the shown frames
//     skip numbers (frame 1 is coding_order 4: three hidden ARFs are coded first in its unit);
//   * a show_existing_frame unit has coding_order "-" and the order_hint of the frame it shows;
//   * the OBU tree under the summary is the frame's own AVPacket.
//
// The numbers were cross-checked against odyssey's own trace (trace_param_gen/cs_frame_*.txt,
// coding_order and order_hint per coded frame) on an odyssey-encoded stream: all 40 coded frames
// and the 19 show_existing_frame units agree. That stream is not part of the test data, so the
// expectations here are the libaom stream's, read off the same code.
#include <QApplication>
#include <QLabel>
#include <QSettings>
#include <QTabWidget>
#include <QTreeView>
#include <iostream>
#include <unistd.h>

#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/FileInfoWidget.h"

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

QString value(QWidget *pane, const QString &key)
{
  auto *label = pane->findChild<QLabel *>("frameInfo:" + key);
  return label ? label->text() : QString("(no label)");
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 50-frame-info-tab <test.ivf>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1600, 1000);
  w->show();
  w->loadFiles({QString(argv[1])});
  settle(5000);

  auto *info     = w->findChild<FileInfoWidget *>();
  auto *tabs     = info ? info->findChild<QTabWidget *>() : nullptr;
  auto *playback = w->findChild<PlaybackController *>();
  auto *obuView  = info ? info->findChild<QTreeView *>("frameObuView") : nullptr;
  check(info && tabs && playback && obuView, "the info pane, its tabs and the playback controller");
  if (!info || !tabs || !playback || !obuView)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  check(tabs->count() == 3 && tabs->tabText(0) == "File" && tabs->tabText(1) == "Frame" &&
            tabs->tabText(2) == "OBU",
        "tabs are File, Frame, OBU");
  check(tabs->isTabEnabled(1), "the Frame tab is enabled once the stream is parsed");

  const auto show = [&](int frame) {
    playback->setCurrentFrameAndUpdate(frame);
    settle(600);
    std::cout << "  frame " << frame << ": index " << value(info, "YUView frame index").toStdString()
              << ", coding_order " << value(info, "coding_order").toStdString() << ", order_hint "
              << value(info, "order_hint").toStdString() << ", show_existing "
              << value(info, "show_existing_frame").toStdString() << std::endl;
  };

  show(0);
  check(value(info, "YUView frame index") == "0" && value(info, "coding_order") == "0" &&
            value(info, "order_hint") == "0" && value(info, "frame_type") == "KEY_FRAME",
        "frame 0: the key frame, coding_order 0, order_hint 0");

  show(1);
  check(value(info, "YUView frame index") == "1", "the tab follows the frame on screen");
  check(value(info, "coding_order") == "4" && value(info, "order_hint") == "1",
        "frame 1 is coding_order 4, order_hint 1 - after the hidden frames of its unit");
  check(value(info, "Hidden frames in this TU").contains("coding_order 1, order_hint 8"),
        "and lists the hidden ARF it carries (coding_order 1, order_hint 8)");

  show(2);
  check(value(info, "coding_order") == "-", "frame 2 (show_existing_frame) has coding_order \"-\"");
  check(value(info, "order_hint") == "2", "and the order_hint of the frame it shows, 2");
  check(value(info, "show_existing_frame").startsWith("1 (") &&
            value(info, "show_existing_frame").contains("coding_order 3"),
        "naming the coded frame it shows (coding_order 3)");

  show(13);
  check(value(info, "coding_order") == "16" && value(info, "order_hint") == "3",
        "frame 13 after the second key frame: coding_order 16, order_hint 3");

  // The tree under the summary is the frame's own packet.
  const auto root = obuView->rootIndex();
  check(root.isValid() && root.data().toString().contains("AVPacket 13"),
        "the OBU tree is rooted at AVPacket 13: " + root.data().toString().toStdString());

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
