// The Container tab of the Bitstream Analysis panel: an MP4's box tree and its located samples.
//
// The parsing itself is Qt-free in src/container; this is only the boundary that maps the file and
// fills widgets. Mapping rather than reading matters - mdat is the whole video and there is no
// reason to pull it into memory to look at the headers around it.
#pragma once

#include <QString>
#include <QWidget>

#include "container/Mp4Parser.h"
#include "container/Mp4SampleTable.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace bda::integration
{

class Mp4ContainerWidget : public QWidget
{
  Q_OBJECT

public:
  explicit Mp4ContainerWidget(QWidget *parent = nullptr);

public slots:
  /* Show this file. A path that is not an MP4-family file clears the tab and says so rather than
   * showing a stale tree from the previously selected item.
   */
  void setFile(const QString &path);
  void clearFile();

private:
  void fillBoxTree(const std::vector<container::Mp4Box> &boxes, QTreeWidgetItem *parent);
  void fillSamples(const std::vector<container::Mp4Track> &tracks);

  QLabel      *summary{};
  QTreeWidget *boxTree{};
  QTreeWidget *sampleTree{};

  QString shownPath;
};

} // namespace bda::integration

/* uic writes the member declaration for a promoted widget using the bare class name - same reason
 * as MotionEstimationWidget and AssistPanelWidget.
 */
using Mp4ContainerWidget = bda::integration::Mp4ContainerWidget;
