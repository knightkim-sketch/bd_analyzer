// Find diff - compare two streams and say where their syntax first parts company.
//
// This is the window for layer A of docs/ai/30-designs/stream-diff-design.md: the sequence and
// frame header syntax, compared OBU by OBU. The superblock bit window (B), the per block syntax
// (C) and the recon comparison (D) are not wired yet; their controls are present and disabled so
// the window does not quietly imply it did more than it did.
#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QList>
#include <QTreeWidget>

#include <string>

#include "diff/SyntaxDiff.h"

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

class StreamDiffWindow : public QDialog
{
  Q_OBJECT

public:
  explicit StreamDiffWindow(QWidget *parent = nullptr);

  /* Start comparing the selection. Returns the reason when it will not - the window may not even
   * be open yet, so the caller decides how to show it.
   */
  StreamDiffResult compare(const QList<playlistItem *> &selection);

  /* What the last finished comparison found. Exists so the comparison can be tested without a
   * display - everything else this window does is drawing.
   */
  const bda::diff::SectionDiffResult &lastResult() const { return this->result; }
  bool                                busy() const { return this->running; }

signals:
  //!< Emitted from the worker thread; the slot below runs it back on the GUI thread.
  void comparisonFinished();

private slots:
  void showResult();

private:
  void setBusy(const QString &what);

  QLabel *      summary{};
  QTreeWidget * tree{};
  QCheckBox *   compareCdf{};
  QCheckBox *   compareRecon{};

  QString nameA, nameB;

  /* Written by the worker, read by showResult() after the queued signal. Only one comparison runs
   * at a time - the window is modal to its own work in that the controls stay disabled until it
   * finishes - so a mutex would guard nothing the signal does not already order.
   */
  bda::diff::SectionDiffResult result;
  QString                      workerError;
  std::size_t                  elementsA{}, elementsB{};
  bool                         running{};
};

} // namespace bda::integration
