// Reproduce: opening several files from the command line.
//
//   bd-analyzer refc.av1 host.av1
//
// YUViewApplication hands argv straight to MainWindow::loadFiles(), so this drives the same call
// and then counts what actually reached the playlist. Two questions in one run: does it survive,
// and does every file end up as an item.
//
//   repro-open-multiple-files <file> [file ...] [--settle ms]
#include <QApplication>
#include <QFileInfo>
#include <QSettings>

#include <iostream>
#include <string>
#include <unistd.h>

#include "playlistitem/playlistItem.h"
#include "ui/Mainwindow.h"
#include "ui/widgets/PlaylistTreeWidget.h"

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
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  QStringList files;
  int         settleMs = 8000;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--settle" && i + 1 < argc)
      settleMs = std::stoi(argv[++i]);
    else
      files << QString::fromUtf8(argv[i]);
  }
  if (files.isEmpty())
  {
    std::cerr << "usage: repro-open-multiple-files <file> [file ...] [--settle ms]\n";
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerReproOpenMany");
  QCoreApplication::setApplicationName("bdAnalyzerReproOpenMany");
  QSettings().clear();
  QSettings().remove("Autosaveplaylist");

  std::cout << "-- construct MainWindow" << std::endl;
  auto *w = new MainWindow(false); // never deleted - see regression 25
  w->resize(1200, 800);
  w->show();
  settle(500);

  std::cout << "-- loadFiles(" << files.size() << " files)" << std::endl;
  for (const auto &f : files)
    std::cout << "     " << f.toStdString() << std::endl;
  w->loadFiles(files);

  std::cout << "-- settle " << settleMs << "ms (this is where the parser threads run)" << std::endl;
  /* Churn the selection while the background parse is in flight.
   *
   * backgroundParsingFunction() checks its parser but not currentCompressedVideo, and that is a
   * QPointer - it goes null on its own when the item dies. Selecting and deselecting is what makes
   * the widget swap both, so this is the window in which the thread can find one set and the other
   * gone.
   */
  auto *churnTree = w->findChild<PlaylistTreeWidget *>();
  for (int round = 0; round < 40 && churnTree; ++round)
  {
    churnTree->selectAll();
    settle(30);
    churnTree->clearSelection();
    settle(30);
    if (churnTree->topLevelItemCount() > 0)
      churnTree->setCurrentItem(churnTree->topLevelItem(round % churnTree->topLevelItemCount()));
    settle(30);
  }
  settle(settleMs);

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  if (!tree)
  {
    std::cerr << "no playlist widget\n";
    return 1;
  }
  std::cout << "-- playlist holds " << tree->topLevelItemCount() << " items" << std::endl;
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
    if (auto *item = dynamic_cast<playlistItem *>(tree->topLevelItem(i)))
      std::cout << "     " << QFileInfo(item->properties().name).fileName().toStdString()
                << std::endl;

  std::cout << "-- done, no crash" << std::endl;
  std::cout.flush();
  _exit(0);
}
