// Regression: the filter cascade of the Convolution filter pane, as the item and the ME panel see it.
//
// What this pins down:
//   * A chain filters what is displayed, and readFilteredRawFrame() (the Y4M export) produces the
//     same bytes - the screen and the exported file must not disagree. Chroma is left alone.
//   * Motion estimation reads the source until "Use as ME input" is ticked, and the filtered frames
//     after. Ticking it makes the item report a change, which is what restarts the estimate.
//   * The pane's Add / Remove buttons build the chain; a second GM compensation stage is refused.
//   * With OpenCV: a GM compensation stage passes frame 0 through, and the display path - which has
//     to fetch frame k-1 itself, under the raw data lock - gives the same picture as the export path.
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <cmath>
#include <iostream>
#include <memory>

#include "filter/FilterChain.h"
#include "playlistitem/playlistItemRawFile.h"
#include "video/yuv/videoHandlerYUV.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

class Probe : public playlistItemRawFile
{
public:
  using playlistItemRawFile::playlistItemRawFile;
  video::yuv::videoHandlerYUV *yuv()
  {
    return dynamic_cast<video::yuv::videoHandlerYUV *>(this->video.get());
  }
};

QByteArray displayed(Probe &item, int frameIdx)
{
  item.loadFrame(frameIdx, false, true, false);
  int loaded = -1;
  return item.yuv()->getCurrentRawYUVData(loaded);
}

QPushButton *button(QWidget *pane, const QString &text)
{
  for (auto *b : pane->findChildren<QPushButton *>())
    if (b->text() == text)
      return b;
  return nullptr;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerProbe");
  QCoreApplication::setApplicationName("bdAnalyzerProbe");
  QSettings().clear();

  if (argc < 2)
  {
    std::cerr << "usage: 47-filter-chain-and-me-input <176x144 yuv420p file>" << std::endl;
    return 2;
  }
  const QString path = argv[1];
  const int     w = 176, h = 144, frameIdx = 3;
  const qint64  lumaBytes = qint64(w) * h;

  std::cout << "chain on the display and export paths" << std::endl;
  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    const auto source = displayed(item, frameIdx);

    video::yuv::ConvolutionFilter filter;
    filter.enabled = true;
    auto box       = bda::filter::FilterStage::withDefaults(bda::filter::StageType::Custom3x3);
    box.params     = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    filter.chain.stages = {box, box};
    item.yuv()->setConvolutionFilter(filter);

    const auto shown    = displayed(item, frameIdx);
    const auto exported = item.readFilteredRawFrame(frameIdx);
    check(shown != source, "the chain changes the displayed samples");
    check(shown == exported, "display and export produce the same bytes");
    check(shown.mid(lumaBytes) == source.mid(lumaBytes), "chroma is left alone");
    check(item.readRawFrame(frameIdx) == source, "readRawFrame still returns the source");

    // Two boxes by hand at one interior pixel: the cascade is the second box over the first.
    const auto at = [&](const QByteArray &f, int x, int y) { return int(uint8_t(f[y * w + x])); };
    const int  x = 90, y = 60;
    int        expected;
    {
      long sum = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
        {
          long inner = 0;
          for (int ey = -1; ey <= 1; ey++)
            for (int ex = -1; ex <= 1; ex++)
              inner += at(source, x + dx + ex, y + dy + ey);
          sum += std::lround(double(inner) / 9.0);
        }
      expected = int(std::lround(double(sum) / 9.0));
    }
    check(at(shown, x, y) == expected,
          "a two-stage cascade equals two 3x3 means by hand (" + std::to_string(at(shown, x, y)) +
              " vs " + std::to_string(expected) + ")");

    std::cout << "ME input" << std::endl;
    check(item.readMeFrame(frameIdx) == source, "ME reads the source while the box is unticked");
    int changes = 0;
    QObject::connect(&item, &playlistItem::SignalItemChanged, [&changes]() { ++changes; });
    item.yuv()->setFilterIsMeInput(true);
    check(item.readMeFrame(frameIdx) == exported, "ME reads the filtered frames once ticked");
    check(changes == 1, "ticking it reports one item change (that restarts ME)");
    item.yuv()->setFilterIsMeInput(true);
    check(changes == 1, "ticking it again reports nothing");

    filter.enabled = false;
    item.yuv()->setConvolutionFilter(filter);
    check(item.readMeFrame(frameIdx) == source, "a disabled filter gives ME the source again");
  }

  std::cout << "pane buttons" << std::endl;
  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    std::unique_ptr<QWidget> pane(item.yuv()->createConvolutionFilterControls());
    auto *combo  = pane->findChild<QComboBox *>();
    auto *list   = pane->findChild<QListWidget *>();
    auto *add    = button(pane.get(), "Add");
    auto *remove = button(pane.get(), "Remove");
    check(combo && list && add && remove, "the pane has the filter list, Add, the stages and Remove");
    if (combo && list && add && remove)
    {
      check(combo->itemText(0).contains("Custom 3x3"), "the list starts with Custom 3x3");
      combo->setCurrentIndex(0);
      add->click();
      add->click();
      const auto f = item.yuv()->getConvolutionFilter();
      check(list->count() == 2 && f.chain.stages.size() == 2, "Add twice gives two stages");
      check(f.enabled, "Add switches the filter on");
      list->setCurrentRow(0);
      remove->click();
      check(list->count() == 1 && item.yuv()->getConvolutionFilter().chain.stages.size() == 1,
            "Remove drops the selected stage");

      const int gm = combo->findData(int(bda::filter::StageType::GmCompensation));
      if (gm >= 0)
      {
        combo->setCurrentIndex(gm);
        add->click();
        add->click();
        int gmCount = 0;
        for (const auto &s : item.yuv()->getConvolutionFilter().chain.stages)
          gmCount += s.type == bda::filter::StageType::GmCompensation;
        check(gmCount == 1, "a second GM compensation stage is refused");
      }
    }
  }

  if (bda::filter::stageInfo(bda::filter::StageType::GmCompensation))
  {
    std::cout << "GM compensation (OpenCV build)" << std::endl;
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    video::yuv::ConvolutionFilter filter;
    filter.enabled = true;
    filter.chain.stages = {
        bda::filter::FilterStage::withDefaults(bda::filter::StageType::Gaussian),
        bda::filter::FilterStage::withDefaults(bda::filter::StageType::GmCompensation)};
    item.yuv()->setConvolutionFilter(filter);

    // Frame 0 has no previous frame: only the blur in front of GM acts.
    video::yuv::ConvolutionFilter blurOnly = filter;
    blurOnly.chain.stages.pop_back();
    auto frame0 = item.readRawFrame(0);
    blurOnly.applyToLuma(frame0, item.getRawPixelFormat(), item.getRawFrameSize());
    check(item.readFilteredRawFrame(0) == frame0, "frame 0 passes through the GM stage");

    // The display path fetches frame k-1 itself; it must agree with the export path, which reads
    // it from the file. Jump straight to the frame, so nothing was cached on the way.
    const auto shown    = displayed(item, frameIdx);
    const auto exported = item.readFilteredRawFrame(frameIdx);
    check(!shown.isEmpty() && shown == exported, "display and export agree with a GM stage");
    // And the display path still shows the requested frame after fetching the previous one.
    int loaded = -1;
    item.yuv()->getCurrentRawYUVData(loaded);
    check(loaded == frameIdx, "the buffer still holds the requested frame");
  }
  else
    std::cout << "  skip  GM compensation (built without OpenCV)" << std::endl;

  std::cout << (g_failures ? "FAILED" : "PASSED") << std::endl;
  return g_failures ? 1 : 0;
}
