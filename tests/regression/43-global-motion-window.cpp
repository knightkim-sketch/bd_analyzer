// Regression: the Global motion window runs the estimator over a clip and shows a frame's windows.
//
// What the window adds to src/me/GlobalMotion, and what this pins down:
//   * it takes one item the estimator can read, and refuses anything else with a sentence - a
//     compressed stream has no source pictures unless an original is attached;
//   * a run fills one table row per frame, with the vector the clip was built with (+8, +4) - the
//     sign included: the content moves left and up, so the reference lies to the right and below;
//   * selecting a frame moves the main window there and draws the 16 windows over the picture, on
//     the item's motion estimation container, with type ids of their own;
//   * the Block Info pane lists the window under a pixel, since it walks every type with data;
//   * the CSV export is odyssey's format, 16 rows a frame.
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTableWidget>

#include <cmath>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/GlobalMotionWindow.h"
#include "integration/GmStatisticsAdapter.h"
#include "playlistitem/playlistItemCompressedVideo.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/PlaylistTreeWidget.h"

using bda::integration::GlobalMotionWindow;

namespace
{
int  failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++failures;
}
void settle(int ms)
{
  for (int i = 0; i < ms / 10; ++i)
  {
    QCoreApplication::processEvents();
    usleep(10000);
  }
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

/* Frame n shows texel(x + dx n, y + dy n): the content moves left and up by (dx, dy) a frame, so
 * the current picture matches the reference at (x + dx, y + dy).
 *
 * Value noise, the unit test's texture, not the sinusoids of tests 26 and 27: one of those has a
 * 9 px period, which aliases at the 1/4 downsampling this resolution uses, and the gates then
 * (rightly) reject most windows - a test of the texture, not of the window.
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

bool waitIdle(GlobalMotionWindow &window, int timeoutMs)
{
  for (int t = 0; t < timeoutMs && window.busy(); t += 50)
    settle(50);
  return !window.busy();
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 43-global-motion-window <compressed stream without an original>\n";
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerGmWindow");
  QCoreApplication::setApplicationName("bdAnalyzerGmWindow");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  // 1280 wide: downsampled by 4 (n = 2), so the source-resolution refinement runs too.
  constexpr int frames = 6;
  const auto    clip   = writePanningClip("bd-gm-pan_1280x720_yuv420p.yuv", 1280, 720, frames, 8, 4);
  check(!clip.isEmpty(), "the panning clip was written");

  // ---- refusals, without a main window -------------------------------------------------------
  {
    GlobalMotionWindow          window;
    playlistItemCompressedVideo stream(QString::fromUtf8(argv[1]), 0, InputFormat::Libav,
                                       decoder::DecoderEngine::Invalid);
    const auto r = window.setTarget({&stream});
    check(!r.ok() && r.message.contains("original"),
          "a compressed stream without its original is refused, saying why");
    check(!window.setTarget({}).ok(), "an empty selection is refused");
  }

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1400, 900);
  w->show();
  w->loadFiles({clip});
  settle(2000);

  auto *tree     = w->findChild<PlaylistTreeWidget *>();
  auto *playback = w->findChild<PlaybackController *>();
  auto *item     = tree && !tree->getSelectedItems().empty() ? tree->getSelectedItems()[0] : nullptr;
  check(item != nullptr && playback != nullptr, "the raw clip is loaded and selected");
  if (!item || !playback)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  QAction *open = nullptr;
  for (auto *action : w->findChildren<QAction *>())
    if (action->text().startsWith("&Global motion"))
      open = action;
  check(open != nullptr, "the View menu has Global motion");
  if (open)
    open->trigger();
  auto *window = w->findChild<GlobalMotionWindow *>();
  check(window != nullptr, "the action opened the window on the selected item");
  if (!window)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  check(window->run(), "a run starts");
  check(waitIdle(*window, 60000), "and finishes");
  const auto results = window->results();
  check(int(results.size()) == frames - 1, "one result per frame after the first (" +
                                               std::to_string(results.size()) + ")");

  /* The clip's own motion, in source pixels, on the windows the gates accepted. Exact everywhere
   * except where content enters the picture - the right column and bottom row here - which the
   * reference never had; the source-resolution refinement is known to land a pixel off there
   * (design doc section 11).
   */
  int matched = 0, accepted = 0, edgeNear = 0, other = 0;
  for (const auto &[poc, r] : results)
    for (const auto &win : r.windows)
      if (win.acceptX && win.acceptY)
      {
        ++accepted;
        const int ex = std::abs(win.x.dFull - 8), ey = std::abs(win.y.dFull - 4);
        if (ex == 0 && ey == 0)
          ++matched;
        else if ((win.winI == 3 || win.winJ == 3) && ex <= 1 && ey <= 1)
          ++edgeNear;
        else
          ++other;
      }
  std::cout << "        accepted windows " << accepted << ": exact (+8, +4) " << matched
            << ", entering edge within 1 px " << edgeNear << ", other " << other << std::endl;
  check(accepted >= 12 * (frames - 1), "nearly every window accepts a vector on a clean pan");
  check(other == 0 && matched >= accepted - (frames - 1) * 7,
        "accepted windows report (+8, +4) - sign and scale included - bar the entering edge");

  auto *table = window->findChildren<QTableWidget *>().value(0);
  check(table != nullptr && table->rowCount() == frames - 1, "the frame table has a row per frame");

  // ---- select a frame: the main window follows and the windows are drawn --------------------
  window->selectFrame(3);
  settle(800);
  check(playback->getCurrentFrame() == 3, "selecting frame 3 moves the main window there");

  auto *data = item->getMotionEstimationData();
  check(data != nullptr && data->getFrameIndex() == 3, "the overlay holds frame 3");
  if (data)
  {
    check(data->at(bda::integration::kGmWindowTypeId).valueData.size() == 16,
          "16 windows are drawn");
    const auto vectors = data->at(bda::integration::kGmAcceptedTypeId).vectorData.size() +
                         data->at(bda::integration::kGmRawTypeId).vectorData.size();
    check(vectors >= 16, "every window carries a vector, accepted or raw");

    const auto info = item->getBlockInfoAt(QPoint(640, 360), 3);
    bool       listed = false;
    for (const auto &e : info.entries)
      if (e.typeName.startsWith("GM "))
        listed = true;
    check(listed, "the Block Info query lists the window under the pixel");
  }

  // ---- export --------------------------------------------------------------------------------
  const auto csvPath = QDir::tempPath() + "/bd-gm-window-export.csv";
  QString    error;
  check(window->exportCsv(csvPath, &error), "the CSV is written " + error.toStdString());
  QFile csv(csvPath);
  int   lines = 0;
  if (csv.open(QIODevice::ReadOnly))
    while (!csv.readLine().isEmpty())
      ++lines;
  check(lines == 1 + 16 * (frames - 1), "header plus 16 rows a frame (" + std::to_string(lines) + ")");
  QFile::remove(csvPath);
  QFile::remove(clip);

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  // MainWindow is deliberately leaked (see test 21); skip a teardown it was never built for.
  _exit(failures == 0 ? 0 : 1);
}
