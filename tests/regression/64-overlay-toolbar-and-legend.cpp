// Regression: the overlay toolbar, the overlay legend and the dark theme (patch 0086).
//
// Pinned here:
//   * The toolbar has one check button per overlay group of the selected item - the rows of the
//     properties panel - and the first eight answer to F1 - F8.
//   * A toolbar button and the panel's check box drive each other, and both switch the overlay.
//   * With an overlay on, the legend is drawn in the top right corner of the view; switched off from
//     the View menu it is gone.
//   * "Dark (Fusion)" is offered, turns the palette dark, and another theme brings the old one back.
//
// Argument: an AV1 stream that opens under the dav1d analyzer (it has statistics to show).
// With BDA_OVERLAY_SCREENSHOT=<path.png> the window is saved with an overlay and the legend on.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QToolBar>
#include <iostream>
#include <string>
#include <unistd.h>

#include "common/Functions.h"
#include "integration/AppTheme.h"
#include "playlistitem/playlistItem.h"
#include "statistics/StatisticUIHandler.h"
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

QAction *menuAction(MainWindow *w, const QString &menu, const QString &text)
{
  for (auto *top : w->menuBar()->actions())
    if (top->menu() && top->menu()->title().remove('&') == menu)
      for (auto *action : top->menu()->actions())
        if (action->text().remove('&') == text)
          return action;
  return nullptr;
}

QCheckBox *panelCheckBox(MainWindow *w, const QString &label)
{
  for (auto *box : w->findChildren<QCheckBox *>())
    if (box->text() == label)
      return box;
  return nullptr;
}

// Pixels in the top right corner of the view that differ between two grabs.
int changedTopRight(const QImage &a, const QImage &b)
{
  int changed = 0;
  for (int y = 0; y < std::min(120, a.height()); ++y)
    for (int x = a.width() * 2 / 3; x < a.width(); ++x)
      changed += a.pixel(x, y) != b.pixel(x, y);
  return changed;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");
  if (argc < 2)
  {
    std::cout << "usage: " << argv[0] << " <av1 stream>" << std::endl;
    return 2;
  }

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1600, 1000);
  w->show();
  w->loadFiles({QString(argv[1])});
  settle(3000);

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  auto *view = w->findChild<splitViewWidget *>("displaySplitView");
  auto *bar  = w->findChild<QToolBar *>("overlayToolBar");
  const auto items = tree ? tree->getAllPlaylistItems() : QList<playlistItem *>();
  check(items.size() == 1 && view && bar, "the stream is open, the view and the toolbar exist");
  if (items.size() != 1 || !view || !bar)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }
  auto *handler = items[0]->getStatisticsUIHandler();
  check(handler != nullptr, "the item has statistics");
  if (!handler)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  // --- toolbar buttons -------------------------------------------------------------------------
  const auto groups  = handler->overlayGroups();
  const auto actions = bar->actions();
  check(!groups.isEmpty() && actions.size() >= groups.size(),
        "one toolbar button per overlay group (" + std::to_string(groups.size()) + " groups, " +
            std::to_string(actions.size()) + " buttons)");
  for (int i = 0; i < std::min<qsizetype>(8, actions.size()); ++i)
    check(actions[i]->shortcut() == QKeySequence(Qt::Key_F1 + i),
          "button " + std::to_string(i + 1) + " answers to F" + std::to_string(i + 1));
  if (actions.size() > 8)
    check(actions[8]->shortcut().isEmpty(), "the ninth button has no F key");

  // --- toolbar <-> panel -----------------------------------------------------------------------
  const auto group  = groups.front();
  auto      *button = actions.front();
  for (const auto &g : groups)
    handler->setOverlayGroupRendered(g, false);
  settle(300);
  check(!button->isChecked(), "all overlays off: the first button is off");

  const QImage before = view->grab().toImage();
  button->trigger();
  settle(800);
  check(handler->isOverlayGroupRendered(group), "the button switched the group on: " +
                                                     group.toStdString());
  if (auto *box = panelCheckBox(w, group))
    check(box->isChecked(), "and the panel's check box followed");
  else
    check(false, "the panel has a check box for the group");

  if (auto *box = panelCheckBox(w, group))
  {
    box->setChecked(false);
    settle(300);
    check(!button->isChecked(), "unticking the panel's check box releases the button");
    box->setChecked(true);
    settle(800);
    check(button->isChecked(), "ticking it again presses the button");
  }

  // --- legend ----------------------------------------------------------------------------------
  const QImage withLegend = view->grab().toImage();
  check(changedTopRight(before, withLegend) > 200, "the legend appears in the top right corner");
  if (const auto path = qEnvironmentVariable("BDA_OVERLAY_SCREENSHOT"); !path.isEmpty())
    w->grab().save(path);

  auto *legendAction = menuAction(w, "View", "Show Overlay Legend");
  check(legendAction && legendAction->isChecked(), "View > Show Overlay Legend is on by default");
  if (legendAction)
  {
    legendAction->trigger();
    settle(300);
    const QImage withoutLegend = view->grab().toImage();
    check(changedTopRight(withoutLegend, withLegend) > 200, "switched off, the legend is gone");
    check(!QSettings().value("View/ShowOverlayLegend", true).toBool(), "and the choice is saved");
    legendAction->trigger();
  }

  // --- toolbar in the View menu ----------------------------------------------------------------
  auto *barAction = menuAction(w, "View", "Show Overlay Toolbar");
  check(barAction && barAction->isChecked(), "View > Show Overlay Toolbar is there and on");

  // --- dark theme ------------------------------------------------------------------------------
  // MainWindow::updateSettings() is not a slot, so the palette step it calls is driven directly.
  check(functions::getThemeNameList().contains(bda::integration::kDarkFusionTheme),
        "Dark (Fusion) is offered in the settings");
  const auto lightWindow = qApp->palette().color(QPalette::Window);
  bda::integration::applyThemePalette(bda::integration::kDarkFusionTheme);
  check(qApp->palette().color(QPalette::Window).lightness() < 80, "Dark (Fusion) darkens the palette");
  bda::integration::applyThemePalette("Default");
  check(qApp->palette().color(QPalette::Window) == lightWindow, "Default brings the old palette back");

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
