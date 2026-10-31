// Regression: perspectives, the analysis tools as docks, the properties tabs and the H.264 decoder
// setting (patch 0087).
//
// Pinned here:
//   * Five tool docks exist from start-up, hidden, and none of the tools is created until asked for.
//   * View > Perspective has five entries on Alt+1 - Alt+5; each shows its own set of docks.
//   * "Save Layout to This Perspective" keeps the user's arrangement, "Reset This Perspective" goes
//     back to the definition.
//   * A tool opens in its dock, not in a window of its own. Closing the dock closes the tool, Esc in
//     the tool hides the dock, and asking again brings both back.
//   * An item's properties are on two tabs, Video and Overlays.
//   * Settings > Decoders offers a default decoder for H.264.
//
// Argument: a raw YUV / Y4M sequence (for the properties tabs).
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTabWidget>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/SequenceGeneratorWindow.h"
#include "integration/StreamDiffWindow.h"
#include "integration/ToolDock.h"
#include "ui/Mainwindow.h"
#include "ui/SettingsDialog.h"

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

QMenu *subMenu(QMenu *menu, const QString &title)
{
  for (auto *action : menu->actions())
    if (action->menu() && action->menu()->title().remove('&') == title)
      return action->menu();
  return nullptr;
}

QMenu *topMenu(MainWindow *w, const QString &title)
{
  for (auto *action : w->menuBar()->actions())
    if (action->menu() && action->menu()->title().remove('&') == title)
      return action->menu();
  return nullptr;
}

QAction *actionIn(QMenu *menu, const QString &text)
{
  if (menu)
    for (auto *action : menu->actions())
      if (action->text().remove('&').startsWith(text))
        return action;
  return nullptr;
}

bool shown(MainWindow *w, const char *name)
{
  auto *dock = w->findChild<QDockWidget *>(name);
  return dock && dock->isVisible();
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1600, 1000);
  w->show();
  QMetaObject::invokeMethod(w, "resetWindowLayout");
  settle(200);

  // --- tool docks ------------------------------------------------------------------------------
  const char *toolDocks[] = {
      "bdRateDock", "streamDiffDock", "globalMotionDock", "meCompareDock", "sequenceGeneratorDock"};
  for (auto *name : toolDocks)
  {
    auto *dock = w->findChild<bda::integration::ToolDock *>(name);
    check(dock && !dock->isVisible() && w->dockWidgetArea(dock) == Qt::BottomDockWidgetArea,
          std::string(name) + " exists, hidden, at the bottom");
  }
  check(w->findChild<bda::integration::StreamDiffWindow *>() == nullptr &&
            w->findChild<bda::integration::SequenceGeneratorWindow *>() == nullptr,
        "no tool is created before it is asked for");

  // --- perspectives ----------------------------------------------------------------------------
  auto *perspective = subMenu(topMenu(w, "View"), "Perspective");
  check(perspective != nullptr, "View > Perspective exists");
  const QStringList names = {"Default", "Bitstream debug", "YUV compare", "RD / BD-rate", "ME / GM"};
  for (int i = 0; i < names.size(); ++i)
  {
    auto *action = actionIn(perspective, names[i]);
    check(action && action->shortcut() == QKeySequence(Qt::ALT | Qt::Key(Qt::Key_1 + i)),
          names[i].toStdString() + " is on Alt+" + std::to_string(i + 1));
  }

  actionIn(perspective, "RD / BD-rate")->trigger();
  settle(200);
  check(shown(w, "rateDistortionPlotDock") && shown(w, "bdRateDock") && !shown(w, "fileInfoDock"),
        "RD / BD-rate shows the RD plot and SB BD-rate, not Info");
  actionIn(perspective, "ME / GM")->trigger();
  settle(200);
  check(shown(w, "motionEstimationDock") && shown(w, "globalMotionDock") &&
            shown(w, "meCompareDock") && !shown(w, "rateDistortionPlotDock"),
        "ME / GM shows ME, global motion and compare, not the RD plot");
  actionIn(perspective, "Bitstream debug")->trigger();
  settle(200);
  check(shown(w, "fileInfoDock") && shown(w, "cachingInfoDock") && !shown(w, "frameInfoDock") &&
            !shown(w, "motionEstimationDock"),
        "Bitstream debug shows Info and Block Info");
  check(actionIn(perspective, "Bitstream debug")->isChecked(), "and its menu entry is checked");

  // Save and reset.
  actionIn(perspective, "ME / GM")->trigger();
  settle(200);
  w->findChild<QDockWidget *>("propertiesDock")->hide();
  actionIn(perspective, "Save Layout")->trigger();
  actionIn(perspective, "Default")->trigger();
  settle(200);
  check(shown(w, "propertiesDock"), "Default shows the properties");
  actionIn(perspective, "ME / GM")->trigger();
  settle(200);
  check(!shown(w, "propertiesDock") && shown(w, "motionEstimationDock"),
        "ME / GM comes back as saved, properties hidden");
  actionIn(perspective, "Reset This")->trigger();
  settle(200);
  check(shown(w, "propertiesDock"), "Reset This Perspective brings back its definition");
  QMetaObject::invokeMethod(w, "resetWindowLayout");
  settle(200);
  check(actionIn(perspective, "Default")->isChecked(), "Reset Window Layout makes Default current");

  // --- a tool in its dock ----------------------------------------------------------------------
  auto *generate = actionIn(topMenu(w, "Tools"), "Generate Y4M Sequence");
  check(generate != nullptr, "Tools > Generate Y4M Sequence exists");
  auto *genDock = w->findChild<bda::integration::ToolDock *>("sequenceGeneratorDock");
  if (generate && genDock)
  {
    generate->trigger();
    settle(300);
    auto *tool = w->findChild<bda::integration::SequenceGeneratorWindow *>();
    check(tool && genDock->isVisible() && genDock->tool() == tool, "the generator opens in its dock");
    check(tool && !tool->isWindow() && tool->isVisible(), "as a widget of the dock, not a window");
    genDock->close();
    settle(100);
    check(!genDock->isVisible(), "closing the dock hides it");
    generate->trigger();
    settle(200);
    check(genDock->isVisible() && tool && tool->isVisible(), "asking again brings dock and tool back");
    if (tool)
    {
      QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QApplication::sendEvent(tool, &esc);
      settle(100);
      check(!genDock->isVisible(), "Esc in the tool hides the dock");
      generate->trigger();
      settle(200);
      check(genDock->isVisible() && tool->isVisible(), "and it comes back after Esc too");
    }
  }

  // --- properties tabs -------------------------------------------------------------------------
  if (argc > 1)
  {
    w->loadFiles({QString(argv[1])});
    settle(1500);
    auto *tabs = w->findChild<QTabWidget *>("propertyTabs");
    check(tabs && tabs->count() == 2 && tabs->tabText(0) == "Video" && tabs->tabText(1) == "Overlays",
          "the item's properties are on Video and Overlays tabs");
  }
  else
    std::cout << "  SKIP  properties tabs (no file given)" << std::endl;

  // --- H.264 default decoder -------------------------------------------------------------------
  {
    SettingsDialog dialog;
    bool           found = false;
    for (auto *combo : dialog.findChildren<QComboBox *>())
      found |= combo->findText("JM (analysis)") >= 0 && combo->findText("FFMpeg") >= 0 &&
               combo->count() == 2;
    check(found, "Settings > Decoders has an H.264 default decoder (JM or FFmpeg)");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
