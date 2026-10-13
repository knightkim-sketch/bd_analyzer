// Regression: a side-by-side view comes back from the saved playlist with its split point.
//
// The playlist snapshot stored the split mode but never the split point: ViewStateHandler wrote
// the point only when `splitting` was set, and getViewState() never sets it. Restoring read the
// missing point as 0, so the split sat on the left edge, the first view - the one with the item -
// had no width, and the whole view showed plain background. Reported as "the YUV image is not
// shown" on a 4K stream; any item and resolution do the same.
//
// What this pins down:
//   * the split point survives a quit and a restart, and the restored view draws the picture;
//   * a snapshot saved before the fix (split mode, no point) restores with the split in the middle
//     instead of at 0 - existing settings recover without anyone editing them.
#include <QApplication>
#include <QDomDocument>
#include <QImage>
#include <QSet>
#include <QSettings>
#include <iostream>
#include <string>
#include <unistd.h>

#include "ui/Mainwindow.h"
#include "ui/views/SplitViewWidget.h"
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
splitViewWidget *mainView(MainWindow *w)
{
  return w->findChild<splitViewWidget *>("displaySplitView");
}
// Distinct colours in the left third of the view - where the first (item) view sits for a split
// anywhere right of it. Plain background is one colour.
int coloursOnTheLeft(splitViewWidget *view)
{
  const QImage g = view->grab().toImage();
  QSet<QRgb>   colours;
  for (int y = g.height() / 4; y < 3 * g.height() / 4; y += 3)
    for (int x = 2; x < g.width() / 4; x += 3)
      colours.insert(g.pixel(x, y));
  return colours.size();
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 44-split-view-state-restored <raw.yuv>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerSplitState");
  QCoreApplication::setApplicationName("bdAnalyzerSplitState");
  QSettings().clear();
  // No derived-data cache, so closing the window opens no "delete the cache?" dialog.
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  // ---- session 1: split side by side at 0.3 and quit -----------------------------------------
  // MainWindows are never deleted here - see 21-playlist-saved-across-sessions.
  auto *w = new MainWindow(false);
  w->resize(1400, 900);
  w->show();
  w->loadFiles({QString(argv[1])});
  settle(2000);
  auto *view = mainView(w);
  check(view != nullptr, "the main view is found");
  if (!view)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }
  view->setViewState(QPointF(0, 0), 1.0, 0.3, 1);
  settle(500);
  w->close();
  settle(500);
  check(QSettings().contains("Autosaveplaylist"), "quitting saved the playlist");

  // ---- session 2: the split point comes back --------------------------------------------------
  {
    auto *w2 = new MainWindow(false);
    w2->resize(1400, 900);
    w2->show();
    settle(2500);
    QPointF offset;
    double  zoom = 0, split = 0;
    int     mode = -1;
    mainView(w2)->getViewState(offset, zoom, split, mode);
    check(mode == 1, "the view is side by side again");
    check(split > 0.29 && split < 0.31, "with the split where it was left (" +
                                            std::to_string(split) + ")");
    const int colours = coloursOnTheLeft(mainView(w2));
    check(colours > 20, "and the item's view draws the picture (" + std::to_string(colours) +
                            " colours, 1 would be bare background)");
    w2->close();
    settle(500);
  }

  // ---- a snapshot from before the fix: split mode, no point ---------------------------------
  {
    const auto saved = QSettings().value("Autosaveplaylist").toByteArray();
    QDomDocument doc;
    check(bool(doc.setContent(qUncompress(saved))), "the snapshot is readable");
    int removed = 0;
    for (const auto *tag : {"splittingPoint", "splitting"})
    {
      auto nodes = doc.elementsByTagName(tag);
      while (nodes.count() > 0)
      {
        auto node = nodes.at(0);
        node.parentNode().removeChild(node);
        ++removed;
      }
    }
    check(removed > 0, "the split point is taken out, as an old snapshot would have it");
    QSettings().setValue("Autosaveplaylist", qCompress(doc.toByteArray()));

    auto *w3 = new MainWindow(false);
    w3->resize(1400, 900);
    w3->show();
    settle(2500);
    QPointF offset;
    double  zoom = 0, split = 0;
    int     mode = -1;
    mainView(w3)->getViewState(offset, zoom, split, mode);
    check(mode == 1 && split > 0.49 && split < 0.51,
          "an old snapshot restores side by side with the split in the middle (" +
              std::to_string(split) + "), not at 0");
    const int colours = coloursOnTheLeft(mainView(w3));
    check(colours > 20, "and the picture is drawn (" + std::to_string(colours) + " colours)");
  }

  std::cout << "RESULT: " << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
