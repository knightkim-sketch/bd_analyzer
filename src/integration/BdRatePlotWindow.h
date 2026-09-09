// The SB BD-rate window: rate-distortion curves per group, with the BD-rate against the anchor.
//
// A window of its own rather than a dock, because it holds three plots side by side and a table -
// it needs the room, and it is opened for a comparison rather than kept around.
#pragma once

#include <QDialog>
#include <QPoint>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <map>
#include <optional>
#include <vector>

#include "bdrate/BdRateMath.h"
#include "integration/BdRateCollector.h"
#include "integration/BdRateGroups.h"

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace bda::integration
{

/* One plot: log10(bits) across, PSNR up, one curve per group.
 *
 * The curve drawn is the fitted polynomial, not straight segments between the points, because the
 * BD-rate is the integral of that fit - drawing the segments would show a curve the number does not
 * come from.
 */
class BdRateCurvePanel : public QWidget
{
  Q_OBJECT

public:
  struct Curve
  {
    QString                        name;
    std::vector<bdrate::RatePoint> points;
    bool                           isAnchor{};
    //!< Against the anchor. Absent for the anchor itself and where the metric is undefined.
    std::optional<bdrate::BdRateResult> bdRate;
  };

  explicit BdRateCurvePanel(const QString &title, QWidget *parent = nullptr);

  void setCurves(std::vector<Curve> curves);
  //!< Shown instead of the plot when there is nothing to draw, with the reason.
  void setMessage(const QString &message);

  QSize sizeHint() const override { return QSize(320, 260); }

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  QString            title;
  QString            message;
  std::vector<Curve> curves;
};

class BdRatePlotWindow : public QDialog
{
  Q_OBJECT

public:
  explicit BdRatePlotWindow(QWidget *parent = nullptr);

  /* Add a selection as a new curve. Returns the reason when it could not be added, so the caller
   * can put it in front of the user - the window may not even be open yet.
   */
  BdRateGroupResult addGroup(const QList<playlistItem *> &selection);

  bool empty() const { return this->groups.empty(); }

public slots:
  /* Which frame the window describes. The Frame and SB panels are about one frame, so they follow
   * the view rather than pinning whatever was showing when the window opened.
   */
  void setCurrentFrame(int frameIdx);
  //!< The superblock the user clicked, in pixels. The SB panel follows it.
  void setSelectedPosition(const QPoint &pixelPos, bool valid);
  //!< Re-collect and redraw. Cheap enough to call whenever something might have changed.
  void refresh();

private:
  void rebuildGroupTable();
  void updatePanels();
  void collect();

  QCheckBox *showSb{};
  QCheckBox *showFrame{};
  QCheckBox *showSequence{};
  QLabel    *status{};

  QHBoxLayout      *panelRow{};
  BdRateCurvePanel *sbPanel{};
  BdRateCurvePanel *framePanel{};
  BdRateCurvePanel *sequencePanel{};

  QTableWidget *groupTable{};
  QTableWidget *valueTable{};

  std::vector<BdRateGroup> groups;
  int                      anchorIndex{0};

  int                   frameIdx{-1};
  std::optional<QPoint> selectedPos;
  BdRateFrameData       collected;

  /* Set while rebuildGroupTable() is filling the widget, so the itemChanged handler can tell a
   * programmatic fill from a user edit. Without it, renaming a group from code triggers the rename
   * handler, which rebuilds the table, which fires again.
   */
  bool fillingTable{};

  /* The SSE for a frame is computed off the GUI thread, so the first collection of a frame comes
   * back with `pending` set and nothing to draw. This asks again until it arrives.
   *
   * Bounded rather than endless: if the statistics never turn up - a frame that cannot be decoded,
   * an original that went away - the window should settle on saying so instead of polling for the
   * rest of the session.
   */
  QTimer retryTimer;
  int    retriesLeft{};

  /* The sequence sweep. Owned here rather than by the collector, because it is a long running
   * thing with a progress bar and a cancel - it belongs to the window that shows both.
   */
  BdRateSequenceSweeper *sweeper{};
  QProgressBar          *sweepProgress{};
  QPushButton           *sweepCancel{};
};

} // namespace bda::integration
