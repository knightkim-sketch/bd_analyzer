// Regression: Help > About bd-analyzer says which build this is.
//
// What this pins down:
//   * the Help menu leads with "About bd-analyzer", and YUView's own About is still there below it;
//   * the box shows the version from the repository's VERSION file - the one package.sh stamps on
//     the rpm - so a binary and its package cannot disagree about what they are;
//   * the git hash came through from scripts/build.sh (not the "unknown" a script-less build gets).
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/BdaAbout.h"
#include "ui/Mainwindow.h"

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
  if (argc < 2)
  {
    std::cerr << "usage: 46-about-bd-analyzer <path to VERSION>\n";
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerAbout");
  QCoreApplication::setApplicationName("bdAnalyzerAbout");
  QSettings().clear();

  QFile versionFile(QString::fromUtf8(argv[1]));
  check(versionFile.open(QIODevice::ReadOnly), "the VERSION file is readable");
  const auto version = QString::fromUtf8(versionFile.readAll()).trimmed();
  check(!version.isEmpty(), "and holds a version (" + version.toStdString() + ")");

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->show();
  settle(500);

  QMenu *help = nullptr;
  for (auto *action : w->menuBar()->actions())
    if (action->text() == "&Help")
      help = action->menu();
  check(help != nullptr, "the window has a Help menu");
  if (!help)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }
  const auto items = help->actions();
  check(!items.isEmpty() && items[0]->text() == "About bd-analyzer",
        "it leads with About bd-analyzer");
  check(items.size() > 1 && items[1]->text() == "About YUView", "and keeps About YUView below it");

  check(bda::integration::bdaVersion() == version,
        "the build carries the VERSION file's version (" +
            bda::integration::bdaVersion().toStdString() + ")");
  check(bda::integration::bdaGitHash() != "unknown" && !bda::integration::bdaGitHash().isEmpty(),
        "and the git hash from the build script (" +
            bda::integration::bdaGitHash().toStdString() + ")");

  // The box itself: open it from the menu, read it, close it.
  QString shown;
  QTimer::singleShot(500, [&shown] {
    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
    {
      shown = box->text();
      box->close();
    }
  });
  if (!items.isEmpty())
    items[0]->trigger();
  settle(200);
  check(shown.contains(version) && shown.contains(bda::integration::bdaGitHash()) &&
            shown.contains("Built"),
        "the box shows the version, the git hash and the build date");

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
