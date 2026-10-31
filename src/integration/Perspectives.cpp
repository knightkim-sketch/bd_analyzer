#include "Perspectives.h"

#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>

#include <map>

namespace bda::integration
{

namespace
{

struct Definition
{
  QString     name;
  QStringList shown; // object names; every other dock but the playback controls is hidden
  QStringList front; // raised last, so these are the visible tab where docks share a tab bar
};

/* The docks by object name (mainwindow.ui and the tool docks MainWindow adds):
 *   playlistDockWidget, fileInfoDock (Info), cachingInfoDock (Block Info), propertiesDock,
 *   frameInfoDock, playbackControllerDock, rateDistortionPlotDock, motionEstimationDock,
 *   assistDock, ytTranscodeDock, bdRateDock, streamDiffDock, globalMotionDock, meCompareDock,
 *   sequenceGeneratorDock
 */
const std::vector<Definition> &definitions()
{
  static const std::vector<Definition> list = {
      {"Default",
       {"playlistDockWidget", "fileInfoDock", "cachingInfoDock", "propertiesDock", "frameInfoDock"},
       {}},
      // What is in the stream and in the clicked block; the hex dump is under the picture.
      {"Bitstream debug",
       {"playlistDockWidget", "fileInfoDock", "cachingInfoDock", "propertiesDock"},
       {}},
      // Two items side by side: their differences, the histogram, the block under the cursor.
      {"YUV compare",
       {"playlistDockWidget", "cachingInfoDock", "propertiesDock", "frameInfoDock", "streamDiffDock"},
       {"streamDiffDock"}},
      // Rate and distortion: the frame's scatter and the SB BD-rate groups, tabbed at the bottom.
      {"RD / BD-rate",
       {"playlistDockWidget", "cachingInfoDock", "propertiesDock", "rateDistortionPlotDock",
        "bdRateDock"},
       {"bdRateDock"}},
      // Motion: the ME controls next to the properties, compare and global motion at the bottom.
      {"ME / GM",
       {"playlistDockWidget", "cachingInfoDock", "propertiesDock", "motionEstimationDock",
        "meCompareDock", "globalMotionDock"},
       {"motionEstimationDock", "globalMotionDock"}},
  };
  return list;
}

const Definition *findDefinition(const QString &name)
{
  for (const auto &definition : definitions())
    if (definition.name == name)
      return &definition;
  return nullptr;
}

QString stateKey(const QString &name)
{
  return "Perspectives/" + name + "/state";
}

} // namespace

Perspectives::Perspectives(QMainWindow *window, std::function<void()> placeDefault)
    : QObject(window), window(window), placeDefault(std::move(placeDefault))
{
  this->currentName = QSettings().value("Perspectives/current", "Default").toString();
  if (!findDefinition(this->currentName))
    this->currentName = "Default";
}

QStringList Perspectives::names() const
{
  QStringList names;
  for (const auto &definition : definitions())
    names.append(definition.name);
  return names;
}

void Perspectives::addMenuActions(QMenu *menu)
{
  this->actions = new QActionGroup(menu);
  int number    = 0;
  for (const auto &name : this->names())
  {
    auto *action = menu->addAction(name);
    action->setCheckable(true);
    action->setChecked(name == this->currentName);
    action->setShortcut(QKeySequence(Qt::ALT | Qt::Key(Qt::Key_1 + number++)));
    action->setData(name);
    this->actions->addAction(action);
    connect(action, &QAction::triggered, this, [this, name] { this->apply(name); });
  }
  menu->addSeparator();
  connect(menu->addAction("Save Layout to This Perspective"),
          &QAction::triggered,
          this,
          &Perspectives::saveCurrent);
  connect(menu->addAction("Reset This Perspective"),
          &QAction::triggered,
          this,
          &Perspectives::resetCurrent);
}

void Perspectives::markCurrent(const QString &name)
{
  if (!findDefinition(name))
    return;
  this->currentName = name;
  QSettings().setValue("Perspectives/current", name);
  if (this->actions)
    for (auto *action : this->actions->actions())
      action->setChecked(action->data().toString() == name);
}

void Perspectives::apply(const QString &name)
{
  if (!findDefinition(name))
    return;
  const auto saved = QSettings().value(stateKey(name)).toByteArray();
  if (saved.isEmpty() || !this->window->restoreState(saved, kSchema))
    this->applyDefinition(name);
  this->markCurrent(name);
  emit applied(name);
}

void Perspectives::saveCurrent()
{
  QSettings().setValue(stateKey(this->currentName), this->window->saveState(kSchema));
}

void Perspectives::resetCurrent()
{
  QSettings().remove(stateKey(this->currentName));
  this->apply(this->currentName);
}

void Perspectives::applyDefinition(const QString &name)
{
  const auto *definition = findDefinition(name);
  this->placeDefault();
  for (auto *dock : this->window->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly))
  {
    if (dock->objectName() == "playbackControllerDock")
      continue;
    dock->setVisible(definition->shown.contains(dock->objectName()));
  }
  for (const auto &front : definition->front)
    if (auto *dock = this->window->findChild<QDockWidget *>(front, Qt::FindDirectChildrenOnly))
      dock->raise();

  // The bottom tab group (RD plot, tools) gets a usable height when the perspective shows it.
  for (const auto &name : definition->shown)
    if (auto *dock = this->window->findChild<QDockWidget *>(name, Qt::FindDirectChildrenOnly);
        dock && this->window->dockWidgetArea(dock) == Qt::BottomDockWidgetArea)
    {
      this->window->resizeDocks({dock}, {320}, Qt::Vertical);
      break;
    }
}

} // namespace bda::integration
