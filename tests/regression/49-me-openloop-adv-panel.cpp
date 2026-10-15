// Regression: odyssey open-loop adv chosen in the ME panel runs a whole GF group and draws its result.
//
// What this pins down:
//   * choosing "Odyssey open-loop ME adv" and ticking the box runs the group the shown frame is in,
//     and the Block Info row for the clicked 64x64 block carries the same common SAD as calling the
//     estimator directly on the same file (src/me/OdysseyOpenLoopAdvMe) - the panel adds no
//     arithmetic of its own;
//   * another frame of the same group is shown from the kept result, without a new run;
//   * the status names the frame's type and layer, and "Second reference" switches to the second
//     search of a B frame.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <cmath>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/MotionEstimationWidget.h"
#include "me/OdysseyOpenLoopAdvMe.h"
#include "me/YuvLumaReader.h"
#include "playlistitem/playlistItem.h"
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

QString writePanningClip(const QString &name, int w, int h, int frames, int dx, int dy)
{
  const QString path = QDir::tempPath() + "/" + name;
  QFile         f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return {};
  // Plus a little noise that changes every frame, so no block matches exactly and the compared
  // SADs are not trivially zero.
  const auto texel = [](int x, int y, int n) {
    const double v = 50.0 + 70.0 * noise(x, y, 16, 1) + 50.0 * noise(x, y, 5, 2) + 6.0 * lattice(x, y, 100 + n);
    return static_cast<char>(static_cast<unsigned char>(int(v)));
  };
  QByteArray       luma(w * h, '\0');
  const QByteArray chroma(2 * (w / 2) * (h / 2), '\x80');
  for (int n = 0; n < frames; ++n)
  {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        luma[y * w + x] = texel(x + dx * n, y + dy * n, n);
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

QComboBox *comboWithItem(QWidget *parent, const QString &text)
{
  for (auto *box : parent->findChildren<QComboBox *>())
    if (box->findText(text) >= 0)
      return box;
  return nullptr;
}

// The "common SAD N" of the adv row in the Block Info at a pixel, or -1.
long long advCommonSad(playlistItem *item, QPoint pixel, int frame)
{
  const auto info = item->getBlockInfoAt(pixel, frame);
  for (const auto &e : info.entries)
    if (e.typeName.contains("adv") && e.typeName.contains("64x64"))
    {
      const auto m = QRegularExpression("common SAD (\\d+)").match(e.valueText);
      if (m.hasMatch())
        return m.captured(1).toLongLong();
    }
  return -1;
}

// The direct estimator's common SAD of the 64x64 block containing `pixel`, search `s` of `frame`.
long long directCommonSad(const bda::me::OdyAdvResult &r, int frame, int s, QPoint pixel)
{
  const auto it = r.frames.find(frame);
  if (it == r.frames.end() || int(it->second.searches.size()) <= s)
    return -2;
  for (const auto &b : it->second.searches[std::size_t(s)].result.blocks)
    if (b.block.size == bda::me::BlockSize::Blk64 && QRect(b.block.x, b.block.y, 64, 64).contains(pixel))
      return b.commonSad;
  return -3;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerMeAdvPanel");
  QCoreApplication::setApplicationName("bdAnalyzerMeAdvPanel");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  constexpr int w = 320, h = 192, frames = 9;
  const auto    clip = writePanningClip("bd-me-adv_320x192_yuv420p.yuv", w, h, frames, 4, 2);
  check(!clip.isEmpty(), "the panning clip was written");

  // The direct run the panel has to agree with: GF 16 (one group 1-8), 16x16 TMV, 64x64 only.
  bda::me::YuvLumaReader reader;
  std::string            err;
  reader.openRaw(clip.toStdString(), w, h, 8, err);
  bda::me::OdyAdvParams p;
  p.gfInterval = 16;
  p.blockSizes = bda::me::BlockSizeSet();
  p.blockSizes.add(bda::me::BlockSize::Blk64);
  const auto direct = bda::me::estimateOdysseyOpenLoopAdv(
      [&reader](int poc) {
        std::string e;
        return reader.readLuma(poc, bda::me::kOdysseyPadFull, e);
      },
      frames, frames - 1, p);
  check(direct.error.empty() && direct.frames.size() == std::size_t(frames), "the direct run analysed all frames");

  auto *win = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  win->resize(1400, 900);
  win->show();
  win->loadFiles({clip});
  settle(2000);

  auto *panel    = win->findChild<MotionEstimationWidget *>();
  auto *tree     = win->findChild<PlaylistTreeWidget *>();
  auto *playback = win->findChild<PlaybackController *>();
  auto *item     = tree && !tree->getSelectedItems().empty() ? tree->getSelectedItems()[0] : nullptr;
  auto *algo     = panel ? comboWithItem(panel, "Odyssey open-loop ME adv (GF + TMV)") : nullptr;
  auto *show     = panel ? comboWithItem(panel, "Nearest reference") : nullptr;
  check(panel && item && playback && algo && show, "the clip, the panel and the adv controls are there");
  if (!panel || !item || !playback || !algo || !show)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  const QPoint pixel(100, 100);
  playback->setCurrentFrameAndUpdate(5);
  settle(300);
  algo->setCurrentIndex(algo->findText("Odyssey open-loop ME adv (GF + TMV)"));
  checkBoxWithText(panel, "Compute in the background")->setChecked(true);

  long long sad = -1;
  for (int t = 0; t < 30000 && sad < 0; t += 100)
  {
    settle(100);
    sad = advCommonSad(item, pixel, 5);
  }
  const auto want = directCommonSad(direct, 5, 0, pixel);
  check(sad > 0, "frame 5 gets an adv row in Block Info, with a non-zero SAD (noisy clip)");
  check(sad == want, "with the direct estimator's common SAD (" + std::to_string(sad) + " vs " +
                         std::to_string(want) + ")");

  // Same group: shown from the kept result, no new run needed.
  playback->setCurrentFrameAndUpdate(3);
  settle(300);
  const auto sad3 = advCommonSad(item, pixel, 3);
  check(sad3 >= 0 && sad3 == directCommonSad(direct, 3, 0, pixel),
        "frame 3 of the same group appears at once, matching the direct run");

  // The second search of a B frame.
  show->setCurrentIndex(1);
  settle(300);
  const auto sad3b = advCommonSad(item, pixel, 3);
  check(sad3b == directCommonSad(direct, 3, 1, pixel),
        "\"Second reference\" shows the second search (" + std::to_string(sad3b) + "; direct first " +
            std::to_string(directCommonSad(direct, 3, 0, pixel)) + ", second " +
            std::to_string(directCommonSad(direct, 3, 1, pixel)) + ")");

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
