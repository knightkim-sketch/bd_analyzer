// Regression: odyssey closed-loop chosen in the ME panel, with and without start vectors.
//
// What this pins down:
//   * "Odyssey closed-loop ME" can be chosen and shows its own rows (Start MV from, Sub-pel);
//   * with "Start MV from: Odyssey open-loop ME" the Block Info row of a 64x64 block carries the same
//     sub-pel SAD as running the open-loop estimator and then the closed-loop one directly on the same
//     frames (src/me/OdysseyClosedLoopMe) - the panel and runner add no arithmetic of their own;
//   * "None (0, 0)" gives the direct no-seed result, and "1/4 pel only" the quarter-pel one;
//   * "Global model (SVT ME)" gives SVT ME -> RANSAC rotzoom -> synthesised start vectors.
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
#include "me/GlobalModel.h"
#include "me/OdysseyClosedLoopMe.h"
#include "me/SvtIntegerMe.h"
#include "me/OdysseyOpenLoopMe.h"
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

// The "sub-pel SAD N" of the closed-loop 64x64 row in the Block Info at a pixel, or -1.
long long closedLoopSad(playlistItem *item, QPoint pixel, int frame)
{
  const auto info = item->getBlockInfoAt(pixel, frame);
  for (const auto &e : info.entries)
    if (e.typeName.contains("closed-loop") && e.typeName.contains("64x64"))
    {
      const auto m = QRegularExpression("sub-pel SAD (\\d+)").match(e.valueText);
      if (m.hasMatch())
        return m.captured(1).toLongLong();
    }
  return -1;
}

