// Regression: File > Export Stabilized Y4M - digital image stabilisation against a known camera.
//
// A clip with a known camera shake comes from the sequence generator (320x192, 40 frames, a
// textured background larger than the picture, a moving circle as an outlier, RandomWalk camera of
// 6 px per axis), so the ground truth is the camera CSV the generator writes next to it. The clip
// opens in MainWindow's playlist through the generator, and the export runs through its
// non-interactive entry point with fixed options (RotZoom, each smoothing method at its default) -
// the menu entry is checked for its place and its options dialog (inspected, then cancelled).
//
// What this pins down:
//   * the File menu has "Export &Stabilized Y4M..." right after "Export &Filtered Y4M...";
//   * the export writes every frame at the input size, and <output>.trajectory.csv with a header
//     and one row per frame; no frame pair fails its model on this clip;
//   * for each smoothing method (Moving average R 15, Gaussian sigma 6, Kalman causal / RTS with
//     Q 0.002, R 4) the next two checks hold (Kalman causal: 40% instead of 80%, and less than
//     RTS - see the loop over methods for why):
//   * stability: the inter-frame global shift (SVT integer ME 16x16 + RANSAC translation fit,
//     measured on the files) has its standard deviation reduced by at least 80% against the input,
//     and so does the ground-truth one (rendered camera_k + correction_k), which integer-pel ME
//     cannot resolve;
//   * accuracy: the correction tracks the camera: |correction_k - (smooth(gt)_k - gt_k)| <= 1 px
//     on both axes for every frame, smooth() being the same method and parameters, gt the
//     generator's trajectory (the smoothed component removed, as the design asks);
//   * with the same options a second run writes a byte-identical file (deterministic, Gaussian);
//   * the precomputed-models entry point, given the first run's models by absolute frame number,
//     writes a byte-identical Y4M and CSV, and its progress callback cancels;
//   * the options dialog defaults to Gaussian and shows only the chosen method's fields;
//   * the Translation model fits every pair as well and its corrections track the camera too.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QImage>
#include <QSettings>
#include <QMenu>
#include <QTimer>

