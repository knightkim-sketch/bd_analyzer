// Find diff - compare two streams and say where they first part company.
//
// Runs the pipeline of docs/ai/30-designs/stream-diff-design.md section 4.5 in order, each step
// narrowing the next:
//   1-2  sequence / frame header syntax, OBU by OBU
//   3    OBU payloads as bytes - which coded frame first differs, and from which byte
//   4    that one frame decoded on both sides - which SB(row, col) and MI(row, col) first differ
//   D    reconstructed pictures, frame by frame (opt-in: it decodes both whole streams)
//
// A row that names a place can be activated, and the application is then put in that state - the
// stream and frame shown, the block selected - instead of leaving the reader to find and click it.
#pragma once

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>

#include <atomic>
#include <optional>
#include <string>

#include "diff/ObuPayloadDiff.h"
#include "diff/SyntaxDiff.h"
#include "integration/StreamDiffSteps.h"

class playlistItem;

namespace bda::integration
{

/* Why a comparison was refused, in words that can go straight in front of the user.
 *
 * Same shape as BdRateGroupResult: the action is a deliberate menu press, so a refusal has to say
 * what to do differently rather than just fail.
 */
struct StreamDiffResult
{
  bool    accepted{};
  QString message;

  bool ok() const { return this->accepted; }
};

//!< A place a result row points at. pixel is (x, y) in luma samples; unset for a whole frame.
struct StreamDiffLocation
{
  int                   frameIdx{-1};
  std::optional<QPoint> pixel;
};

//!< Everything one run found, step by step. Plain data, written by the worker.
struct StreamDiffReport
{
  bda::diff::SectionDiffResult headers;
  std::size_t                  elementsA{}, elementsB{};

  bool                         payloadCompared{};
  bda::diff::PayloadDiffResult payload;
  std::size_t                  obusA{}, obusB{};

  /* Step 4. blockNote says why it did not run when it did not - the first differing OBU is not a
   * frame, or nothing displays it - so the window never just leaves the section out.
   */
  bool            blockCompared{};
  QString         blockNote;
  BlockStepResult blocks;

  bool            reconCompared{};
  ReconStepResult recon;
};

class StreamDiffWindow : public QDialog
{
  Q_OBJECT

public:
  explicit StreamDiffWindow(QWidget *parent = nullptr);
  ~StreamDiffWindow() override;

  /* Start comparing the selection. Returns the reason when it will not - the window may not even
   * be open yet, so the caller decides how to show it.
   */
  StreamDiffResult compare(const QList<playlistItem *> &selection);

  /* What the last finished comparison found. Exists so the comparison can be tested without a
   * display - everything else this window does is drawing.
   */
  const bda::diff::SectionDiffResult &lastResult() const { return this->report.headers; }
  const StreamDiffReport &            lastReport() const { return this->report; }
  bool                                busy() const { return this->running; }

  /* The place "Go to first difference" goes to: the first differing block of step 4 when there is
   * one, else the first frame whose pictures differ. Unset when neither step found anything.
   */
  std::optional<StreamDiffLocation> firstDifference() const;

  /* Put the application at a result's place, with the streams the two toggles name. Public so the
   * jump can be driven without clicking a row.
   */
  void locate(const StreamDiffLocation &where);

  //!< 0 = A, 1 = B. The same choice the toggles offer.
  void setBlockInfoStream(int stream);
  void setSyntaxInfoStream(int stream);

signals:
  //!< Emitted from the worker thread; the slots below run it back on the GUI thread.
  void comparisonFinished();
  void progressChanged(QString text);

  /* Ask the main window to show this place.
   *
   * blockStream is the stream the Block Info, Frame Info and hexdump panes and the highlight follow;
   * otherStream rides along as the second selection, so the split view and the two-stream panes
   * still show both. syntaxStream is the one the Bitstream Analysis pane should show - a separate
   * choice, because reading one stream's header tree beside the other stream's block is a real use.
   */
  void locateRequested(playlistItem *blockStream,
                       playlistItem *otherStream,
                       playlistItem *syntaxStream,
                       int           frameIdx,
                       QPoint        pixelPos);

protected:
  void closeEvent(QCloseEvent *event) override;

private slots:
  void showResult();
  void showProgress(const QString &text);
  void activateRow(QTreeWidgetItem *row);
  void reconToggled(bool checked);

private:
  void setBusy(const QString &what);
  void startWorker(bool headersAndBlocks, bool recon);
  void updateControls();

  void addHeaderSection(QString &summary);
  void addPayloadSection(QString &summary);
  void addBlockSection(QString &summary);
  void addReconSection(QString &summary);

  QLabel *      summary{};
  QLabel *      progress{};
  QTreeWidget * tree{};
  QCheckBox *   compareCdf{};
  QCheckBox *   compareRecon{};
  QPushButton * goToFirst{};
  QButtonGroup *blockInfoChoice{};
  QButtonGroup *syntaxInfoChoice{};

  QString                nameA, nameB;
  QPointer<playlistItem> itemA, itemB;

  /* Written by the worker, read by showResult() after the queued signal. Only one comparison runs
   * at a time - the controls stay disabled until it finishes - so a mutex would guard nothing the
   * signal does not already order.
   */
  StreamDiffReport  report;
  QString           workerError;
  bool              running{};
  std::atomic<bool> cancelRequested{};
  //!< Unticking the picture comparison stops that part alone.
  std::atomic<bool> reconCancelRequested{};
  QPointer<QThread> worker;

  //!< The place last jumped to, so a toggle can re-show it with the other stream.
  std::optional<StreamDiffLocation> lastLocation;
};

} // namespace bda::integration
