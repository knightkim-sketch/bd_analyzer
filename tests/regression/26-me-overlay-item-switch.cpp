// Regression: the ME overlay across a playlist selection change, and whether it is actually drawn.
//
// Three bugs, all reported from the same session and all about the overlay:
//
//   * Selecting a second item while the overlay was on aborted with
//       StatisticUIHandler.cpp: Assertion `spacerItems[0] != nullptr' failed
//     An item is handed its overlay types as soon as it becomes the estimate's target. If nothing
//     has asked that item for a properties widget yet, its statistics controls do not exist, and
//     the rebuild walks into the branch that removes a spacer which was never added. The first
//     block below drives exactly that state; the window-level part after it covers the path the
//     report came from.
//   * The vectors were registered, stored and painted, and still invisible: the type asked for
//     scaleVectorToZoom, which the painter reads as `width * zoomFactor / 8`, turning a 2 pixel
//     line into a quarter of one at the 1:1 zoom.
//   * Ticking the one "ME" row also switched on the two cost overlays, because a row's checkbox
//     sets render on every type sharing its uiGroup and all three kinds shared one. The arrows
//     were drawn underneath a colour-mapped rectangle over every block.
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <iostream>
#include <cmath>
#include <set>
#include <string>
#include <unistd.h>

#include "integration/MeStatisticsAdapter.h"
#include "integration/MotionEstimationWidget.h"
#include "playlistitem/playlistItemRawFile.h"
#include "statistics/StatisticsDataPainting.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
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
/* The clips are written here rather than taken as arguments, because the check further down needs
 * content that actually moves. A static test pattern gives one non-zero vector in the whole frame,
 * which is a weak thing to hang "the arrows are visible" on.
 */
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

  QByteArray luma;
  luma.resize(w * h);
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

