#pragma once

#include <QWidget>

#include <map>
#include <vector>

#include "StreamDiffWindow.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace bda::integration
{

/* A list of places worth looking at, each one click away (the Problems dock).
 *
 * Fed from two sources in this first version (docs/ai/30-designs/ui-redesign-decisions.md D-20):
 *   - Find diff: the first differing header element and OBU payload, every superblock whose block
 *     syntax differs, and every frame whose reconstruction differs - the first divergence on top;
 *   - decoder errors of the items in the playlist.
 * Activating a row asks for its frame and position (locateRequested); MainWindow passes that to the
 * Find diff tool, which moves the view and the block panels there.
 */
class ProblemsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ProblemsWidget(QWidget *parent = nullptr);

  void setDiffReport(const StreamDiffReport &report);
  // One row per item; rebuilt only when the text changes (this is called on every frame).
  void setDecoderError(const void *item, const QString &itemName, const QString &error);
  // Drop the rows of items that are gone.
  void keepDecoderErrorsOf(const std::vector<const void *> &items);
  int  count() const;

signals:
  void locateRequested(StreamDiffLocation where);

private:
  QTreeWidgetItem *addRow(const QString &severity,
                          int            frameIdx,
                          const QString &where,
                          const QString &source,
                          const QString &message,
                          std::optional<QPoint> pixel);
  void             removeSource(const QString &source);
  void             updateSummary();

  QLabel                                   *summary{};
  QTreeWidget                              *list{};
  std::map<const void *, QTreeWidgetItem *> decoderRows;
  std::map<const void *, QString>           decoderErrors;
};

} // namespace bda::integration
