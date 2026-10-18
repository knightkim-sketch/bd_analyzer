// Regression: the Global motion window's Stabilization (DIS) section and "Apply to view".
//
// A clip with a known camera shake comes from the sequence generator (as test 52). The window runs
// with its RANSAC model, and the section turns the per-frame models into a trajectory. Pinned down:
//   * a trajectory for every frame of the run (the reference of the first one included), drawn by the
//     plot, recomputed when the method changes;
//   * "Apply to view" makes the main window show the stabilised picture: the samples the view loaded
//     for a frame are byte-identical to that frame of "Export stabilized" with the same settings, and
//     to readFilteredRawFrame (the export of the filtered picture);
//   * motion estimation still reads the source (readMeFrame is untouched);
//   * another smoothing method re-renders the view (the buffer is dropped and reloaded with other
//     samples), and switching off brings the source back.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QSettings>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unistd.h>

#include "generator/Scene.h"
#include "integration/GlobalMotionWindow.h"
#include "integration/SequenceGeneratorWindow.h"
#include "integration/TrajectoryPlot.h"
#include "playlistitem/playlistItemWithVideo.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/PlaylistTreeWidget.h"
#include "video/yuv/videoHandlerYUV.h"

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

// Frame k of an 8-bit 4:2:0 y4m (no frame parameters), or empty.
QByteArray y4mFrame(const QString &path, int k, int w, int h)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  const auto all    = f.readAll();
  const int  header = all.indexOf('\n') + 1;
  const int  size   = w * h * 3 / 2;
  const int  at     = header + k * (6 + size) + 6;
  return header > 0 && at + size <= all.size() ? all.mid(at, size) : QByteArray();
}

// The samples the view holds for frame k, once it has loaded them.
QByteArray viewRaw(playlistItem *item, PlaybackController *playback, int k)
{
  playback->setCurrentFrameAndUpdate(k);
  auto *yuv = dynamic_cast<video::yuv::videoHandlerYUV *>(item->getFrameHandler());
  for (int t = 0; yuv && t < 5000; t += 50)
  {
    settle(50);
    int        idx = -1;
    const auto raw = yuv->getCurrentRawYUVData(idx);
    if (idx == k && !raw.isEmpty())
      return raw;
  }
  return {};
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerStabilizeView");
  QCoreApplication::setApplicationName("bdAnalyzerStabilizeView");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *win = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  win->resize(1400, 900);
  win->show();
  settle(500);

  // The shaken clip: a textured background larger than the picture, RandomWalk camera of 6 px.
  constexpr int w = 320, h = 192, frames = 24;
  generator::Scene s;
  s.width = w, s.height = h, s.frames = frames;
  {
    generator::Layer bg;
    bg.kind   = generator::ShapeKind::Image;
    bg.patchW = 352, bg.patchH = 224;
    bg.x = -16, bg.y = -16, bg.w = 352, bg.h = 224;
    bg.rgba.resize(std::size_t(bg.patchW) * bg.patchH * 4);
    std::uint32_t st  = 7;
    const auto    rnd = [&st] {
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
  }
  s.camera.mode = generator::CameraMode::RandomWalk;
  s.camera.ampX = 6, s.camera.ampY = 6, s.camera.seed = 5;
  const QString clip = QDir::tempPath() + "/bd-stabilize-view-53.y4m";
  check(generator::writeY4m(s, clip.toStdString()).empty(), "the shaken clip is written");
  win->loadFiles({clip});
  settle(2500);

  auto *tree     = win->findChild<PlaylistTreeWidget *>();
  auto *playback = win->findChild<PlaybackController *>();
  auto *item     = tree && !tree->getSelectedItems().empty() ? tree->getSelectedItems()[0] : nullptr;
  auto *video    = dynamic_cast<playlistItemWithVideo *>(item);
  check(item && playback && video, "the clip is loaded and selected");
  if (!item || !playback || !video)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  QAction *open = nullptr;
  for (auto *action : win->findChildren<QAction *>())
    if (action->text().startsWith("&Global motion"))
      open = action;
  if (open)
    open->trigger();
  auto *gm = win->findChild<integration::GlobalMotionWindow *>();
  check(gm != nullptr, "the Global motion window opens on the item");
  if (!gm)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  check(gm->run(), "a run starts (Model rotzoom by default)");
  for (int t = 0; t < 120000 && gm->busy(); t += 50)
    settle(50);
  settle(300);

  // ---- the trajectory --------------------------------------------------------------------------
  const auto &trajectory = gm->stabilizationFrames();
  check(int(trajectory.size()) == frames && gm->stabilizationFirstFrame() == 0,
        "a trajectory for frames 0.." + std::to_string(frames - 1) + " (" + std::to_string(trajectory.size()) + ")");
  check(gm->stabilizerParams().method == filter::SmoothingMethod::Gaussian, "smoothed with Gaussian by default");
  check(gm->findChild<integration::TrajectoryPlot *>() != nullptr, "and drawn by the trajectory plot");
  double largest = 0;
  for (const auto &f : trajectory)
    largest = std::max({largest, std::abs(f.correction.dx), std::abs(f.correction.dy)});
  check(largest > 0.5, "the shake needs correcting (largest " + std::to_string(largest) + " px)");

  // ---- Apply to view -----------------------------------------------------------------------------
  const int  k      = 10;
  const auto source = item->readRawFrame(k);
  QString    why;
  check(gm->setApplyToView(true, &why), "Apply to view is accepted " + why.toStdString());
  const auto shown = viewRaw(item, playback, k);
  check(!shown.isEmpty() && shown != source, "the view loads frame 10 stabilised, not the source");
  check(video->readFilteredRawFrame(k) == shown, "readFilteredRawFrame gives the same samples as the view");
  check(item->readMeFrame(k) == source, "motion estimation still reads the source");

  const QString out = QDir::tempPath() + "/bd-stabilize-view-53-out.y4m";
  QString       error;
  check(gm->exportStabilized(out, &error), "Export stabilized writes the clip " + error.toStdString());
  const auto exported = y4mFrame(out, k, w, h);
  check(!exported.isEmpty() && exported == shown.left(exported.size()),
        "and its frame 10 is byte-identical to what the view shows");

  // ---- another method re-renders; off restores the source ---------------------------------------
  auto *method = gm->findChild<QComboBox *>("stabMethod");
  method->setCurrentIndex(method->findData(int(filter::SmoothingMethod::KalmanCausal)));
  settle(200);
  check(gm->stabilizerParams().method == filter::SmoothingMethod::KalmanCausal, "the method is now Kalman causal");
  playback->setCurrentFrameAndUpdate(k + 1);
  settle(300);
  const auto shownKalman = viewRaw(item, playback, k);
  check(!shownKalman.isEmpty() && shownKalman != shown && shownKalman == video->readFilteredRawFrame(k),
        "the view is redrawn with the Kalman correction");

  check(gm->setApplyToView(false, &why), "Apply to view can be switched off");
  playback->setCurrentFrameAndUpdate(k + 1);
  settle(300);
  check(viewRaw(item, playback, k) == source, "and the view shows the source again");

  QFile::remove(out), QFile::remove(out + ".trajectory.csv"), QFile::remove(clip);
  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
