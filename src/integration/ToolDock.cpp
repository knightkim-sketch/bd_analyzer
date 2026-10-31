#include "ToolDock.h"

#include <QCloseEvent>
#include <QDialog>
#include <QLabel>

namespace bda::integration
{

ToolDock::ToolDock(const QString &title,
                   const QString &objectName,
                   const QString &howToOpen,
                   QWidget       *parent)
    : QDockWidget(title, parent)
{
  this->setObjectName(objectName);
  this->placeholder = new QLabel(howToOpen, this);
  this->placeholder->setAlignment(Qt::AlignCenter);
  this->placeholder->setWordWrap(true);
  this->placeholder->setMargin(12);
  this->placeholder->setEnabled(false);
  this->setWidget(this->placeholder);
}

void ToolDock::setTool(QDialog *tool)
{
  if (!tool || tool == this->toolDialog)
    return;
  this->toolDialog = tool;
  // A plain child widget from here on: no window frame, laid out by the dock. The size the dialog
  // asked for as a window (1100 - 1400 px wide) would otherwise force the dock to that width.
  tool->setWindowFlags(Qt::Widget);
  tool->setMinimumSize(0, 0);
  this->setWidget(tool);
  this->placeholder->deleteLater();
  this->placeholder = nullptr;

  // The tool closed itself (Esc -> reject, or a Close button): hide the dock rather than keep an
  // empty frame, and make the tool visible again for the next time the dock is shown.
  connect(tool, &QDialog::finished, this, [this] {
    if (this->closingTool)
      return;
    this->hide();
    if (this->toolDialog)
      this->toolDialog->show();
  });
}

void ToolDock::present()
{
  if (this->toolDialog && this->toolDialog->isHidden())
    this->toolDialog->show();
  this->show();
  this->raise();
  if (this->isFloating())
    this->activateWindow();
}

void ToolDock::closeEvent(QCloseEvent *event)
{
  /* The user closed the dock: that is closing the tool, as closing its window was. The tools stop
   * their background work in closeEvent (Find diff, Global motion). Shown again by present().
   */
  if (this->toolDialog && this->toolDialog->isVisible())
  {
    this->closingTool = true;
    this->toolDialog->close();
    this->closingTool = false;
  }
  QDockWidget::closeEvent(event);
}

} // namespace bda::integration
