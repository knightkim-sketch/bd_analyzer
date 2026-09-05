// Regression: the main window can be destroyed, and every dock can be brought back.
//
// The other MainWindow tests deliberately leak the window, so nothing ever ran ~MainWindow - and
// that is where the bug lived. Two things are pinned here:
//
//   * Quitting aborted with "free(): invalid pointer". FrameInfoWidget held its block-statistics
//     section as a value member and handed it to the caching pane, where QWidget::setParent()
//     made Qt an owner too. Both destroyed it. This test simply deletes the window: a double
//     free aborts the process, so the exit code is the assertion.
//   * The Motion Estimation dock ships hidden and had no entry under View -> Dock Panels, so
//     there was no way to show it at all. Every dock needs a way back.
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMenu>
#include <QSettings>
#include <iostream>
#include <string>

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
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *w = new MainWindow(false);
  w->resize(1400, 900);
  w->show();
  QCoreApplication::processEvents();

  // --- every dock has a way back ---------------------------------------------------------------
  /* The menu is built from each dock's own toggleViewAction(), so the entries are matched by
   * identity rather than by label - rewording a menu item should not break this.
   */
  QMenu *dockPanels = nullptr;
  for (auto *menu : w->findChildren<QMenu *>())
    if (menu->title() == "Dock Panels")
      dockPanels = menu;
  check(dockPanels != nullptr, "View -> Dock Panels exists");

  if (dockPanels)
  {
    for (auto *dock : w->findChildren<QDockWidget *>())
    {
      const auto name = dock->objectName();
      check(dockPanels->actions().contains(dock->toggleViewAction()),
            ("dock \"" + name + "\" can be toggled from the menu").toStdString());
    }
  }

  auto *meDock = w->findChild<QDockWidget *>("motionEstimationDock");
  check(meDock != nullptr, "the Motion Estimation dock is in the window");
  if (meDock)
  {
    // It ships hidden - which is exactly why the menu entry above has to exist.
    meDock->setVisible(true);
    QCoreApplication::processEvents();
    check(meDock->isVisible(), "and can be shown");
  }

  /* --- and the window can be destroyed ---------------------------------------------------------
   *
   * No check() to make here: a double free aborts, and the harness reads that as a crash.
   */
  delete w;
  QCoreApplication::processEvents();
  std::cout << "  ok    the main window was destroyed without aborting" << std::endl;

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
