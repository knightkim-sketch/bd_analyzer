// Reproduce: load a playlist, move the frame, select every item, right-click -> SIGSEGV.
//
// The context menu handler itself only builds a QMenu, so the crash is almost certainly not in the
// menu construction but in whatever runs inside the nested event loop that QMenu::exec() spins up.
// This driver reproduces the gesture in that order and keeps the nested loop alive long enough for
// the queued work to land.
//
//   repro-playlist-context-menu <playlist.yuvplaylist> [frameIdx] [loadSettleMs] [preClickMs]
//
// frameIdx < 0 skips the frame move - the user reports the crash without one. loadSettleMs is how
// long the streams get to parse before the selection, preClickMs how long between selecting every
// item and the right-click; the crash is most likely a queued signal arriving inside the menu's
// nested event loop, so both of those are the knobs that matter.
//
// Runs under its own QSettings identity so it never touches the settings - or the autosaved
// playlist - of a real bd_analyzer the user has open at the same time.
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QSettings>
#include <QTimer>
#include <iostream>
#include <string>
#include <unistd.h>

#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/PlaylistTreeWidget.h"
#include "playlistitem/playlistItem.h"

namespace
{
void settle(int ms)
{
  for (int i = 0; i < ms / 10; ++i)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    usleep(10000);
  }
}

void step(const std::string &what)
{
  std::cout << "-- " << what << std::endl; // unbuffered: the last line printed marks the crash site
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: repro-playlist-context-menu <playlist.yuvplaylist> [frameIdx] [settleMs]\n";
    return 2;
  }
  const auto playlist = QString(argv[1]);
  const int  frameIdx = argc >= 3 ? std::stoi(argv[2]) : 5;
  const int  settleMs   = argc >= 4 ? std::stoi(argv[3]) : 4000;
  const int  preClickMs = argc >= 5 ? std::stoi(argv[4]) : 2000;

  QCoreApplication::setOrganizationName("bdAnalyzerReproCtxMenu");
  QCoreApplication::setApplicationName("bdAnalyzerReproCtxMenu");
  QSettings().clear();
  QSettings().remove("Autosaveplaylist");

  step("construct MainWindow");
  auto *w = new MainWindow(false); // never deleted - see regression 25
  w->resize(1200, 800);
  w->show();
  settle(500);

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  if (!tree) { std::cerr << "no playlist widget\n"; return 1; }

  step("load playlist");
  // loadFiles() routes a .yuvplaylist to the (private) playlist reader - the same path the
  // File menu takes.
  tree->loadFiles(QStringList(playlist));
  settle(settleMs);
  if (tree->topLevelItemCount() == 0) { std::cerr << "playlist loaded nothing\n"; return 1; }
  std::cout << "   items: " << tree->topLevelItemCount() << std::endl;

  if (frameIdx >= 0)
  {
    step("move the frame to " + std::to_string(frameIdx));
    auto *playback = w->findChild<PlaybackController *>();
    if (!playback) { std::cerr << "no playback controller\n"; return 1; }
    tree->setCurrentItem(tree->topLevelItem(0));
    settle(1000);
    playback->setCurrentFrameAndUpdate(frameIdx);
    settle(2000);
  }
  else
    step("no frame move");

  step("select every item");
  tree->selectAll();
  settle(preClickMs);
  int selected = 0;
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
    if (tree->topLevelItem(i)->isSelected())
      ++selected;
  std::cout << "   selected: " << selected << std::endl;

  // Offscreen never puts pixels anywhere, so the paint path - which a real right-click forces by
  // repainting what the popup uncovers - is otherwise never taken. grab() renders for real.
  step("force a repaint of the window and the view");
  {
    auto shot = w->grab();
    std::cout << "   grabbed " << shot.width() << "x" << shot.height() << std::endl;
  }
  settle(500);

  step("right-click on the selection");
  // QMenu::exec() blocks in its own event loop, so the dismissal has to be queued before the
  // event is delivered. Two shots: the first lets the queued work run inside the nested loop,
  // the second closes the menu so this process can exit.
  QTimer::singleShot(1500, [] { QCoreApplication::processEvents(); });
  QTimer::singleShot(3000, [] {
    if (auto *popup = QApplication::activePopupWidget())
      popup->close();
  });
  const auto pos = tree->viewport()->rect().center();
  QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, tree->viewport()->mapToGlobal(pos));
  QCoreApplication::sendEvent(tree->viewport(), &ev);

  step("survived the context menu");
  settle(2000);
  step("done - no crash");
  return 0;
}
