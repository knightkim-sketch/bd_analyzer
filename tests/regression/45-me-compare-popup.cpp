// Regression: Shift+click on a block opens the ME compare popup - the source block next to the
// reference block its vector fetched, one tab per estimate covering the pixel.
//
// What this pins down:
//   * with no estimate for the frame, Shift+click explains what to turn on instead of opening an
//     empty window;
//   * with block ME on two sizes, the popup has one tab per size, smallest first;
//   * the reference block is the one the estimator used: the popup's SAD equals the common SAD the
//     estimator reported, and on a clean pan it is 0 - the block was fetched at the right place
//     and with the right sign;
//   * after a Global motion run, the GM window covering the pixel is added as a tab, cut at the GM
//     window size, and on the same pan its reference matches exactly too.
//
// The click is the view's signal, not a synthetic mouse event, as in tests 27 and 43: the mouse
// path (Shift held on a click) is on the manual checklist in docs/dev/feature-verification.md.
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/GlobalMotionWindow.h"
#include "integration/MeCompareWindow.h"
#include "integration/MotionEstimationWidget.h"
#include "playlistitem/playlistItem.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/views/SplitViewWidget.h"
#include "ui/widgets/PlaylistTreeWidget.h"

using bda::integration::GlobalMotionWindow;
using bda::integration::MeCompareWindow;
using bda::integration::MotionEstimationWidget;

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

double lattice(int ix, int iy, unsigned seed)
{
  unsigned h = unsigned(ix) * 374761393u + unsigned(iy) * 668265263u + seed * 2246822519u;
  h          = (h ^ (h >> 13)) * 1274126177u;
  return double((h ^ (h >> 16)) & 0xffff) / 65535.0;
}

double noise(double x, double y, double grid, unsigned seed)
{
  const double gx = x / grid, gy = y / grid;
  const int    ix = int(std::floor(gx)), iy = int(std::floor(gy));
  const double fx = gx - ix, fy = gy - iy;
  const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
  const double a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
  const double c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
  return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy;
}

/* Frame n shows texel(x + dx n, y + dy n), so a block of frame n is found in frame n - 1 at
 * (x + dx, y + dy). The texture of test 43 - value noise has no period for a wrong vector to hit.
 */
