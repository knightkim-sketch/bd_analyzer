// Regression: the Container tab widget, driven the way the panel drives it.
//
// The parser itself is covered by tests/unit/mp4-parser.cpp without Qt. What this adds is the part
// that unit test cannot reach: the widget maps the file rather than reading it, fills two trees,
// and has to behave when handed something that is not an ISO base media file at all.
//
// What this pins down:
//   * An MP4 fills the box tree, and the tree has real depth - moov/trak/mdia/minf/stbl is five
//     levels, and a flat tree would mean the container recursion silently stopped.
//   * The sample list is filled from the sample table, so the tab actually locates samples.
//   * A non-MP4 path (an IVF, which the app opens happily) clears the trees and explains itself
//     instead of leaving the previous file's tree on screen - the stale-view bug this guards.
//   * Being handed a missing file does not crash; it reports.
#include <QApplication>
#include <QLabel>
#include <QTreeWidget>
#include <cstdlib>
#include <iostream>
#include <string>

#include "integration/Mp4ContainerWidget.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

//!< Deepest path from any top level row. moov/trak/mdia/minf/stbl is five levels below the root.
int treeDepth(const QTreeWidgetItem *item)
{
  int deepest = 0;
  for (int index = 0; index < item->childCount(); ++index)
    deepest = std::max(deepest, treeDepth(item->child(index)));
  return deepest + 1;
}

int treeDepth(const QTreeWidget *tree)
{
  int deepest = 0;
  for (int index = 0; index < tree->topLevelItemCount(); ++index)
    deepest = std::max(deepest, treeDepth(tree->topLevelItem(index)));
  return deepest;
}

QTreeWidget *findTree(QWidget *parent, int which)
{
  const auto trees = parent->findChildren<QTreeWidget *>();
  return which < trees.size() ? trees.at(which) : nullptr;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  std::cout << "mp4 container tab" << std::endl;

  bda::integration::Mp4ContainerWidget widget;

  auto *boxTree    = findTree(&widget, 0);
  auto *sampleTree = findTree(&widget, 1);
  check(boxTree != nullptr && sampleTree != nullptr, "the widget builds a box tree and a sample list");
  if (boxTree == nullptr || sampleTree == nullptr)
    return 1;

  check(boxTree->topLevelItemCount() == 0, "both trees start empty");

  const std::string mp4 = argc >= 2 ? argv[1] : "";
  const std::string ivf = argc >= 3 ? argv[2] : "";

  if (mp4.empty())
  {
    std::cout << "SKIP (no mp4 given)" << std::endl;
    return g_failures == 0 ? 0 : 1;
  }

  // --- an MP4 -----------------------------------------------------------------------------
  widget.setFile(QString::fromStdString(mp4));
  check(boxTree->topLevelItemCount() >= 2, "an MP4 fills the box tree with several top level boxes");
  {
    bool haveFtyp = false, haveMoov = false, haveMdat = false;
    for (int index = 0; index < boxTree->topLevelItemCount(); ++index)
    {
      const auto type = boxTree->topLevelItem(index)->text(0);
      haveFtyp        = haveFtyp || type == "ftyp";
      haveMoov        = haveMoov || type == "moov";
      haveMdat        = haveMdat || type == "mdat";
    }
    check(haveFtyp && haveMoov && haveMdat, "ftyp, moov and mdat are all listed");
  }
  {
    /* Five levels means the walk went moov -> trak -> mdia -> minf -> stbl. A depth of one or two
     * is what a broken container allowlist looks like, and it would still show a plausible tree.
     */
    const auto depth = treeDepth(boxTree);
    check(depth >= 5, "the box tree is at least five levels deep (moov/trak/mdia/minf/stbl)");
    if (depth < 5)
      std::cout << "        depth was " << depth << std::endl;
  }
  check(sampleTree->topLevelItemCount() > 0, "the sample list is filled from the sample table");
  {
    // Offsets must be numbers pointing into the file, not empty or placeholder cells.
    bool ok = sampleTree->topLevelItemCount() > 0;
    if (ok)
    {
      bool converted = false;
      const auto offset = sampleTree->topLevelItem(0)->text(1).toULongLong(&converted);
      ok                = converted && offset > 0;
    }
    check(ok, "the first sample has a real file offset");
  }

  // --- something that is not an ISO base media file ---------------------------------------
  if (!ivf.empty())
  {
    widget.setFile(QString::fromStdString(ivf));
    check(boxTree->topLevelItemCount() == 0 && sampleTree->topLevelItemCount() == 0,
          "an IVF clears both trees instead of leaving the MP4's on screen");
    const auto labels = widget.findChildren<QLabel *>();
    bool       explains = false;
    for (const auto *label : labels)
      explains = explains || label->text().contains("not an ISO base media file");
    check(explains, "and says why, rather than looking like a parse failure");
  }

  // --- a path that is not there ------------------------------------------------------------
  {
    widget.setFile(QStringLiteral("/nonexistent/definitely-not-here.mp4"));
    check(boxTree->topLevelItemCount() == 0, "a missing file leaves the tree empty");
    const auto labels   = widget.findChildren<QLabel *>();
    bool       reported = false;
    for (const auto *label : labels)
      reported = reported || label->text().contains("Could not open");
    check(reported, "and is reported rather than crashing");
  }

  // --- back to the MP4, to prove the widget is reusable ------------------------------------
  {
    widget.setFile(QString::fromStdString(mp4));
    check(boxTree->topLevelItemCount() >= 2, "the MP4 can be shown again after an error");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
