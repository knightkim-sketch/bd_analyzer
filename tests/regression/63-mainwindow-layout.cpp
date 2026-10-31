// Regression: the main window layout (patch 0084).
//
// Pinned here:
//   * No two menu actions share a shortcut. Ctrl+F was bound to both Show Frame Info and the split
//     view's full screen toggle, so Qt fired neither ("Ambiguous shortcut overload").
//   * The block syntax table exists once. A hidden Block Info dock duplicated the one in the
//     caching pane.
//   * The comparisons live under Analysis and the generator/exports under Tools.
//   * Reset Window Layout places every dock: the default ones docked and visible, the
//     task-specific ones (ME, AI Assistant, YouTube, RD plot) hidden. It used to restore a saved
//     blob that named none of the docks added since.
//   * Full screen hides every visible dock and brings back exactly those. The old fixed list
//     missed three docks.
//   * The title says bd-analyzer and the status bar is shown (messages were written to a hidden one).
//
// With BDA_LAYOUT_SCREENSHOT=<path.png> the window is also saved as an image for a look by eye.
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <iostream>
#include <string>

#include "ui/Mainwindow.h"
#include "ui/widgets/BlockInfoWidget.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

QMenu *topMenu(MainWindow *w, const QString &title)
{
  for (auto *action : w->menuBar()->actions())
    if (action->menu() && action->menu()->title().remove('&') == title)
      return action->menu();
  return nullptr;
}

bool menuHas(QMenu *menu, const QString &text)
{
  if (!menu)
    return false;
  for (auto *action : menu->actions())
    if (action->text().remove('&').startsWith(text))
      return true;
  return false;
}

void collectShortcuts(QMenu *menu, QMap<QString, QStringList> &out)
{
  for (auto *action : menu->actions())
  {
    if (action->menu())
      collectShortcuts(action->menu(), out);
    else if (!action->shortcut().isEmpty())
      out[action->shortcut().toString()].push_back(action->text().remove('&'));
  }
}

QDockWidget *dock(MainWindow *w, const char *name) { return w->findChild<QDockWidget *>(name); }
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *w = new MainWindow(false);
  w->resize(1600, 1000);
  w->show();
  QCoreApplication::processEvents();

  // --- shortcuts -------------------------------------------------------------------------------
  QMap<QString, QStringList> shortcuts;
  for (auto *action : w->menuBar()->actions())
    if (action->menu())
      collectShortcuts(action->menu(), shortcuts);
  for (auto it = shortcuts.cbegin(); it != shortcuts.cend(); ++it)
    check(it.value().size() == 1,
          ("shortcut " + it.key() + " is used once (" + it.value().join(", ") + ")").toStdString());

  // --- one block syntax table --------------------------------------------------------------------
  check(w->findChildren<BlockInfoWidget *>().size() == 1, "one BlockInfoWidget in the window");
  check(dock(w, "blockInfoDock") == nullptr, "the duplicate Block Info dock is gone");
  if (auto *blockDock = dock(w, "cachingInfoDock"))
    check(blockDock->windowTitle() == "Block Info", "the caching pane is titled Block Info");

  // --- menus -------------------------------------------------------------------------------------
  auto *analysis = topMenu(w, "Analysis");
  auto *tools    = topMenu(w, "Tools");
  auto *view     = topMenu(w, "View");
  auto *file     = topMenu(w, "File");
  check(analysis != nullptr, "Analysis menu exists");
  check(tools != nullptr, "Tools menu exists");
  check(menuHas(analysis, "SB BD-rate"), "SB BD-rate is under Analysis");
  check(menuHas(analysis, "Find diff"), "Find diff is under Analysis");
  check(menuHas(analysis, "Global motion"), "Global motion is under Analysis");
  check(menuHas(tools, "Generate Y4M Sequence"), "Generate Y4M Sequence is under Tools");
  check(menuHas(tools, "Export Filtered Y4M"), "Export Filtered Y4M is under Tools");
  check(menuHas(tools, "Export Stabilized Y4M"), "Export Stabilized Y4M is under Tools");
  check(!menuHas(view, "SB BD-rate") && !menuHas(view, "Find di"), "View no longer holds the comparisons");
  check(!menuHas(file, "Generate Y4M") && !menuHas(file, "Export Filtered"),
        "File no longer holds the generator and exports");

  // --- reset layout ------------------------------------------------------------------------------
  // Disturb it first: float one dock, show a hidden one, hide a default one.
  dock(w, "propertiesDock")->setFloating(true);
  dock(w, "assistDock")->show();
  dock(w, "fileInfoDock")->hide();
  QMetaObject::invokeMethod(w, "resetWindowLayout");
  QCoreApplication::processEvents();

  const char *shown[]  = {"playlistDockWidget", "fileInfoDock",  "cachingInfoDock",
                          "propertiesDock",     "frameInfoDock", "playbackControllerDock"};
  const char *hidden[] = {"motionEstimationDock", "assistDock", "ytTranscodeDock",
                          "rateDistortionPlotDock"};
  for (auto *name : shown)
  {
    auto *d = dock(w, name);
    check(d && d->isVisible() && !d->isFloating(), std::string("after reset ") + name + " is docked and shown");
  }
  for (auto *name : hidden)
  {
    auto *d = dock(w, name);
    check(d && !d->isVisible(), std::string("after reset ") + name + " is hidden");
  }
  check(w->dockWidgetArea(dock(w, "cachingInfoDock")) == Qt::LeftDockWidgetArea, "Block Info is on the left");
  check(w->dockWidgetArea(dock(w, "frameInfoDock")) == Qt::RightDockWidgetArea, "Frame Info is on the right");
  check(w->statusBar()->isVisible(), "the status bar is shown");
  check(w->windowTitle().startsWith("bd-analyzer"), "the window title is bd-analyzer");

  if (const auto path = qEnvironmentVariable("BDA_LAYOUT_SCREENSHOT"); !path.isEmpty())
    w->grab().save(path);

  // --- full screen round trip ------------------------------------------------------------------
  dock(w, "motionEstimationDock")->show();
  QCoreApplication::processEvents();
  QList<QDockWidget *> visibleBefore;
  for (auto *d : w->findChildren<QDockWidget *>())
    if (d->isVisible())
      visibleBefore.push_back(d);

  QMetaObject::invokeMethod(w, "toggleFullscreen");
  QCoreApplication::processEvents();
  auto anyVisible = false;
  for (auto *d : w->findChildren<QDockWidget *>())
    anyVisible |= d->isVisible();
  check(!anyVisible, "full screen hides every dock (ME included)");

  QMetaObject::invokeMethod(w, "toggleFullscreen");
  QCoreApplication::processEvents();
  QList<QDockWidget *> visibleAfter;
  for (auto *d : w->findChildren<QDockWidget *>())
    if (d->isVisible())
      visibleAfter.push_back(d);
  check(visibleAfter == visibleBefore, "leaving full screen brings back exactly the docks that were shown");

  delete w;
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
