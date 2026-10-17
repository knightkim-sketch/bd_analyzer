// Regression: File > Export Stabilized Y4M - digital image stabilisation against a known camera.
//
// A clip with a known camera shake comes from the sequence generator (320x192, 40 frames, a
// textured background larger than the picture, a moving circle as an outlier, RandomWalk camera of
// 6 px per axis), so the ground truth is the camera CSV the generator writes next to it. The clip
// opens in MainWindow's playlist through the generator, and the export runs through its
// non-interactive entry point with fixed options (RotZoom, radius 15) - the menu entry would open
// modal dialogs, so only its presence is checked.
//
// What this pins down:
//   * the File menu has "Export &Stabilized Y4M..." right after "Export &Filtered Y4M...";
//   * the export writes every frame at the input size, and <output>.trajectory.csv with a header
//     and one row per frame; no frame pair fails its model on this clip;
//   * stability: the inter-frame global shift (SVT integer ME 16x16 + RANSAC translation fit,
//     measured on the files) has its standard deviation reduced by at least 80% against the input,
//     and so does the ground-truth one (rendered camera_k + correction_k), which integer-pel ME
//     cannot resolve;
//   * accuracy: the correction tracks the camera: |correction_k - (smooth(gt)_k - gt_k)| <= 1 px
//     on both axes for every frame, smooth() being the same clamped moving average, gt the
//     generator's trajectory (the smoothed component removed, as the design asks);
//   * with the same options a second run writes a byte-identical file (deterministic);
//   * the Translation model fits every pair as well and its corrections track the camera too.
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSettings>
#include <QMenu>

#include <cmath>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "generator/Scene.h"
#include "integration/SequenceGeneratorWindow.h"
#include "integration/StabilizedY4mExport.h"
#include "me/GlobalModel.h"
#include "me/IMotionEstimator.h"
#include "me/YuvLumaReader.h"
#include "playlistitem/playlistItem.h"
#include "ui/Mainwindow.h"
#include "ui/widgets/PlaylistTreeWidget.h"

using namespace bda;

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

