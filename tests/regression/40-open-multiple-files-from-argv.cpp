// Regression: opening several files at once, the way the command line does it.
//
//     bd-analyzer refc.av1 host.av1
//
// YUViewApplication hands argv straight to MainWindow::loadFiles(), so that call is the whole
// command line path. Two things had to hold and one of them did not:
//
//   * every file becomes a playlist item, including one the parser cannot finish - a stream that
//     fails to open is a result to look at, not a reason to drop it silently;
//   * the background parser must not take the process down. It runs on a QtConcurrent thread, and
//     QtConcurrent only carries exceptions derived from QException. The AV1 parser throws
//     std::logic_error on syntax it will not accept, and that escaped the task: the process died
//     inside the unwinder, which is why the crash address pointed at this function's exception
//     cleanup rather than at any statement in it.
//
// Not covered here: a file that cannot be opened at all. Such an item is still added to the
// playlist and then used, and getYUVVideo() asserts on it (rawFormat is never set). NDEBUG is
// defined in neither the application nor the test build, so that assert is live in the shipped
// binary too - a separate defect from the one above, and not yet fixed.
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
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    usleep(10000);
  }
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3)
  {
    std::cerr << "usage: 40-open-multiple-files-from-argv <file> <file> [file ...]\n";
    return 2;
  }
  QStringList files;
  for (int i = 1; i < argc; ++i)
    files << QString::fromUtf8(argv[i]);

  QCoreApplication::setOrganizationName("bdAnalyzerOpenMany");
  QCoreApplication::setApplicationName("bdAnalyzerOpenMany");
  QSettings().clear();
  QSettings().remove("Autosaveplaylist");

  auto *w = new MainWindow(false); // never deleted - see regression 25
  w->resize(1200, 800);
  w->show();
  settle(500);

  w->loadFiles(files);
  // The parse runs on a worker; the crash happened there, not in loadFiles.
  settle(6000);

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  if (!tree)
  {
    std::cout << "RESULT: FAIL - no playlist widget" << std::endl;
    return 1;
  }

  check(tree->topLevelItemCount() == files.size(),
        "every file given on the command line became a playlist item (" +
            std::to_string(tree->topLevelItemCount()) + " of " +
            std::to_string(files.size()) + ")");

  // Selecting each one is what starts a parse for it, so this is where an escaping exception used
  // to land.
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
  {
    tree->setCurrentItem(tree->topLevelItem(i));
    settle(800);
  }
  check(true, "selecting each item in turn did not take the process down");

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
  return failures == 0 ? 0 : 1;
}