QCheckBox *checkBoxWithText(QWidget *parent, const QString &text)
{
  for (auto *box : parent->findChildren<QCheckBox *>())
    if (box->text() == text)
      return box;
  return nullptr;
}
const stats::StatisticsType *findType(stats::StatisticsData &data, int id)
{
  for (const auto &t : data.getStatisticsTypes())
    if (t.typeID == id)
      return &t;
  return nullptr;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  /* --- the abort itself -------------------------------------------------------------------
   *
   * An item whose properties widget has never been asked for, given overlay types. No window
   * involved: the report came through the playlist, but the abort is one call deep and pinning it
   * here keeps it pinned no matter which sequence of clicks gets there.
   */
  {
    const auto solo = writePanningClip("bd-me-solo_192x128_yuv420p.yuv", 192, 128, 4, 2, 1);
    auto      *item = new playlistItemRawFile(solo);
    check(!item->propertiesWidgetCreated(), "an item that was never asked for its properties");

    bda::me::BlockSizeSet sizes;
    sizes.add(bda::me::BlockSize::Blk64);
    check(bda::integration::syncMeStatTypes(item->getStatisticsData(),
                                            bda::me::Algorithm::SvtIntegerMe,
                                            sizes),
          "is still given overlay types");

    // Aborted here before the guard went in.
    item->getStatisticsUIHandler()->updateStatisticsHandlerControls();
    std::cout << "  ok    updating controls that do not exist yet is a no-op, not an abort"
              << std::endl;

    /* And they are built from the current types when they are finally needed - the guard skips the
     * update, it does not lose the types.
     */
    item->getPropertiesWidget();
    check(item->propertiesWidgetCreated(), "the controls come up later");
    item->getStatisticsUIHandler()->updateStatisticsHandlerControls();
    std::cout << "  ok    and updating them once they exist still works" << std::endl;

    delete item;
    QFile::remove(solo);
  }

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1400, 900);
  w->show();
  // Both pan, by different amounts, so neither estimate is a frame of zero vectors.
  const auto clipA = writePanningClip("bd-me-switch-a_320x240_yuv420p.yuv", 320, 240, 8, 3, 2);
  const auto clipB = writePanningClip("bd-me-switch-b_192x128_yuv420p.yuv", 192, 128, 8, -2, 1);
  check(!clipA.isEmpty() && !clipB.isEmpty(), "the two panning clips were written");
  if (clipA.isEmpty() || clipB.isEmpty())
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  w->loadFiles({clipA, clipB});
  settle(2000);

  auto *panel = w->findChild<MotionEstimationWidget *>();
  auto *tree  = w->findChild<PlaylistTreeWidget *>();
  check(panel != nullptr && tree != nullptr, "the window came up with the ME panel");
  if (!panel || !tree)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  const auto items = tree->getAllPlaylistItems();
  check(items.size() == 2, "both raw YUV items are in the playlist");
  if (items.size() != 2)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }
  auto *itemA = dynamic_cast<playlistItemRawFile *>(items[0]);
  auto *itemB = dynamic_cast<playlistItemRawFile *>(items[1]);
  check(itemA != nullptr && itemB != nullptr, "and both are raw file items");
  if (!itemA || !itemB)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  // --- run an estimate on the first item -------------------------------------------------------
  tree->setCurrentItem(itemA);
  settle(500);
  if (auto *playback = w->findChild<PlaybackController *>())
    playback->setCurrentFrameAndUpdate(3);
  settle(300);

  auto *enable = checkBoxWithText(panel, "Compute in the background");
  check(enable != nullptr, "the enable checkbox is there");
  if (!enable)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }
  enable->setChecked(true);
  settle(20000);

  const int vecId = bda::integration::meVectorTypeId(bda::me::Algorithm::SvtIntegerMe,
                                                     bda::me::BlockSize::Blk64);
  check(itemA->getStatisticsData().hasDataForTypeID(vecId), "the first item has its vectors");

  /* --- one row per kind ------------------------------------------------------------------------
   *
   * The panel gives a uiGroup one checkbox that drives every type in it, so the vectors and the
   * two cost overlays have to be in groups of their own or "show the vectors" means "show all
   * three".
   */
  {
    auto      &data     = itemA->getStatisticsData();
    const auto costId   = bda::integration::meCostTypeId(bda::me::Algorithm::SvtIntegerMe,
                                                       bda::me::BlockSize::Blk64);
    const auto commonId = bda::integration::meCommonSadTypeId(bda::me::Algorithm::SvtIntegerMe,
                                                              bda::me::BlockSize::Blk64);
    const auto *vec     = findType(data, vecId);
    const auto *cost    = findType(data, costId);
    const auto *common  = findType(data, commonId);
    check(vec && cost && common, "all three overlay kinds are registered");
    if (vec && cost && common)
    {
      std::set<QString> groups{vec->uiGroup, cost->uiGroup, common->uiGroup};
      check(groups.size() == 3, "each kind has a row of its own");
      check(vec->render, "the vectors are drawn - the user asked for them by ticking the box");
      check(!cost->render && !common->render,
            "and the cost overlays are not, because they paint over the picture");
      check(!vec->scaleVectorToZoom,
            "the vector line is not scaled to zoom - at 1:1 that divided its width by eight");
      check(vec->vectorStyle.width >= 1.0, "so a visible width survives to the painter");
    }
  }

  /* --- and an arrow really lands on the canvas -------------------------------------------------
   *
   * Registered, stored and rendered are three different things; this checks the last one by
   * painting the overlay alone onto white and looking for the colour it is drawn in.
   */
  {
    auto &data = itemA->getStatisticsData();
    QImage img(1200, 1200, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.translate(600, 600);
    stats::paintStatisticsData(&p, data, data.getFrameIndex(), 1.0);
    p.end();

    int arrowPixels = 0;
    for (int y = 0; y < img.height(); ++y)
      for (int x = 0; x < img.width(); ++x)
      {
        const auto c = img.pixelColor(x, y);
        // The SVT overlay draws in green; the cost overlays would show up as filled blocks.
        if (c.green() > 100 && c.green() > c.red() + 40 && c.green() > c.blue() + 40)
          ++arrowPixels;
      }
    check(arrowPixels > 50, "the vectors are actually painted (" + std::to_string(arrowPixels) +
                                " arrow pixels)");
  }

  /* --- switching to the other item ------------------------------------------------------------
   *
   * This is the crash. The second item is handed its overlay types on selection, before anything
   * built its statistics controls, and the rebuild asserted on a spacer that was never added.
   */
  tree->setCurrentItem(itemB);
  settle(20000);
  std::cout << "  ok    selecting a second item with the overlay on did not abort" << std::endl;

  check(itemB->getStatisticsData().hasDataForTypeID(vecId),
        "and the second item gets an estimate of its own");
  check(itemB->getStatisticsUIHandler() != itemA->getStatisticsUIHandler(),
        "each item keeps its own statistics controls");

  // Back again, which is the same path with the controls now built on both sides.
  tree->setCurrentItem(itemA);
  settle(3000);
  std::cout << "  ok    and switching back did not abort either" << std::endl;

  QFile::remove(clipA);
  QFile::remove(clipB);
  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
