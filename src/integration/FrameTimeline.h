#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <optional>
#include <vector>

#include "FrameAvcSummary.h"
#include "FrameObuSummary.h"

#include "playlistitem/playlistItem.h"

class QAbstractItemModel;

namespace bda::integration
{

/* The frame timeline: one bar per frame of the selected item, in playback order.
 *
 *   height  coded bytes of the frame (linear, or log from the context menu); raw video has none, so
 *           its bars are all the same height
 *   colour  frame type - AV1 KEY red, INTRA_ONLY orange, INTER blue, SWITCH purple,
 *           show_existing_frame a thin bar; H.264 / HEVC I red, P blue, B green. Until the bitstream
 *           parser has reached a frame only "key" (red) and "not key" (blue) are known.
 *   marker  the frame on screen
 *   B       with two items selected, the second item's bars are drawn as outlines on top
 * Click to go to a frame, Shift+drag to select a range: the line above the bars then shows the
 * range's total bytes, bitrate, frames per type, the largest and smallest frame and, with a second
 * item, the difference in bytes.
 * See docs/ai/30-designs/ui-redesign-decisions.md D-18, D-19, D-22.
 */
class FrameTimeline : public QWidget
{
  Q_OBJECT

public:
  explicit FrameTimeline(QWidget *parent = nullptr);

  struct Bar
  {
    qint64  bytes{-1}; //!< -1: unknown (raw video)
    int     type{-1};  //!< One of the Type values below
    QString typeName;
  };
  enum Type
  {
    Unknown = -1,
    Key,        //!< AV1 KEY, H.264 / HEVC I / IDR
    IntraOnly,  //!< AV1 INTRA_ONLY
    Inter,      //!< AV1 INTER, H.264 / HEVC P
    Switch,     //!< AV1 SWITCH
    BiPred,     //!< H.264 / HEVC B
    ShowExisting
  };

  // What the range selection adds up to.
  struct Summary
  {
    int     first{}, last{};
    qint64  bytes{};
    double  kbps{};
    int     largest{-1}, smallest{-1};
    QString perType;
    std::optional<double> diffPercentB; //!< (B - A) / A over the same frames
  };

  const std::vector<Bar> &bars() const { return this->barsA; }
  std::optional<Summary>  selectionSummary() const;
  void                    setLogScale(bool on);
  bool                    logScale() const { return this->log; }

public slots:
  void currentSelectedItemsChanged(playlistItem *item1, playlistItem *item2);
  void setCurrentFrame(int frameIdx);
  // The bitstream parser's model of the first selected item (the one the Info pane shows).
  void setBitstreamModel(QAbstractItemModel *model);
  void selectRange(int first, int last);
  void clearSelection();

signals:
  void seekRequested(int frameIdx);

protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  bool event(QEvent *event) override;

private:
  std::vector<Bar> barsOf(playlistItem *item) const;
  void             reload();
  void             refreshTypes();
  int              frameAt(int x) const;
  QRect            barArea() const;

  QPointer<playlistItem>       itemA, itemB;
  std::vector<Bar>             barsA, barsB;
  QPointer<QAbstractItemModel> model;
  FrameObuIndex                obuIndex;
  FrameAvcIndex                avcIndex;
  QTimer                       typeRefresh; //!< The parser fills its model in the background
  int                          current{-1};
  std::optional<std::pair<int, int>> selection;
  int                          dragStart{-1};
  bool                         log{};
};

} // namespace bda::integration
