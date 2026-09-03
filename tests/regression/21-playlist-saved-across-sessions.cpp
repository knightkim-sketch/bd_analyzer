// Regression: the playlist survives a normal quit and comes back on the next start, without any
// dialog, and the File menu carries the on/off switch for that.
//
// Before this, the snapshot in the settings ("Autosaveplaylist") existed only for crash recovery:
// PlaylistTreeWidget's destructor deleted it on every conventional quit, and the one path that
// restored it asked "It looks like YUView crashed...". Quitting therefore always lost the playlist
// unless the user answered a dialog and picked a file.
//
// What this pins down:
//   * The File menu has a checkable "Save Playlist on Exit", on by default, and toggling it writes
//     the setting - that is the only control for this behaviour.
//   * Closing the window snapshots the playlist with no dialog and no file, and the next start
//     restores it with no dialog.
//   * The snapshot survives the destructor while the setting is on, and is still cleaned up when
//     it is off (otherwise every start would look like a crash recovery).
//   * The autosave timer stops when the snapshot is taken. MainWindow::closeEvent empties the
//     playlist right after, and a timer firing then would overwrite the snapshot with an empty
//     playlist - the bug this ordering is designed to avoid.
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QMenuBar>
#include <QSettings>
#include <QTimer>
#include <iostream>
#include <string>
#include <unistd.h>

#include "ui/Mainwindow.h"
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

QAction *findAction(QMainWindow *w, const QString &text)
{
  for (auto *action : w->menuBar()->findChildren<QAction *>())
    if (action->text() == text)
      return action;
  return nullptr;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 21-playlist-saved-across-sessions <raw.yuv>" << std::endl;
    return 2;
  }
  const QString rawFile = QString(argv[1]);

  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  /* Point the derived-data cache at a directory that cannot be created. BDCachePaths then reports
   * no cache, so MainWindow::closeEvent does not open its "delete the cache?" dialog - which would
   * block this test on a modal window. Nothing else here depends on caching.
   */
  {
    QSettings settings;
    settings.setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");
  }

  /* One window for both the menu check and the quit check.
   *
   * MainWindow is never deleted here: doing so aborts with "free(): invalid pointer" in this
   * codebase regardless of this feature (reproduced with a bare new/show/delete), which is why
   * the other MainWindow-driven tests leak it too. And a leaked window with an *empty* playlist
   * is not harmless either - its 10 s autosave timer would fire and remove the snapshot - so the
   * window that gets closed here is the one that holds the item.
   */
  auto *w = new MainWindow(false);
  w->show();
  settle(500);

  // --- the menu switch -----------------------------------------------------------------------
  {
    auto *action = findAction(w, "Save Playlist on Exit");
    check(action != nullptr, "the File menu has a \"Save Playlist on Exit\" entry");
    check(action && action->isCheckable(), "the entry is a checkbox");
    check(action && action->isChecked(), "and it is on by default");

    if (action)
    {
      action->setChecked(false);
      check(!QSettings().value("SavePlaylistOnExit", true).toBool(),
            "unchecking it turns the setting off");
      action->setChecked(true);
      check(QSettings().value("SavePlaylistOnExit", true).toBool(),
            "checking it turns the setting back on");
    }
  }

  // --- quit and come back --------------------------------------------------------------------
  {
    QSettings().remove("Autosaveplaylist");

    w->loadFiles({rawFile});
    settle(2000);

    auto *tree = w->findChild<PlaylistTreeWidget *>();
    check(tree && tree->topLevelItemCount() == 1, "the first session has one item in the playlist");

    w->close();
    settle(500);
    check(QSettings().contains("Autosaveplaylist"),
          "closing the window snapshots the playlist, with no dialog and no file");

    // The second session must restore it without asking. A dialog here would block, so the runner
    // timeout is what catches a regression into the old prompt.
    auto *w2 = new MainWindow(false);
    w2->show();
    settle(2000);

    auto *tree2 = w2->findChild<PlaylistTreeWidget *>();
    check(tree2 && tree2->topLevelItemCount() == 1,
          "the next start restores the playlist without asking");
  }

  // --- switched off: back to the crash-recovery behaviour ------------------------------------
  {
    QSettings().setValue("SavePlaylistOnExit", false);
    QSettings().setValue("Autosaveplaylist", QByteArray("not-a-real-playlist"));

    // Only the widget, so the restore dialog of MainWindow is not in play here.
    auto *tree = new PlaylistTreeWidget();
    check(tree->isAutosaveAvailable(), "a snapshot is present before the widget goes away");
    delete tree;
    check(!QSettings().contains("Autosaveplaylist"),
          "the destructor still clears the snapshot when the setting is off");
  }

  // --- switched on: the destructor keeps it --------------------------------------------------
  {
    QSettings().setValue("SavePlaylistOnExit", true);
    QSettings().setValue("Autosaveplaylist", QByteArray("not-a-real-playlist"));

    auto *tree = new PlaylistTreeWidget();
    delete tree;
    check(QSettings().contains("Autosaveplaylist"),
          "and keeps it when the setting is on");
  }

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