QByteArray fileBytes(const QString &path)
{
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Inter-frame content motion k-1 -> k of a y4m, measured with SVT ME 16x16 + a RANSAC translation.
struct Shift
{
  double x{}, y{};
  bool   ok{};
};
std::vector<Shift> measureShifts(const QString &path)
{
  me::YuvLumaReader reader;
  std::string       err;
  std::vector<Shift> shifts;
  if (!reader.openY4m(path.toStdString(), err))
    return shifts;
  auto         estimator = me::makeEstimator(me::Algorithm::SvtIntegerMe);
  me::MeParams params;
  params.blockSizes = me::BlockSizeSet();
  params.blockSizes.add(me::BlockSize::Blk16);
  params.staticBypass = false; // as the export: a still pair is a measurement, not a missing one
  me::GmModelParams fit;
  fit.type = me::GmModelType::Translation;
  me::MePlane prev = reader.readLuma(0, me::kOdysseyPadFull, err);
  shifts.push_back({0, 0, true});
  for (int k = 1; k < reader.frameCount(); ++k)
  {
    me::MePictureSet pics;
    pics.current   = reader.readLuma(k, me::kOdysseyPadFull, err);
    pics.reference = prev;
    pics.frameIdx = k, pics.refFrameIdx = k - 1, pics.refDistance = 1;
    const auto r = estimator->estimateFrame(pics, params);
    const auto m = me::fitGlobalModel(r, reader.width(), reader.height(), fit);
    // ref = cur + p: the content moved by -p from k-1 to k.
    shifts.push_back({-m.p[0], -m.p[1], r.ok() && m.ok()});
    if (!m.ok())
      std::cout << "  info  " << path.toStdString() << " frame " << k << ": " << m.error << std::endl;
    prev = std::move(pics.current);
  }
  return shifts;
}

double spread(const std::vector<Shift> &s) // std-dev of the 2-D shift, frames 1..n-1
{
  double mx = 0, my = 0;
  const int n = int(s.size()) - 1;
  for (int k = 1; k <= n; ++k)
    mx += s[std::size_t(k)].x, my += s[std::size_t(k)].y;
  mx /= n, my /= n;
  double v = 0;
  for (int k = 1; k <= n; ++k)
    v += std::pow(s[std::size_t(k)].x - mx, 2) + std::pow(s[std::size_t(k)].y - my, 2);
  return std::sqrt(v / n);
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerStabilize");
  QCoreApplication::setApplicationName("bdAnalyzerStabilize");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *win = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  win->resize(1400, 900);
  win->show();
  settle(500);

  // The menu entry, right after the filtered export.
  {
    QAction *filtered = nullptr, *stabilized = nullptr, *after = nullptr;
    for (auto *menu : win->findChildren<QMenu *>())
    {
      const auto actions = menu->actions();
      for (int i = 0; i < actions.size(); ++i)
        if (actions[i]->text() == "Export &Filtered Y4M...")
        {
          filtered = actions[i];
          after    = i + 1 < actions.size() ? actions[i + 1] : nullptr;
        }
        else if (actions[i]->text() == "Export &Stabilized Y4M...")
          stabilized = actions[i];
    }
    check(filtered && stabilized && after == stabilized,
          "the File menu has Export Stabilized Y4M right after Export Filtered Y4M");
  }

  QAction *open = nullptr;
  for (auto *action : win->findChildren<QAction *>())
    if (action->text().startsWith("&Generate Y4M Sequence"))
      open = action;
  if (open)
    open->trigger();
  settle(200);
  auto *gen = win->findChild<integration::SequenceGeneratorWindow *>();
  check(gen != nullptr, "the generator window opens");
  if (!gen)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  // Textured background bigger than the picture (so the camera never shows its edge), and a moving
  // circle that the global model has to leave out.
  auto &s  = gen->scene();
  s.width  = 320, s.height = 192, s.frames = 40;
  s.layers.clear();
  {
    generator::Layer bg;
    bg.kind   = generator::ShapeKind::Image;
    bg.patchW = 352, bg.patchH = 224;
    bg.x = -16, bg.y = -16, bg.w = 352, bg.h = 224;
    bg.rgba.resize(std::size_t(bg.patchW) * bg.patchH * 4);
    std::uint32_t st   = 99;
    const auto    rnd  = [&st] {
      st = st * 1103515245u + 12345u;
      return int((st >> 16) & 0xff);
    };
    std::vector<int> cells(std::size_t(bg.patchW / 4 + 1) * (bg.patchH / 4 + 1));
    for (auto &c : cells)
      c = 30 + rnd() * 190 / 255;
    for (int y = 0; y < bg.patchH; ++y)
      for (int x = 0; x < bg.patchW; ++x)
      {
        const int v  = std::clamp(cells[std::size_t(y / 4) * (bg.patchW / 4 + 1) + x / 4] + (rnd() % 21) - 10, 0, 255);
        auto     *px = &bg.rgba[(std::size_t(y) * bg.patchW + x) * 4];
        px[0] = std::uint8_t(v), px[1] = std::uint8_t(255 - v / 2), px[2] = std::uint8_t(v / 3 + 60), px[3] = 255;
      }
    s.layers.push_back(bg);
    generator::Layer ball;
    ball.kind   = generator::ShapeKind::Circle;
    ball.x = 40, ball.y = 60, ball.w = 40, ball.h = 40;
    ball.colour = {200, 90, 160};
    ball.motion.vx = 5, ball.motion.vy = 1;
    s.layers.push_back(ball);
  }
  s.camera.mode = generator::CameraMode::RandomWalk;
  s.camera.ampX = 6, s.camera.ampY = 6, s.camera.ampDeg = 0, s.camera.seed = 3;
  gen->sceneChanged();

  const QString clip = QDir::tempPath() + "/bd-stabilize-52.y4m";
  QString       error;
  check(gen->generate(clip, &error), "the shaken clip is generated (" + error.toStdString() + ")");
  settle(2000);
  auto *tree = win->findChild<PlaylistTreeWidget *>();
  auto *item = tree && !tree->getSelectedItems().empty() ? tree->getSelectedItems()[0] : nullptr;
  check(item != nullptr && item->properties().name.contains("bd-stabilize-52"), "and opens in the playlist");
  if (!item)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  const QString                           out = QDir::tempPath() + "/bd-stabilize-52-out.y4m";
  integration::StabilizedExportOptions    options; // RotZoom, radius 15
  int                                     calls = 0;
  const auto result = integration::exportStabilizedY4m(item, out, options, [&calls](int, int) {
    ++calls;
    return true;
  });
  check(result.ok() && result.framesWritten == 40 && result.framesTotal == 40,
        "the export writes 40 frames (" + result.error.toStdString() + ")");
  check(calls == 80, "progress is reported for both passes (" + std::to_string(calls) + " calls)");
  check(result.failedFits == 0, "every frame pair gets a model (" + std::to_string(result.failedFits) + " failed)");
  for (std::size_t k = 1; k < result.models.size(); ++k)
    if (!result.models[k].ok())
      std::cout << "  info  frame " << k << ": no model - " << result.models[k].error << std::endl;

  {
    QFile csv(result.csvPath);
    const auto lines = csv.open(QIODevice::ReadOnly) ? QString::fromUtf8(csv.readAll()).split('\n', Qt::SkipEmptyParts)
                                                     : QStringList();
    check(result.csvPath == out + ".trajectory.csv" && lines.size() == 41 &&
              lines.value(0).startsWith("frame,raw_dx,raw_dy,raw_angle,raw_scale,smooth_dx"),
          "<output>.trajectory.csv: header and 40 rows");
  }
  me::YuvLumaReader rd;
  std::string       err;
  check(rd.openY4m(out.toStdString(), err) && rd.frameCount() == 40 && rd.width() == 320 && rd.height() == 192,
        "the output reads back as 320x192, 40 frames");

  // Stability, measured on the files.
  const auto in = measureShifts(clip), st = measureShifts(out);
  bool       measured = in.size() == 40 && st.size() == 40;
  for (const auto &v : in)
    measured = measured && v.ok;
  for (const auto &v : st)
    measured = measured && v.ok;
  const double sIn = measured ? spread(in) : 0, sOut = measured ? spread(st) : 0;
  std::cout << "  info  inter-frame shift std-dev: input " << sIn << " px, output " << sOut << " px ("
            << (sIn > 0 ? 100 * (1 - sOut / sIn) : 0) << "% less)" << std::endl;
  check(measured && sIn > 1 && sOut <= 0.2 * sIn, "the inter-frame shift std-dev drops by at least 80%");

  // Accuracy against the generator's camera (output coordinates; no flips here).
  {
    const int           n = 40, R = options.radius;
    std::vector<double> gx(n), gy(n);
    for (int k = 0; k < n; ++k)
    {
      const auto c = generator::cameraInOutput(s, k);
      gx[std::size_t(k)] = c.dx, gy[std::size_t(k)] = c.dy;
    }
    double worst = 0, rawWorst = 0;
    for (int k = 0; k < n; ++k)
    {
      const int lo = std::max(0, k - R), hi = std::min(n - 1, k + R);
      double    sx = 0, sy = 0;
      for (int i = lo; i <= hi; ++i)
        sx += gx[std::size_t(i)], sy += gy[std::size_t(i)];
      sx /= hi - lo + 1, sy /= hi - lo + 1;
      const auto &c = result.trajectory[std::size_t(k)].correction;
      worst = std::max({worst, std::abs(c.dx - (sx - gx[std::size_t(k)])), std::abs(c.dy - (sy - gy[std::size_t(k)]))});
      const auto &r = result.trajectory[std::size_t(k)].raw;
      rawWorst = std::max({rawWorst, std::abs(r.dx - (gx[std::size_t(k)] - gx[0])), std::abs(r.dy - (gy[std::size_t(k)] - gy[0]))});
    }
    std::cout << "  info  worst |correction - (smooth(gt) - gt)| = " << worst << " px, worst |raw - gt| = " << rawWorst
              << " px" << std::endl;
    check(worst <= 1.0, "the correction tracks the camera within 1 px on every frame");

    /* Stability from the ground truth, free of the integer-pel ME resolution: the output content
     * sits at camera_k + correction_k, so its frame-to-frame steps are compared with the camera's.
     * The camera that shows is the rendered one: the background is a 1:1 image patch sampled at
     * pixel centres (nearest neighbour), so a camera offset d moves it by the whole pixels
     * ceil(d - 0.5). The CSV value d itself is reported alongside.
     */
    const auto spreadFor = [&](bool rendered, double &camera, double &stabilized) {
      std::vector<Shift> in(1), out(1);
      const auto shown = [rendered](double d) { return rendered ? std::ceil(d - 0.5) : d; };
      for (int k = 1; k < n; ++k)
      {
        const auto &c1 = result.trajectory[std::size_t(k)].correction, &c0 = result.trajectory[std::size_t(k - 1)].correction;
        in.push_back({shown(gx[std::size_t(k)]) - shown(gx[std::size_t(k - 1)]),
                      shown(gy[std::size_t(k)]) - shown(gy[std::size_t(k - 1)]), true});
        out.push_back({in.back().x + c1.dx - c0.dx, in.back().y + c1.dy - c0.dy, true});
      }
      camera = spread(in), stabilized = spread(out);
    };
    double gIn, gOut, cIn, cOut;
    spreadFor(true, gIn, gOut);
    spreadFor(false, cIn, cOut);
    std::cout << "  info  rendered-camera step std-dev: " << gIn << " px -> stabilized " << gOut << " px ("
              << 100 * (1 - gOut / gIn) << "% less); with the CSV's fractional camera " << cIn << " -> " << cOut
              << " px (" << 100 * (1 - cOut / cIn) << "% less)" << std::endl;
    check(gOut <= 0.2 * gIn, "by the ground truth too, the frame-to-frame motion drops by at least 80%");
  }

  // Deterministic.
  {
    const QString again = QDir::tempPath() + "/bd-stabilize-52-again.y4m";
    const auto    r2    = integration::exportStabilizedY4m(item, again, options);
    const auto    a = fileBytes(out), b = fileBytes(again);
    check(r2.ok() && !a.isEmpty() && a == b, "a second run writes a byte-identical file");
    QFile::remove(again), QFile::remove(again + ".trajectory.csv");
  }

  // The Translation model on the same clip: every pair fitted, corrections within 1 px of the camera.
  {
    const QString tpath = QDir::tempPath() + "/bd-stabilize-52-translation.y4m";
    auto          topt  = options;
    topt.model          = me::GmModelType::Translation;
    const auto r        = integration::exportStabilizedY4m(item, tpath, topt);
    double     worst    = r.ok() && r.trajectory.size() == 40 ? 0 : 1e9;
    for (int k = 0; k < 40 && worst < 1e9; ++k)
    {
      const int lo = std::max(0, k - topt.radius), hi = std::min(39, k + topt.radius);
      double    sx = 0, sy = 0;
      for (int i = lo; i <= hi; ++i)
        sx += generator::cameraInOutput(s, i).dx, sy += generator::cameraInOutput(s, i).dy;
      sx /= hi - lo + 1, sy /= hi - lo + 1;
      const auto c  = generator::cameraInOutput(s, k);
      const auto &t = r.trajectory[std::size_t(k)].correction;
      worst = std::max({worst, std::abs(t.dx - (sx - c.dx)), std::abs(t.dy - (sy - c.dy)), std::abs(t.angle) * 1e3});
    }
    std::cout << "  info  Translation model: worst correction error " << worst << " px" << std::endl;
    check(r.ok() && r.failedFits == 0 && worst <= 1.0,
          "the Translation model also fits every pair and tracks the camera within 1 px, angle 0");
    QFile::remove(tpath), QFile::remove(tpath + ".trajectory.csv");
  }

  QFile::remove(out), QFile::remove(out + ".trajectory.csv");
  QFile::remove(clip), QFile::remove(clip + ".camera.csv");
  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
