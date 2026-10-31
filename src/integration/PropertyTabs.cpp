#include "PropertyTabs.h"

#include <QLayout>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace bda::integration
{

namespace
{
int lastTab = 0;
}

void splitPropertiesIntoTabs(QVBoxLayout *column, const std::vector<QLayout *> &overlays)
{
  auto *videoPage   = new QWidget;
  auto *videoColumn = new QVBoxLayout(videoPage);
  videoColumn->setContentsMargins(0, 0, 0, 0);
  while (auto *item = column->takeAt(0))
  {
    if (auto *widget = item->widget())
    {
      videoColumn->addWidget(widget);
      delete item;
    }
    else if (auto *layout = item->layout())
    {
      // A layout keeps its old parent after takeAt(); addLayout() refuses one that has a parent.
      layout->setParent(nullptr);
      videoColumn->addLayout(layout);
    }
    else
      videoColumn->addItem(item); // a spacer
  }

  auto *overlayPage   = new QWidget;
  auto *overlayColumn = new QVBoxLayout(overlayPage);
  overlayColumn->setContentsMargins(0, 0, 0, 0);
  for (auto *layout : overlays)
    if (layout)
      overlayColumn->addLayout(layout, 1);

  auto *tabs = new QTabWidget;
  tabs->setObjectName("propertyTabs");
  tabs->addTab(videoPage, "Video");
  tabs->addTab(overlayPage, "Overlays");
  tabs->setCurrentIndex(lastTab);
  QObject::connect(tabs, &QTabWidget::currentChanged, [](int index) { lastTab = index; });
  column->addWidget(tabs);
}

} // namespace bda::integration