QString writePanningClip(const QString &name, int w, int h, int frames, int dx, int dy)
{
  const QString path = QDir::tempPath() + "/" + name;
  QFile         f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return {};
  const auto texel = [](int x, int y) {
    const double v = 50.0 + 70.0 * noise(x, y, 16, 1) + 50.0 * noise(x, y, 5, 2);
    return static_cast<char>(static_cast<unsigned char>(int(v)));
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

// The number after "common SAD " in a tab's detail line, or -1.
long long reportedSad(const QString &detail)
{
  const auto at = detail.indexOf("common SAD ");
  if (at < 0)
    return -1;
  bool ok = false;
  const auto value =
      detail.mid(at + 11).section(QRegularExpression("[^0-9]"), 0, 0).toLongLong(&ok);
  return ok ? value : -1;
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerMeCompare");
  QCoreApplication::setApplicationName("bdAnalyzerMeCompare");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  // The size and pan of test 43, so Global motion is known to accept (+8, +4) here.
  const auto clip = writePanningClip("bd-me-compare_1280x720_yuv420p.yuv", 1280, 720, 6, 8, 4);
  check(!clip.isEmpty(), "the panning clip was written");
  if (clip.isEmpty())
  {
    std::cout << "RESULT: FAIL" << std::endl;
    return 1;
  }

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1400, 900);
  w->show();
  w->loadFiles({clip});
  settle(2000);

  auto *panel    = w->findChild<MotionEstimationWidget *>();
  auto *tree     = w->findChild<PlaylistTreeWidget *>();
  auto *playback = w->findChild<PlaybackController *>();
  auto *view     = w->findChild<splitViewWidget *>();
  auto *item     = tree && !tree->getSelectedItems().empty() ? tree->getSelectedItems()[0] : nullptr;
  check(panel && playback && view && item, "the raw clip is loaded with the ME panel and the view");
  if (!panel || !playback || !view || !item)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  constexpr int frame = 3;
  const QPoint  pixel(100, 100);
  playback->setCurrentFrameAndUpdate(frame);
  settle(300);

  // ---- no estimate yet: an explanation, no window ---------------------------------------------
  {
    QString shown;
    QTimer::singleShot(500, [&shown] {
      if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      {
        shown = box->text();
        box->close();
      }
    });
    emit view->blockCompareRequested(item, pixel, frame);
    settle(200);
    check(shown.contains("Ctrl+M"), "without an estimate, Shift+click says what to turn on");
    check(w->findChild<MeCompareWindow *>() == nullptr, "and opens no window");
  }

  // ---- block ME on 16x16 and 64x64 ------------------------------------------------------------
  checkBoxWithText(panel, "16")->setChecked(true); // 64 is on by default
  checkBoxWithText(panel, "Compute in the background")->setChecked(true);
  settle(25000);

  emit view->blockCompareRequested(item, pixel, frame);
  settle(300);
  auto *popup = w->findChild<MeCompareWindow *>();
  check(popup != nullptr && popup->isVisible(), "Shift+click opens the compare popup");
  if (!popup)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }
  check(popup->tabCount() == 2, "one tab per ticked size (" + std::to_string(popup->tabCount()) + ")");
  check(popup->tabTitle(0) == "ME 16x16" && popup->tabTitle(1) == "ME 64x64",
        "smallest first: " + popup->tabTitle(0).toStdString() + ", " +
            popup->tabTitle(1).toStdString());
  for (int i = 0; i < popup->tabCount() && i < 2; ++i)
  {
    const auto diff     = popup->diffOf(i);
    const auto reported = reportedSad(popup->tabDetail(i));
    check(diff.image.width > 0 && diff.sad == reported,
          popup->tabTitle(i).toStdString() + ": the popup's SAD is the estimator's common SAD (" +
              std::to_string(diff.sad) + " vs " + std::to_string(reported) + ")");
  }
  {
    const auto diff = popup->diffOf(0);
    check(diff.image.width == 16 && diff.image.height == 16, "the 16x16 tab is cut at 16x16");
    check(diff.sad == 0, "on the clean pan the 16x16 reference matches exactly (SAD " +
                             std::to_string(diff.sad) + ")");
  }
  popup->setDiffMode(true);
  settle(100);
  check(popup->isVisible(), "switching to the difference view keeps the popup up");

  // ---- Global motion adds its window ----------------------------------------------------------
  QAction *open = nullptr;
  for (auto *action : w->findChildren<QAction *>())
    if (action->text().startsWith("&Global motion"))
      open = action;
  check(open != nullptr, "the View menu has Global motion");
  if (open)
    open->trigger();
  auto *gm = w->findChild<GlobalMotionWindow *>();
  check(gm != nullptr && gm->run(), "a Global motion run starts");
  for (int t = 0; gm && t < 60000 && gm->busy(); t += 50)
    settle(50);
  check(gm && !gm->busy(), "and finishes");

  emit view->blockCompareRequested(item, pixel, frame);
  settle(300);
  check(popup->tabCount() == 3, "the GM window is added as a third tab");
  if (popup->tabCount() == 3)
  {
    check(popup->tabTitle(2) == "GME window (0, 0)",
          "the window covering the pixel, as (row, col): " + popup->tabTitle(2).toStdString());
    const auto diff = popup->diffOf(2);
    check(diff.image.width == 1280 / 4 && diff.image.height == 720 / 4,
          "cut at the GM window size (" + std::to_string(diff.image.width) + "x" +
              std::to_string(diff.image.height) + ")");
    check(diff.sad == 0, "and on the clean pan its reference matches exactly (SAD " +
                             std::to_string(diff.sad) + ")");
  }

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
