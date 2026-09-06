// The Motion Estimation panel.
//
// Lives in src/integration rather than in the upstream tree because it is exactly what that
// boundary is for: a Qt widget that knows both about our ME core and about being a YUView dock.
// mainwindow.ui refers to it as a custom widget; the .pro adds src/ to the include path.
#pragma once

#include <QWidget>

#include "me/MeTypes.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QSpinBox;

namespace bda::integration
{

class MotionEstimationWidget : public QWidget
{
  Q_OBJECT

public:
  explicit MotionEstimationWidget(QWidget *parent = nullptr);

  /* Whether the background estimate should run at all.
   *
   * Not called isEnabled() - QWidget already has that, and shadowing it would be a trap. Off by
   * default: a whole-frame estimate is expensive enough that starting one the moment a file is
   * opened would be a surprise, so the user asks for it.
   */
  bool autoComputeEnabled() const;

  /* The current control state as the estimators want it. The cancel token is not filled in here -
   * whoever runs the estimate owns that.
   */
  me::MeParams params() const;

  /* Grey the controls out with a reason - a compressed stream, a single-frame item, a reference
   * that falls outside the sequence. Saying why beats a panel that simply does nothing.
   *
   * Both are idempotent, and that matters: they are called from the frame-change signal, which
   * arrives from the view's paint path. Calling setEnabled() on the children every time posts
   * EnabledChange events and repaints, and a repaint that leads back to a frame change closes the
   * loop - which is exactly how this hung three MainWindow tests before the guard went in.
   */
  void setUnavailable(const QString &reason);
  void setAvailable();

  // Progress or outcome, shown under the controls.
  void setStatus(const QString &text);

  /* How far the current estimate has got, and whether there is one.
   *
   * A whole-frame search on a large picture takes long enough that "nothing has appeared yet" and
   * "there is no overlay for this frame" look identical without it. The bar answers that
   * regardless of how the run was started.
   */
  void setProgressIdle(const QString &text);
  void setProgressRunning(int doneSuperblocks, int totalSuperblocks);
  void setProgressComplete(const QString &text);

signals:
  /* Any control changed. The window responds by cancelling whatever estimate is in flight and
   * starting a new one - which is why this is one signal rather than one per control: every change
   * has the same consequence.
   */
  void parametersChanged();

private:
  void emitIfLive();

  QCheckBox *enable_{};
  QSpinBox  *interval_{};
  QComboBox *algorithm_{};
  QCheckBox *size8_{};
  QCheckBox *size16_{};
  QCheckBox *size32_{};
  QCheckBox *size64_{};
  QCheckBox    *staticBypass_{};
  QProgressBar *progress_{};
  QLabel       *status_{};

  //!< Tracks what the controls are already showing, so a repeated call does nothing.
  bool controlsEnabled_{true};
};

} // namespace bda::integration

/* uic writes the member declaration for a promoted widget using the bare class name, so the form
 * generated from mainwindow.ui says `MotionEstimationWidget *`. The alias lets the class keep its
 * namespace instead of being flattened to global scope just to satisfy the generated code.
 */
using MotionEstimationWidget = bda::integration::MotionEstimationWidget;
