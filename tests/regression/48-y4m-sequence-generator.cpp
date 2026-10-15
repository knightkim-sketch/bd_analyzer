// Regression: File > Generate Y4M Sequence - draw objects on layers, give them motion, write the y4m
// and get it opened in the playlist.
//
// What this pins down:
//   * the menu opens the generator window;
//   * a scene of four shapes and an image patch, each with its own motion, is written and opens in
//     the playlist by itself, with the scene's size and frame count;
//   * the file holds exactly what the Qt-free renderer draws (src/generator/Scene) - the window adds
//     no pixels of its own;
//   * the MV fields take fractional pixels per frame;
//   * a scene saved to JSON and loaded back renders the same frames.
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QImage>
#include <QSettings>
#include <iostream>
#include <string>
#include <unistd.h>

#include "generator/Scene.h"
#include "integration/SequenceGeneratorWindow.h"
#include "me/YuvLumaReader.h"
#include "playlistitem/playlistItem.h"
#include "ui/Mainwindow.h"
#include "ui/widgets/PlaylistTreeWidget.h"

using namespace bda::generator;

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

Layer shape(ShapeKind kind, double x, double y, double w, double h, Yuv8 colour, int vx, int vy)
{
  Layer l;
  l.kind = kind;
  l.x = x, l.y = y, l.w = w, l.h = h;
  l.colour    = colour;
  l.motion.vx = vx, l.motion.vy = vy;
  return l;
}

bool sameFrames(const Scene &a, const Scene &b)
{
  for (int k = 0; k < a.frames; ++k)
  {
    const auto fa = renderFrame(a, k), fb = renderFrame(b, k);
    if (fa.y != fb.y || fa.u != fb.u || fa.v != fb.v)
      return false;
  }
  return true;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerGenerator");
  QCoreApplication::setApplicationName("bdAnalyzerGenerator");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *win = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  win->resize(1400, 900);
  win->show();
  settle(500);

  QAction *open = nullptr;
  for (auto *action : win->findChildren<QAction *>())
    if (action->text().startsWith("&Generate Y4M Sequence"))
      open = action;
  check(open != nullptr, "the File menu has Generate Y4M Sequence");
  if (open)
    open->trigger();
  settle(200);
  auto *gen = win->findChild<bda::integration::SequenceGeneratorWindow *>();
  check(gen != nullptr && gen->isVisible(), "and it opens the generator window");
  if (!gen)
  {
    std::cout << "RESULT: FAIL" << std::endl;
    _exit(1);
  }

  // Four shapes and an image patch, bottom to top, each moving its own way.
  auto &s  = gen->scene();
  s.width  = 320, s.height = 192, s.frames = 12;
  s.layers = {shape(ShapeKind::Rect, 20, 20, 60, 40, {180, 60, 200}, 3, 2),
              shape(ShapeKind::Circle, 200, 30, 50, 50, {150, 200, 70}, -4, 1),
              shape(ShapeKind::Triangle, 40, 110, 50, 44, {200, 80, 120}, 5, -1),
              shape(ShapeKind::Star, 230, 110, 56, 56, {210, 128, 128}, -2, -3)};
  Layer patch = shape(ShapeKind::Image, 140, 70, 32, 24, {}, 1, 1);
  QImage img(16, 12, QImage::Format_RGBA8888);
  for (int y = 0; y < 12; ++y)
    for (int x = 0; x < 16; ++x)
      img.setPixelColor(x, y, QColor(x * 16, y * 20, 255 - x * 8, 255));
  patch.patchW = 16, patch.patchH = 12;
  patch.rgba.assign(img.constBits(), img.constBits() + 16 * 12 * 4);
  s.layers.push_back(patch);
  gen->sceneChanged();

  const QString path = QDir::tempPath() + "/bd-generated-48.y4m";
  QString       error;
  check(gen->generate(path, &error), "Generate writes the y4m (" + error.toStdString() + ")");
  settle(2000);

  auto *tree = win->findChild<PlaylistTreeWidget *>();
  auto *item = tree && !tree->getSelectedItems().empty() ? tree->getSelectedItems()[0] : nullptr;
  check(item != nullptr && item->properties().name.contains("bd-generated-48"),
        "the generated file opens in the playlist by itself");
  if (item)
  {
    const auto range = item->properties().startEndRange;
    check(range.second - range.first + 1 == 12, "with 12 frames");
    check(item->getRawFrameSize().width == 320 && item->getRawFrameSize().height == 192, "at 320x192");
  }

  bda::me::YuvLumaReader reader;
  std::string            err;
  bool                   same = reader.openY4m(path.toStdString(), err) && reader.frameCount() == 12;
  for (int k : {0, 5, 11})
  {
    const auto plane = reader.readLuma(k, 0, err);
    const auto f     = renderFrame(s, k);
    for (int y = 0; same && y < 192; ++y)
      for (int x = 0; same && x < 320; ++x)
        same = plane.at(x, y) == f.y[std::size_t(y) * 320 + x];
  }
  check(same, "frames 0, 5 and 11 in the file are the renderer's frames");
  const auto f5 = renderFrame(s, 5);
  check(f5.y[std::size_t(20 + 2 * 5) * 320 + 20 + 3 * 5] == 180, "the rectangle has moved by 5 x MV(3, 2) at frame 5");

  // The MV fields take fractions: 1.25 typed into the selected layer's MV x reaches the scene.
  {
    if (auto *list = gen->findChild<QListWidget *>())
      list->setCurrentRow(0); // top of the list = the image patch, the last layer
    settle(100);
    QDoubleSpinBox *mvx = nullptr;
    for (auto *box : gen->findChildren<QDoubleSpinBox *>())
      if (box->minimum() == -512 && box->decimals() == 3 && !mvx)
        mvx = box;
    check(mvx != nullptr, "the MV x field is a fractional spin box");
    if (mvx)
    {
      mvx->setValue(1.25);
      settle(100);
      check(s.layers.back().motion.vx == 1.25, "and 1.25 reaches the layer's motion");
      mvx->setValue(1);
      settle(100);
    }
  }

  // Scene round trip.
  const QString scenePath = QDir::tempPath() + "/bd-generated-48.json";
  const Scene   before    = s;
  check(gen->saveScene(scenePath, &error), "the scene saves to JSON");
  gen->scene().layers.clear();
  check(gen->loadScene(scenePath, &error) && gen->scene().layers.size() == 5, "and loads back with its five layers");
  check(sameFrames(before, gen->scene()), "rendering the same frames");

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  _exit(g_failures == 0 ? 0 : 1);
}
