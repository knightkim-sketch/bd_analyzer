#include "SelectionContext.h"

#include "playlistitem/playlistItem.h"

namespace bda::integration
{

void SelectionContext::setBlock(playlistItem *item, QPoint pixel, int frame, Source source)
{
  this->lastSource = source;
  /* A click in a view is always passed on, also when it repeats the last one: a click is how the
   * user brings the hex dump pane back. Only a jump that lands where the selection already is - the
   * kind a panel setting the selection could loop on - is dropped.
   */
  if (source == Source::Jump && item == this->selectedItem && pixel == this->selectedPixel &&
      frame == this->selectedFrame)
    return;
  if (item != this->selectedItem)
  {
    disconnect(this->itemDestroyed);
    if (item)
      this->itemDestroyed = connect(item, &QObject::destroyed, this, &SelectionContext::clear);
  }
  this->selectedItem  = item;
  this->selectedPixel = pixel;
  this->selectedFrame = frame;
  emit blockChanged(item, pixel, frame);
}

void SelectionContext::forget()
{
  disconnect(this->itemDestroyed);
  this->selectedItem  = nullptr;
  this->selectedPixel = QPoint();
  this->selectedFrame = -1;
}

void SelectionContext::clear()
{
  disconnect(this->itemDestroyed);
  const bool wasSet   = this->selectedFrame >= 0;
  this->lastSource    = Source::PrimaryView; // a cleared selection concerns every listener
  this->selectedItem  = nullptr;
  this->selectedPixel = QPoint();
  this->selectedFrame = -1;
  if (wasSet)
    emit blockChanged(nullptr, QPoint(), -1);
}

} // namespace bda::integration
