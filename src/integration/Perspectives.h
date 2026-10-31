#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

class QActionGroup;
class QMainWindow;
class QMenu;

namespace bda::integration
{

/* Named window layouts for the main tasks: which docks are shown and which is in front.
 *
 * Each perspective is defined here in code, on top of the main window's default placement - that
 * definition is the source of truth. "Save Layout to This Perspective" keeps the user's own
 * arrangement (QMainWindow::saveState) with a schema number; when that blob no longer restores
 * (docks renamed, schema bumped) the perspective falls back to its definition. The docks' object
 * names are therefore a contract: renaming one means bumping kSchema.
 * See docs/ai/30-designs/ui-redesign-decisions.md D-05, D-14.
 */
class Perspectives : public QObject
{
  Q_OBJECT

public:
  // placeDefault puts every dock in its default area and visibility (MainWindow's reset layout).
  Perspectives(QMainWindow *window, std::function<void()> placeDefault);

  static constexpr int kSchema = 1;

  QStringList names() const;
  QString     current() const { return this->currentName; }

  // View > Perspective: one entry per perspective (Alt+1 ...), then save and reset.
  void addMenuActions(QMenu *menu);

  // Record `name` as the current perspective without placing anything (Reset Window Layout).
  void markCurrent(const QString &name);

public slots:
  void apply(const QString &name);
  void saveCurrent();
  void resetCurrent();

signals:
  void applied(const QString &name);

private:
  void applyDefinition(const QString &name);

  QMainWindow          *window;
  std::function<void()> placeDefault;
  QString               currentName;
  QPointer<QActionGroup> actions;
};

} // namespace bda::integration
