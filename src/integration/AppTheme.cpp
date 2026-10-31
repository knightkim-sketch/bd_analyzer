#include "AppTheme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace bda::integration
{

namespace
{

QPalette darkPalette()
{
  const QColor window(43, 43, 43);
  const QColor base(30, 30, 30);
  const QColor text(224, 224, 224);
  const QColor disabled(128, 128, 128);
  const QColor highlight(61, 174, 233);

  QPalette palette;
  palette.setColor(QPalette::Window, window);
  palette.setColor(QPalette::WindowText, text);
  palette.setColor(QPalette::Base, base);
  palette.setColor(QPalette::AlternateBase, window);
  palette.setColor(QPalette::ToolTipBase, window);
  palette.setColor(QPalette::ToolTipText, text);
  palette.setColor(QPalette::PlaceholderText, disabled);
  palette.setColor(QPalette::Text, text);
  palette.setColor(QPalette::Button, window);
  palette.setColor(QPalette::ButtonText, text);
  palette.setColor(QPalette::BrightText, Qt::red);
  palette.setColor(QPalette::Link, highlight);
  palette.setColor(QPalette::Highlight, highlight);
  palette.setColor(QPalette::HighlightedText, Qt::black);
  for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
    palette.setColor(QPalette::Disabled, role, disabled);
  return palette;
}

} // namespace

void applyThemePalette(const QString &themeName)
{
  // The style and palette the platform chose at start-up, to go back to when the dark theme is left.
  // Nothing is touched for the other themes until the dark one was used: a platform theme's palette
  // stays as it is.
  static const QString  platformStyle   = qApp->style()->name();
  static const QPalette platformPalette = qApp->palette();
  static bool           darkApplied     = false;

  if (themeName == kDarkFusionTheme)
  {
    if (qApp->style()->name().compare("fusion", Qt::CaseInsensitive) != 0)
      qApp->setStyle(QStyleFactory::create("Fusion"));
    qApp->setPalette(darkPalette());
    darkApplied = true;
    return;
  }
  if (!darkApplied)
    return;
  if (qApp->style()->name() != platformStyle)
    if (auto *style = QStyleFactory::create(platformStyle))
      qApp->setStyle(style);
  qApp->setPalette(platformPalette);
  darkApplied = false;
}

} // namespace bda::integration
