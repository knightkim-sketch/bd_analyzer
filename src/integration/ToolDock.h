#pragma once

#include <QDialog>
#include <QDockWidget>
#include <QPointer>

class QLabel;

namespace bda::integration
{

/* A dock for one of the analysis tools that used to be floating dialogs (SB BD-rate, Find diff,
 * Global motion, ME block compare, Y4M sequence generator).
 *
 * The dock exists from start-up, with a line telling how to open the tool, so that the saved window
 * state and the perspectives can place it; the tool itself is still created the first time it is
 * asked for and then put in with setTool(). The tool stays the QDialog it was - only its window
 * flags change - so its own code is untouched:
 *   - closing the dock closes the tool, which is what cancels a running job (their closeEvent);
 *   - the tool closing itself (Esc, a Close button) hides the dock instead of leaving it empty.
 * See docs/ai/30-designs/ui-redesign-decisions.md D-06, D-15.
 */
class ToolDock : public QDockWidget
{
  Q_OBJECT

public:
  ToolDock(const QString &title,
           const QString &objectName,
           const QString &howToOpen,
           QWidget       *parent = nullptr);

  void     setTool(QDialog *tool);
  QDialog *tool() const { return this->toolDialog; }

  // Show the dock with its tool, in front of what it is tabbed with.
  void present();

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  QPointer<QDialog> toolDialog;
  QLabel           *placeholder{};
  bool              closingTool{};
};

} // namespace bda::integration
