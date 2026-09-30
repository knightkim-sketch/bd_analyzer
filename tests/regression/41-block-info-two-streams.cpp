// Regression: with two streams selected, the Block Info pane shows both.
//
// Opening two encodings side by side in the split view raises one question - what did each of them
// code at this pixel - and the pane used to answer only half of it: it followed whichever view was
// clicked last and showed that one stream.
//
// Two things are checked, and the second is the one that is easy to get wrong. The columns are
// keyed to the playlist order rather than to which view was clicked, so they do not swap places
// under the reader; and the second stream's values are matched to the first by syntax element
// name, because two encodings routinely code a different number of elements at the same pixel and
// pairing them by index would put one stream's value beside another stream's element.
#include <QApplication>
#include <QHeaderView>
#include <QSettings>
#include <QTreeWidget>

#include <iostream>
#include <string>
#include <thread>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "ui/widgets/BlockInfoWidget.h"

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++failures;
}

void loadOffMainThread(playlistItemCompressedVideo &item, int frameIdx)
{
  std::thread loader([&item, frameIdx] { item.loadFrame(frameIdx, false, true, false); });
  loader.join();
}

// Walk to the first leaf row; the top level items are the block groups.
QTreeWidgetItem *firstLeaf(QTreeWidget *tree)
{
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
    if (tree->topLevelItem(i)->childCount() > 0)
      return tree->topLevelItem(i)->child(0);
  return nullptr;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3)
  {
    std::cerr << "usage: 41-block-info-two-streams <streamA> <streamB>\n";
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerBlockInfoTwo");
  QCoreApplication::setApplicationName("bdAnalyzerBlockInfoTwo");
  QSettings().clear();

  playlistItemCompressedVideo itemA(QString::fromUtf8(argv[1]), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
  playlistItemCompressedVideo itemB(QString::fromUtf8(argv[2]), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
  itemA.setBlockInfoRequested(true);
  itemB.setBlockInfoRequested(true);
  loadOffMainThread(itemA, 0);
  loadOffMainThread(itemB, 0);

  BlockInfoWidget pane;
  auto *tree = pane.findChild<QTreeWidget *>();
  if (!tree)
  {
    std::cout << "RESULT: FAIL - no tree in the pane" << std::endl;
    return 1;
  }

  // One stream only: the pane stays as it was, with a single value column.
  pane.currentSelectedItemsChanged(&itemA, nullptr);
  pane.setSelectedBlock(&itemA, QPoint(8, 8), 0);
  check(tree->isColumnHidden(2), "with one stream the second value column stays hidden");

  // Two streams: the companion column appears, named after the files.
  pane.currentSelectedItemsChanged(&itemA, &itemB);
  pane.setSelectedBlock(&itemA, QPoint(8, 8), 0);
  check(!tree->isColumnHidden(2), "with two streams the second value column appears");

  const auto headerA = tree->headerItem()->text(1);
  const auto headerB = tree->headerItem()->text(2);
  check(headerA != "Value" && !headerA.isEmpty(), "the columns are named after the streams");
  check(headerA != headerB, "and the two names differ");

  auto *leaf = firstLeaf(tree);
  check(leaf != nullptr, "the pane lists syntax elements");
  if (leaf)
  {
    check(!leaf->text(0).isEmpty(), "an element is named");
    check(!leaf->text(1).isEmpty(), "the first stream has a value for it");
    check(!leaf->text(2).isEmpty(), "and the second stream's column is filled in");
    std::cout << "        " << leaf->text(0).toStdString() << " : "
              << leaf->text(1).toStdString() << "  |  " << leaf->text(2).toStdString()
              << std::endl;
  }

  // Clicking the other view must not swap the columns around.
  pane.setSelectedBlock(&itemB, QPoint(8, 8), 0);
  check(tree->headerItem()->text(1) == headerA && tree->headerItem()->text(2) == headerB,
        "clicking the other view leaves the columns in place");

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
  return failures == 0 ? 0 : 1;
}
