// Global motion - run the frame-level estimator over a source clip and look at what it found.
//
// The GUI side of src/me/GlobalMotion (design: docs/ai/30-designs/global-motion-design.md). One
// item at a time: a raw YUV item, or a compressed stream with its original attached - the same
// items the ME panel works on, because the estimator searches the source, not a reconstruction.
//
// The clip is read on a worker thread straight from its file with the Qt-free YuvLumaReader, so a
// run never moves the decoder or the frame buffer the viewer is drawing from.
//
// Two tables: one row per frame, and the 4x4 windows of the selected frame. Selecting a frame moves
// the main window there and draws the windows over the picture (GmStatisticsAdapter); moving the
// main window to another frame redraws them for that frame.
#pragma once

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "integration/StreamDiffWindow.h" // StreamDiffResult - the same refusal shape
#include "me/GlobalMotion.h"

class playlistItem;

namespace bda::integration
{

class GlobalMotionWindow : public QDialog
{
  Q_OBJECT

public:
  explicit GlobalMotionWindow(QWidget *parent = nullptr);
  ~GlobalMotionWindow() override;

  /* Take the selection's one item as the clip to analyse. Refuses, with the reason, anything that
   * is not exactly one item the estimator can read. Does not start a run.
   */
  StreamDiffResult setTarget(const QList<playlistItem *> &selection);

  //!< Start estimating over the frame range in the controls. False when nothing could start.
  bool run();
  void stop();
  bool busy() const { return this->running; }

  //!< Results so far, keyed by display frame (poc). Copied under the lock.
  std::map<int, me::GmFrameResult> results() const;

  //!< Write every finished frame as odyssey's CSV. False with the reason when it could not.
  bool exportCsv(const QString &path, QString *error = nullptr) const;

  //!< Select the frame's row, as clicking it would.
  void selectFrame(int frameIdx);

public slots:
  //!< The main window shows this item at this frame; redraw the windows over it.
  void setCurrentFrame(playlistItem *item, int frameIdx);

signals:
  //!< Emitted from the worker; the slot below picks it up on the GUI thread.
  void frameDone();
  void runFinished();

  //!< Ask the main window to show this item at this frame.
  void showFrameRequested(playlistItem *item, int frameIdx);
  //!< The overlay changed; the view needs a repaint, nothing more.
  void overlayChanged();

protected:
  void closeEvent(QCloseEvent *event) override;

private slots:
  void addFinishedFrames();
  void finishRun();
  void frameRowSelected();

private:
  me::GmParams params() const;
  void         showWindows(int frameIdx);
  void         drawOverlay(int frameIdx);

  QLabel *      summary{};
  QLabel *      progress{};
  QSpinBox *    firstFrame{}, *lastFrame{}, *range{}, *refine{};
  QComboBox *   smoothing{}, *brightness{};
  QPushButton * runButton{}, *stopButton{}, *exportButton{};
  QTableWidget *frames{}, *windows{};

  QPointer<playlistItem> item;
  QString                path;    //!< The file the luma is read from.
  bool                   isY4m{};
  int                    width{}, height{}, bitDepth{8};
  int                    frameCount{};

  mutable std::mutex               lock;
  std::map<int, me::GmFrameResult> done; //!< Written by the worker under lock.
  int                              shownRows{};
  QString                          workerError;
  std::atomic<bool>                cancel{};
  bool                             running{};
  QPointer<QThread>                worker;

  int overlayFrame{-1}; //!< The frame the overlay holds, so a repaint does not refill it.
};

} // namespace bda::integration
