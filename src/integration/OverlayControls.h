#pragma once

#include <QPointer>
#include <QToolBar>

class QPainter;
class QRect;
class playlistItem;

namespace bda::integration
{

/* The overlay legend: one line per statistics overlay that is switched on, drawn in the top right
 * corner of the view. On by default - it is what makes the colours readable - and the user can hide it
 * (View > Show Overlay Legend); that choice is kept in the settings.
 */
bool overlayLegendEnabled();
void setOverlayLegendEnabled(bool enabled);
void paintOverlayLegend(QPainter &painter, playlistItem *item, const QRect &area);

/* A toolbar with one check button per overlay group of the selected item - the same rows as the
 * properties panel - so the overlays can be switched without hunting through the panel. The first
 * eight get F1 - F8. The buttons follow the panel's check boxes and the panel follows the buttons.
 */
class OverlayToolBar : public QToolBar
{
  Q_OBJECT

public:
  explicit OverlayToolBar(QWidget *parent = nullptr);

public slots:
  void currentSelectedItemsChanged(playlistItem *item1, playlistItem *item2);
  // The item's overlay types or their on/off state changed (a panel check box, a new ME run).
  void refresh();

private:
  void rebuild();

  QPointer<playlistItem> item;
  QStringList            shownGroups;
  bool                   syncing{};
};

} // namespace bda::integration