// The direct estimate: optional open-loop seed, then closed-loop, 64x64 block containing `pixel`.
long long directSad(const bda::me::MePictureSet &pictures, bool seeded, bool hp, QPoint pixel)
{
  bda::me::MeParams params;
  params.frameInterval        = 1;
  params.allowHighPrecisionMv = hp;
  bda::me::MeFrameResult seed;
  if (seeded)
  {
    auto seedParams       = params;
    seedParams.algorithm  = bda::me::Algorithm::OdysseyOpenLoop;
    seed                  = bda::me::OdysseyOpenLoopMe().estimateFrame(pictures, seedParams);
    params.seed           = &seed;
  }
  params.blockSizes = bda::me::BlockSizeSet();
  params.blockSizes.add(bda::me::BlockSize::Blk64);
  const auto r = bda::me::OdysseyClosedLoopMe().estimateFrame(pictures, params);
  for (const auto &b : r.blocks)
    if (QRect(b.block.x, b.block.y, 64, 64).contains(pixel))
      return b.nativeCost;
  return -3;
}
// The direct global-model start: SVT ME, RANSAC rotzoom, its vector at every block centre.
long long directModelSad(const bda::me::MePictureSet &pictures, bool hp, QPoint pixel)
{
  bda::me::MeParams params;
  params.frameInterval        = 1;
  params.allowHighPrecisionMv = hp;
  const auto svt              = bda::me::SvtIntegerMe().estimateFrame(pictures, params);
  const auto model = bda::me::fitGlobalModel(svt, pictures.current.width(), pictures.current.height(), {});
  if (!model.ok())
    return -4;
  const auto seed = bda::me::synthesiseModelSeed(model, pictures.current.width(), pictures.current.height());
  params.seed     = &seed;
  params.blockSizes = bda::me::BlockSizeSet();
  params.blockSizes.add(bda::me::BlockSize::Blk64);
  const auto r = bda::me::OdysseyClosedLoopMe().estimateFrame(pictures, params);
  for (const auto &b : r.blocks)
    if (QRect(b.block.x, b.block.y, 64, 64).contains(pixel))
      return b.nativeCost;
  return -3;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerMeClosedLoopPanel");
  QCoreApplication::setApplicationName("bdAnalyzerMeClosedLoopPanel");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  constexpr int w = 320, h = 192, frames = 4;
  const auto    clip = writePanningClip("bd-me-closedloop_320x192_yuv420p.yuv", w, h, frames, 5, 2);
  check(!clip.isEmpty(), "the panning clip was written");

  bda::me::YuvLumaReader reader;
  std::string            err;
  reader.openRaw(clip.toStdString(), w, h, 8, err);
  bda::me::MePictureSet pictures;
  pictures.current     = reader.readLuma(2, bda::me::kOdysseyPadFull, err);
  pictures.reference   = reader.readLuma(1, bda::me::kOdysseyPadFull, err);
  pictures.frameIdx    = 2;
  pictures.refFrameIdx = 1;
  const QPoint pixel(100, 100);

  auto *win = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  win->resize(1400, 900);
  win->show();
  win->loadFiles({clip});
  settle(2000);

  auto *panel    = win->findChild<MotionEstimationWidget *>();
  auto *tree     = win->findChild<PlaylistTreeWidget *>();
  auto *playback = win->findChild<PlaybackController *>();
  auto *item     = tree && !tree->getSelectedItems().empty() ? tree->getSelectedItems()[0] : nullptr;
  auto *algo     = panel ? comboWithItem(panel, "Odyssey closed-loop ME") : nullptr;
  auto *seed     = panel ? comboWithItem(panel, "None (0, 0)") : nullptr;
  auto *subpel   = panel ? comboWithItem(panel, "1/4 pel only") : nullptr;
  check(panel && item && playback && algo && seed && subpel, "the clip, the panel and the closed-loop controls are there");
  if (!panel || !item || !playback || !algo || !seed || !subpel)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  playback->setCurrentFrameAndUpdate(2);
  settle(300);
  algo->setCurrentIndex(algo->findText("Odyssey closed-loop ME"));
  seed->setCurrentIndex(seed->findText("Odyssey open-loop ME"));
  settle(100);
  check(panel->params().algorithm == bda::me::Algorithm::OdysseyClosedLoop &&
            panel->params().seedAlgorithm == bda::me::Algorithm::OdysseyOpenLoop,
        "closed-loop with open-loop start vectors reaches the parameters");
  checkBoxWithText(panel, "Compute in the background")->setChecked(true);

  const auto waitSad = [&](long long notThis) {
    long long sad = -1;
    for (int t = 0; t < 30000; t += 100)
    {
      settle(100);
      sad = closedLoopSad(item, pixel, 2);
      if (sad >= 0 && sad != notThis)
        break;
    }
    return sad;
  };

  const auto seeded     = waitSad(-2);
  const auto wantSeeded = directSad(pictures, true, true, pixel);
  check(seeded >= 0 && seeded == wantSeeded, "open-loop start: the panel's sub-pel SAD is the direct estimate (" +
                                                 std::to_string(seeded) + " vs " + std::to_string(wantSeeded) + ")");

  seed->setCurrentIndex(seed->findText("None (0, 0)"));
  const auto wantNone = directSad(pictures, false, true, pixel);
  const auto none     = wantNone == seeded ? waitSad(-2) : waitSad(seeded);
  check(none == wantNone, "start (0, 0): the direct no-seed estimate (" + std::to_string(none) + " vs " +
                              std::to_string(wantNone) + ")");

  subpel->setCurrentIndex(subpel->findText("1/4 pel only"));
  const auto wantQuarter = directSad(pictures, false, false, pixel);
  const auto quarter     = wantQuarter == none ? waitSad(-2) : waitSad(none);
  check(quarter == wantQuarter, "1/4 pel only: the direct quarter-pel estimate (" + std::to_string(quarter) +
                                    " vs " + std::to_string(wantQuarter) + ")");

  seed->setCurrentIndex(seed->findText("Global model (SVT ME)"));
  settle(100);
  check(panel->params().seedModel == bda::me::GmModelType::RotZoom &&
            panel->params().seedAlgorithm == bda::me::Algorithm::SvtIntegerMe,
        "Global model (SVT ME) asks for a rotzoom fit of SVT's vectors");
  const auto wantModel = directModelSad(pictures, false, pixel);
  const auto model     = wantModel == quarter ? waitSad(-2) : waitSad(quarter);
  check(model >= 0 && model == wantModel, "global model start: the direct estimate (" + std::to_string(model) +
                                              " vs " + std::to_string(wantModel) + ")");

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
