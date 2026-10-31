#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>

#include "playlistitem/playlistItem.h"

namespace bda::integration
{

/* The one place that knows which block is selected: item, frame and pixel position.
 *
 * The views report a click here (setBlock); every panel that follows the selection - Block Info,
 * Frame Info, the hex dump, the AI assistant - listens to blockChanged instead of each being wired
 * to both views separately. MainWindow does the wiring, so the YUViewLib widgets do not know this
 * class. A jump to the selection already set emits nothing (no loops once a panel can set it too);
 * a click in a view always does. The item is held as a QPointer: when it is deleted the selection
 * clears itself.
 * See docs/ai/30-designs/ui-redesign-decisions.md D-07.
 */
class SelectionContext : public QObject
{
  Q_OBJECT

public:
  enum class Source
  {
    PrimaryView,
    SeparateView,
    Jump, //!< Find diff, Problems, Shift+click compare
  };

  explicit SelectionContext(QObject *parent = nullptr) : QObject(parent) {}

  playlistItem *item() const { return this->selectedItem; }
  QPoint        pixel() const { return this->selectedPixel; }
  int           frame() const { return this->selectedFrame; }
  Source        source() const { return this->lastSource; }

public slots:
  void setBlock(playlistItem *item, QPoint pixel, int frame, Source source);
  void clear();
  // Drop the remembered selection without telling anyone, so that the next click is passed on even
  // when it repeats the last one (a new playlist selection resets the panels on its own).
  void forget();

signals:
  void blockChanged(playlistItem *item, QPoint pixel, int frame);

private:
  QPointer<playlistItem>  selectedItem;
  QMetaObject::Connection itemDestroyed;
  QPoint                  selectedPixel;
  int                     selectedFrame{-1};
  Source                  lastSource{Source::PrimaryView};
};

} // namespace bda::integration
