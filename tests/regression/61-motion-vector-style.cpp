// Regression: motion vectors are drawn, 3 wide, for every decoder (patch 0081, DecoderJm).
//
// A vector type made with StatisticsType(id, name, scaling) and no style draws with the default
// LineDrawStyle - black, 0.25 wide - which hardly shows over a picture. That was the JM decoder's
// motion vectors (H.264: "the vectors are missing"), and also FFmpeg's, libde265's and HM's. All
// motion vector types now carry a colour (list 0 / past red, list 1 / future blue) and width 3;
// dav1d's went from 2 to 3.
//   * the motion vector types of JM, FFmpeg (H.264) and dav1d (AV1) are 3 wide and coloured;
//   * JM's vectors really reach the picture: painting only "Motion vector L0" of the pan frame
//     leaves red pixels;
//   * JM's intra direction arrows are switched on.
//
// args: <pan .h264> <av1 .ivf>
#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "statistics/StatisticsDataPainting.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << std::endl;
  if (!condition)
    ++failures;
}

// The item's statistics are protected; a test-only subclass reads them.
struct Item : playlistItemCompressedVideo
{
  using playlistItemCompressedVideo::playlistItemCompressedVideo;
  stats::StatisticsData &stats() { return this->statisticsData; }
  void useEngine(const QString &name)
  {
    if (auto *combo = this->getPropertiesWidget()->findChild<QComboBox *>("comboBoxDecoder"))
      combo->setCurrentIndex(combo->findText(name));
  }
};

class Loader : public QThread
{
public:
  Loader(Item &item, int frame) : item(item), frame(frame) {}
  void run() override { this->item.loadFrame(this->frame, false, true, false); }

private:
  Item &item;
  int   frame;
};

void checkStyles(Item &item, const std::string &what, std::initializer_list<std::pair<const char *, int>> types)
{
  for (const auto &[name, red] : types)
  {
    const stats::StatisticsType *found = nullptr;
    for (const auto &t : item.stats().getStatisticsTypes())
      if (t.typeName == name)
        found = &t;
    if (!found)
    {
      check(false, what + ": has a type \"" + name + "\"");
      continue;
    }
    const auto &style = found->vectorStyle;
    check(found->renderVectorData && style.width == 3.0 &&
              (red ? style.color == Color(255, 0, 0) : style.color == Color(0, 0, 255)),
          what + ": \"" + name + "\" is drawn " + (red ? "red" : "blue") + ", width " + std::to_string(style.width));
  }
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear();
  if (argc < 3)
  {
    std::cerr << "usage: 61-motion-vector-style <pan.h264> <av1.ivf>\n";
    return 2;
  }

  {
    Item item(argv[1]);
    item.useEngine("JM (analysis)");
    checkStyles(item, "JM", {{"Motion vector L0", 1}, {"Motion vector L1", 0}});
    for (const auto &t : item.stats().getStatisticsTypes())
      if (t.typeName == "Intra luma mode")
        check(t.renderVectorData && !t.renderVectorDataValues, "JM: intra direction arrows are drawn, without values");

    // Paint only the list 0 vectors of a P frame of the pan onto a black canvas.
    item.setBlockInfoRequested(true);
    Loader loader(item, 4);
    loader.start();
    loader.wait();
    for (auto &t : item.stats().getStatisticsTypes())
      t.render = t.typeName == "Motion vector L0";
    QImage canvas(176 * 8, 144 * 8, QImage::Format_ARGB32);
    canvas.fill(Qt::black);
    {
      QPainter painter(&canvas);
      // The painter works around the picture centre, as the view does.
      painter.translate(canvas.width() / 2, canvas.height() / 2);
      stats::paintStatisticsData(&painter, item.stats(), 4, 8.0); // zoomed in: 2 samples are 16 pixels
    }
    if (const char *dump = std::getenv("BDA_TEST_DUMP_PNG"))
      canvas.save(dump);
    int red = 0;
    for (int y = 0; y < canvas.height(); ++y)
      for (int x = 0; x < canvas.width(); ++x)
      {
        const auto c = canvas.pixelColor(x, y);
        // The type's opacity (alphaFactor) halves the red over the black canvas.
        red += c.red() > 90 && c.green() < 40 && c.blue() < 40;
      }
    check(red > 2000, "JM: the list 0 vectors of the pan frame paint red pixels (" + std::to_string(red) + ")");
  }
  {
    Item item(argv[1]);
    item.useEngine("FFMpeg");
    checkStyles(item, "FFmpeg (H.264)", {{"Motion Vector -", 1}, {"Motion Vector +", 0}});
  }
  {
    Item item(argv[2]);
    checkStyles(item, "dav1d (AV1)", {{"Motion Vector 0", 1}, {"Motion Vector 1", 0}});
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
