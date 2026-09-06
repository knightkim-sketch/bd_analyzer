// Regression: clicking a block on a raw YUV item shows that block's reproduced motion vector.
//
// The bitstream side of this already worked - clicking a compressed item lists its coding syntax -
// but a raw item answered "no block information", because playlistItem::getBlockInfoAt() defaults
// to "this item has no statistics" and only the compressed item overrode it. So the estimate was
// drawn on the picture and could not be read off it.
//
// What this pins down:
//   * A raw item answers the same query, over the same statistics container the overlay draws
//     through, so the click path from the view to the Block Info pane needs nothing item-specific.
//   * Exactly the block sizes ticked in the ME panel are listed - the checkboxes decide which
//     types exist, and the query lists the types.
//   * One row per size, not three: the vector and the two costs describe one decision about one
//     block, the way the pane already folds a bitstream's L0/L1 vector pair into a single row.
//   * A frame whose estimate has not arrived is reported as having nothing, rather than as having
//     the previous frame's vectors. A plausible wrong vector is worse than none here.
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTreeWidget>
#include <cmath>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/MotionEstimationWidget.h"
#include "playlistitem/playlistItemRawFile.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/BlockInfoWidget.h"
#include "ui/widgets/PlaylistTreeWidget.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
void settle(int ms)
{
  for (int i = 0; i < ms / 10; ++i)
  {
    QCoreApplication::processEvents();
    usleep(10000);
  }
}
QCheckBox *checkBoxWithText(QWidget *parent, const QString &text)
{
  for (auto *box : parent->findChildren<QCheckBox *>())
    if (box->text() == text)
      return box;
  return nullptr;
}

// Real motion, so the vectors under the click are not all zero. Same generator as test 26.
QString writePanningClip(const QString &name, int w, int h, int frames, int dx, int dy)
{
  const QString path = QDir::tempPath() + "/" + name;
  QFile         f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return {};
  const auto texel = [](int x, int y) {
    const double v = 128.0 + 60.0 * std::sin(x / 7.3) * std::cos(y / 5.1) +
                     40.0 * std::sin((x + 2.0 * y) / 11.7) + 25.0 * std::cos((3.0 * x - y) / 4.3);
    return static_cast<char>(static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : int(v))));
  };
  QByteArray       luma(w * h, '\0');
  const QByteArray chroma(2 * (w / 2) * (h / 2), '\x80');
  for (int n = 0; n < frames; ++n)
  {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        luma[y * w + x] = texel(x + dx * n, y + dy * n);
    f.write(luma);
    f.write(chroma);
  }
  f.close();
  return path;
}

// Every row of the pane's tree, flattened to "name = value".
std::vector<QString> paneRows(BlockInfoWidget *pane)
{
  std::vector<QString> rows;
  for (auto *tree : pane->findChildren<QTreeWidget *>())
    for (auto it = QTreeWidgetItemIterator(tree); *it; ++it)
      if ((*it)->childCount() == 0)
        rows.push_back((*it)->text(0) + " = " + (*it)->text(1));
  return rows;
}
size_t rowsContaining(const std::vector<QString> &rows, const QString &needle)
{
  size_t n = 0;
  for (const auto &r : rows)
    if (r.contains(needle))
      ++n;
  return n;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  const auto clip = writePanningClip("bd-me-click_320x240_yuv420p.yuv", 320, 240, 8, 3, 2);
  check(!clip.isEmpty(), "the panning clip was written");
  if (clip.isEmpty())
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1400, 900);
  w->show();
  w->loadFiles({clip});
  settle(2000);

  auto *panel    = w->findChild<MotionEstimationWidget *>();
  auto *pane     = w->findChild<BlockInfoWidget *>();
  auto *tree     = w->findChild<PlaylistTreeWidget *>();
  auto *playback = w->findChild<PlaybackController *>();
  check(pane != nullptr, "the Block Info pane is in the window");
  auto *item = tree && !tree->getSelectedItems().empty()
                   ? dynamic_cast<playlistItemRawFile *>(tree->getSelectedItems()[0])
                   : nullptr;
  check(item != nullptr && panel != nullptr && playback != nullptr, "and the raw item came up");
  if (!pane || !item || !panel || !playback)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  /* --- before any estimate ---------------------------------------------------------------------
   *
   * Frame 0 has no reference, so nothing runs there and there is nothing to report. The pane must
   * say so rather than answer with whatever the container happens to hold.
   */
  {
    const auto info = item->getBlockInfoAt(QPoint(100, 100), 0);
    check(!info.isValid, "with no estimate for the frame, the query comes back empty");
  }

  // --- run one, with two block sizes ticked ----------------------------------------------------
  playback->setCurrentFrameAndUpdate(3);
  settle(300);
  checkBoxWithText(panel, "8")->setChecked(true); // 64 is on by default
  checkBoxWithText(panel, "Compute in the background")->setChecked(true);
  settle(25000);

  {
    const auto info = item->getBlockInfoAt(QPoint(100, 100), 3);
    check(info.isValid, "after the estimate, the clicked pixel has a block");
    check(info.codingBlockRect.width() == 8,
          "and the smallest ticked size is the one highlighted - it is the most specific");

    check(info.entries.size() == 2, "one row per ticked block size, not one per statistics type");
    bool has8 = false, has64 = false;
    for (const auto &e : info.entries)
    {
      if (e.typeName.contains("8x8"))
        has8 = true;
      if (e.typeName.contains("64x64"))
        has64 = true;
      check(e.valueText.startsWith("("),
            "the row leads with the vector: " + e.typeName.toStdString() + " = " +
                e.valueText.toStdString());
      check(e.valueText.contains("common SAD"),
            "with both costs folded in behind it, named so they are not read as one scale");
    }
    check(has8 && has64, "both ticked sizes are listed");
  }

  /* --- the pane really shows it ----------------------------------------------------------------
   *
   * Everything above is the query. This is the widget the user reads, driven the way the view
   * drives it on a click.
   */
  {
    pane->setSelectedBlock(item, QPoint(100, 100), 3);
    settle(300);
    const auto rows = paneRows(pane);
    check(rowsContaining(rows, "ME ") == 2, "the pane lists both sizes");
    check(rowsContaining(rows, "8x8") == 1 && rowsContaining(rows, "64x64") == 1,
          "one row each");
    for (const auto &r : rows)
      std::cout << "        " << r.toStdString() << std::endl;
  }

  /* --- unticking a size takes its row away -----------------------------------------------------
   *
   * The checkboxes are the only switch for which overlays exist, so they have to be the only switch
   * for what the pane lists too - otherwise the two disagree about what was computed.
   */
  {
    checkBoxWithText(panel, "8")->setChecked(false);
    settle(25000);
    const auto info = item->getBlockInfoAt(QPoint(100, 100), 3);
    check(info.isValid, "the estimate is still there after unticking a size");
    check(info.entries.size() == 1, "with one row left");
    check(info.entries.size() == 1 && info.entries[0].typeName.contains("64x64"),
          "and it is the size that is still ticked");
  }

  QFile::remove(clip);
  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