#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "generator/Scene.h"
#include "integration/SequenceGeneratorWindow.h"
#include "filter/Stabilizer.h"
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
  params.staticBypass = false; // as the export: every point a searched vector
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

  const QString out = QDir::tempPath() + "/bd-stabilize-52-out.y4m";
  const int     n   = 40;
  std::vector<double> gx(n), gy(n);
  for (int k = 0; k < n; ++k)
  {
    const auto c = generator::cameraInOutput(s, k);
    gx[std::size_t(k)] = c.dx, gy[std::size_t(k)] = c.dy;
  }
  /* The reference correction for one smoothing setting: the generator's camera fed through the same
   * smoother as pure-translation models (raw = gt_k - gt_0; every method is shift-invariant, so
   * the correction is smooth(gt)_k - gt_k).
   */
  const auto gtCorrection = [&](const filter::StabilizerParams &sp) {
    std::vector<filter::AffineParams> m(std::size_t(n), filter::kAffineIdentity);
    for (int k = 1; k < n; ++k)
      m[std::size_t(k)][0] = -(gx[std::size_t(k)] - gx[std::size_t(k - 1)]),
      m[std::size_t(k)][1] = -(gy[std::size_t(k)] - gy[std::size_t(k - 1)]);
    return filter::stabilize(m, 320, 192, sp);
  };

  /* Stability and accuracy of one export: shift std-dev measured on the files (SVT + RANSAC), the
   * same from the rendered ground truth, and the correction against gtCorrection.
   */
  /* Returns the reduction measured from the rendered ground truth (1 - out / in). `keep` is the largest
   * output / input std-dev ratio accepted (0.2 = at least 80% less).
   */
  const auto evaluate = [&](const char *name, const integration::StabilizedExportResult &result, const QString &file,
                            const filter::StabilizerParams &sp, double keep) -> double {
    const std::string pct = std::to_string(int(std::lround(100 * (1 - keep))));
    const std::string tag = std::string(" [") + name + "]";
    const auto in = measureShifts(clip), st = measureShifts(file);
    bool       measured = in.size() == 40 && st.size() == 40;
    for (const auto &v : in)
      measured = measured && v.ok;
    for (const auto &v : st)
      measured = measured && v.ok;
    const double sIn = measured ? spread(in) : 0, sOut = measured ? spread(st) : 0;
    std::cout << "  info " << tag << " inter-frame shift std-dev: input " << sIn << " px, output " << sOut << " px ("
              << (sIn > 0 ? 100 * (1 - sOut / sIn) : 0) << "% less)" << std::endl;
    check(measured && sIn > 1 && sOut <= keep * sIn, "the inter-frame shift std-dev drops by at least " + pct + "%" + tag);

    // Accuracy against the generator's camera (output coordinates; no flips here).
    const auto ref   = gtCorrection(sp);
    double     worst = result.trajectory.size() == std::size_t(n) ? 0 : 1e9, rawWorst = 0;
    for (int k = 0; k < n && worst < 1e9; ++k)
    {
      const auto &c = result.trajectory[std::size_t(k)].correction, &g = ref[std::size_t(k)].correction;
      worst = std::max({worst, std::abs(c.dx - g.dx), std::abs(c.dy - g.dy)});
      const auto &r = result.trajectory[std::size_t(k)].raw;
      rawWorst = std::max({rawWorst, std::abs(r.dx - (gx[std::size_t(k)] - gx[0])), std::abs(r.dy - (gy[std::size_t(k)] - gy[0]))});
    }
    std::cout << "  info " << tag << " worst |correction - same-method smooth(gt) correction| = " << worst
              << " px, worst |raw - gt| = " << rawWorst << " px" << std::endl;
    check(worst <= 1.0, "the correction tracks the camera smoothed the same way within 1 px on every frame" + tag);

    /* Stability from the ground truth, free of the integer-pel ME resolution: the output content
     * sits at camera_k + correction_k, so its frame-to-frame steps are compared with the camera's.
     * The camera that shows is the rendered one: the background is a 1:1 image patch sampled at
     * pixel centres (nearest neighbour), so a camera offset d moves it by the whole pixels
     * ceil(d - 0.5). The CSV value d itself is reported alongside.
     */
    if (worst >= 1e9)
      return 0;
    const auto spreadFor = [&](bool rendered, double &camera, double &stabilized) {
      std::vector<Shift> camIn(1), camOut(1);
      const auto shown = [rendered](double d) { return rendered ? std::ceil(d - 0.5) : d; };
      for (int k = 1; k < n; ++k)
      {
        const auto &c1 = result.trajectory[std::size_t(k)].correction, &c0 = result.trajectory[std::size_t(k - 1)].correction;
        camIn.push_back({shown(gx[std::size_t(k)]) - shown(gx[std::size_t(k - 1)]),
                       shown(gy[std::size_t(k)]) - shown(gy[std::size_t(k - 1)]), true});
        camOut.push_back({camIn.back().x + c1.dx - c0.dx, camIn.back().y + c1.dy - c0.dy, true});
      }
      camera = spread(camIn), stabilized = spread(camOut);
    };
    double gIn, gOut, cIn, cOut;
    spreadFor(true, gIn, gOut);
    spreadFor(false, cIn, cOut);
    std::cout << "  info " << tag << " rendered-camera step std-dev: " << gIn << " px -> stabilized " << gOut << " px ("
              << 100 * (1 - gOut / gIn) << "% less); with the CSV's fractional camera " << cIn << " -> " << cOut
              << " px (" << 100 * (1 - cOut / cIn) << "% less)" << std::endl;
    check(gOut <= keep * gIn, "by the ground truth too, the frame-to-frame motion drops by at least " + pct + "%" + tag);
    return 1 - gOut / gIn;
  };

  // Default options: RotZoom, Gaussian sigma 6.
  integration::StabilizedExportOptions options;
  check(options.model == me::GmModelType::RotZoom && options.smoothing.method == filter::SmoothingMethod::Gaussian,
        "default options: RotZoom, Gaussian");
  int        calls  = 0;
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
  evaluate("Gaussian", result, out, options.smoothing, 0.2);

  /* The other three methods through the full two-pass export. Kalman causal is held to 40%, not
   * 80%: frame k may use frames <= k only, so on a bounded random walk a constant-velocity filter
   * must either follow the shake (lag-free) or lag behind the camera (smooth) - it cannot do both
   * without look-ahead. Measured on this clip: 47-66% for any q (q 0.002: 62%), against 91% for
   * RTS with the same q / r. It must still lose to RTS on the same clip.
   */
  double reduction[2]{}; // causal, RTS
  for (const auto m : {filter::SmoothingMethod::MovingAverage, filter::SmoothingMethod::KalmanRts,
                       filter::SmoothingMethod::KalmanCausal})
  {
    const QString mpath = QDir::tempPath() + "/bd-stabilize-52-method.y4m";
    auto          mopt  = options;
    mopt.smoothing.method = m;
    const auto r          = integration::exportStabilizedY4m(item, mpath, mopt);
    check(r.ok() && r.framesWritten == 40 && r.failedFits == 0,
          std::string("the export writes 40 frames, every pair fitted [") + filter::smoothingMethodName(m) + "]");
    const bool causal = m == filter::SmoothingMethod::KalmanCausal;
    const auto red    = evaluate(filter::smoothingMethodName(m), r, mpath, mopt.smoothing, causal ? 0.6 : 0.2);
    if (m != filter::SmoothingMethod::MovingAverage)
      reduction[causal ? 0 : 1] = red;
    QFile::remove(mpath), QFile::remove(mpath + ".trajectory.csv");
  }
  std::cout << "  info  ground-truth shake reduction: Kalman causal " << 100 * reduction[0] << "%, Kalman RTS "
            << 100 * reduction[1] << "%" << std::endl;
  check(reduction[0] > 0 && reduction[0] < reduction[1], "Kalman causal removes less shake than Kalman RTS (no look-ahead)");

  // Deterministic.
  {
    const QString again = QDir::tempPath() + "/bd-stabilize-52-again.y4m";
    const auto    r2    = integration::exportStabilizedY4m(item, again, options);
    const auto    a = fileBytes(out), b = fileBytes(again);
    check(r2.ok() && !a.isEmpty() && a == b, "a second run writes a byte-identical file");
    QFile::remove(again), QFile::remove(again + ".trajectory.csv");
  }

  /* Precomputed models (the GM window's entry): the models of the run above keyed by absolute frame
   * number give the same Y4M and CSV byte for byte; progress counts pass 2 only and cancels.
   */
  {
    const auto range = item->properties().startEndRange;
    std::map<int, filter::AffineParams> byFrame;
    for (std::size_t k = 1; k < result.models.size(); ++k)
      if (result.models[k].ok())
        byFrame[range.first + int(k)] = result.models[k].p;
    const QString pre   = QDir::tempPath() + "/bd-stabilize-52-pre.y4m";
    int           pcalls = 0;
    const auto    r = integration::exportStabilizedY4m(item, pre, byFrame, options.smoothing, [&pcalls](int, int total) {
      ++pcalls;
      return total == 40;
    });
    check(byFrame.size() == 39 && r.ok() && r.framesWritten == 40 && pcalls == 40 && !fileBytes(pre).isEmpty() &&
              fileBytes(pre) == fileBytes(out) && fileBytes(pre + ".trajectory.csv") == fileBytes(out + ".trajectory.csv"),
          "precomputed models: byte-identical Y4M and trajectory CSV, progress once per frame (" +
              std::to_string(pcalls) + " calls)");
    const auto c = integration::exportStabilizedY4m(item, pre, byFrame, options.smoothing,
                                                    [](int done, int) { return done < 5; });
    check(c.cancelled && !c.ok() && c.framesWritten == 5, "precomputed models: progress returning false cancels after frame 5");
    QFile::remove(pre), QFile::remove(pre + ".trajectory.csv");
  }

  // The Translation model on the same clip: every pair fitted, corrections within 1 px of the camera.
  {
    const QString tpath = QDir::tempPath() + "/bd-stabilize-52-translation.y4m";
    auto          topt  = options;
    topt.model          = me::GmModelType::Translation;
    const auto r        = integration::exportStabilizedY4m(item, tpath, topt);
    const auto ref      = gtCorrection(topt.smoothing);
    double     worst    = r.ok() && r.trajectory.size() == 40 ? 0 : 1e9;
    for (int k = 0; k < 40 && worst < 1e9; ++k)
    {
      const auto &t = r.trajectory[std::size_t(k)].correction, &g = ref[std::size_t(k)].correction;
      worst = std::max({worst, std::abs(t.dx - g.dx), std::abs(t.dy - g.dy), std::abs(t.angle) * 1e3});
    }
    std::cout << "  info  Translation model: worst correction error " << worst << " px" << std::endl;
    check(r.ok() && r.failedFits == 0 && worst <= 1.0,
          "the Translation model also fits every pair and tracks the camera within 1 px, angle 0");
    QFile::remove(tpath), QFile::remove(tpath + ".trajectory.csv");
  }

  /* The menu entry's options dialog: Method defaults to Gaussian and only the fields of the chosen
   * method are shown. Inspected from a timer while it is modal, then cancelled.
   */
  {
    bool seen = false, ok = false;
    QTimer::singleShot(300, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      auto *method = dialog ? dialog->findChild<QComboBox *>("stabilizeMethod") : nullptr;
      auto *form   = dialog ? dialog->findChild<QFormLayout *>() : nullptr;
      seen         = method && form;
      if (seen)
      {
        const auto visible = [&](const char *name) { return form->isRowVisible(dialog->findChild<QWidget *>(name)); };
        ok = method->count() == 4 && method->currentText() == "Gaussian" && visible("stabilizeSigma") &&
             !visible("stabilizeRadius") && !visible("stabilizeKalmanQ");
        method->setCurrentIndex(method->findText("Kalman RTS"));
        ok = ok && visible("stabilizeKalmanQ") && visible("stabilizeKalmanR") && !visible("stabilizeSigma");
        method->setCurrentIndex(method->findText("Moving average"));
        ok = ok && visible("stabilizeRadius") && !visible("stabilizeSigma") && !visible("stabilizeKalmanR");
      }
      if (dialog)
        dialog->reject();
    });
    integration::exportStabilizedY4m(item, win);
    check(seen && ok, "the options dialog: Method (4 entries, Gaussian default) shows only that method's fields");
  }

  QFile::remove(out), QFile::remove(out + ".trajectory.csv");
  QFile::remove(clip), QFile::remove(clip + ".camera.csv");
  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
