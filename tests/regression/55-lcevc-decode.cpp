// Regression: LCEVC decoding of a compressed item (patch 0074) against the MPEG reference.
//
// Opens each .lvc the way File > Open does and decodes it through the item's own loading path
// (loadRawData, the slot the video handler calls), then compares every frame with LTM's
// ModelDecoder (dithering off; per frame MD5 in tests/data/lcevc/*.ltm-md5.txt):
//   * the stream offers the two LCEVC engines and opens under LTM, at the enhanced size;
//   * every frame in order, then out of order (a backward seek restarts at an entry picture);
//   * the "Base" display signal is the base picture scaled 2x (every other sample is the base);
//   * the LoQ-0 residual signal of a picture with residuals is not flat, and the LoQ-0 residual
//     overlay has blocks;
//   * the Frame tab of the info pane shows the picture configuration (step widths, IDR, sizes);
//   * LCEVCdec gives the same pictures as LTM;
//   * switching to the base decoder goes back to the base size and picture.
//
// args: <hevc-base .lvc> <its md5 list> <h264-base .lvc> <its md5 list>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QLabel>
#include <QTabWidget>
#include <QSettings>
#include <QThread>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "ui/widgets/FileInfoWidget.h"
#include "video/videoHandler.h"
#include "video/yuv/videoHandlerYUV.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << std::endl;
  if (!condition)
    ++failures;
}

struct Reference
{
  std::vector<std::string> enhanced, base;
};

Reference readReference(const std::string &path)
{
  Reference     r;
  std::ifstream f(path);
  std::string   line;
  while (std::getline(f, line))
  {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream s(line);
    int                idx;
    std::string        e, b;
    s >> idx >> e >> b;
    r.enhanced.push_back(e);
    r.base.push_back(b);
  }
  return r;
}

// Frames whose MD5 is not the reference's, for the log.
std::string listMismatches(const std::vector<std::string> &got, const std::vector<std::string> &want)
{
  std::string out;
  for (std::size_t i = 0; i < got.size() && i < want.size(); ++i)
    if (got[i] != want[i])
      out += " " + std::to_string(i);
  return out.empty() ? "" : " mismatch at" + out;
}

QByteArray decodeFrame(playlistItemCompressedVideo &item, int frame)
{
  QMetaObject::invokeMethod(&item, "loadRawData", Qt::DirectConnection, Q_ARG(int, frame), Q_ARG(bool, false));
  auto *video = dynamic_cast<video::videoHandler *>(item.getFrameHandler());
  return video && video->rawData_frameIndex == frame ? video->rawData : QByteArray();
}

std::string md5(const QByteArray &data)
{
  return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex().toStdString();
}

// Every other sample of every other row, per plane (4:2:0): undoes a nearest-neighbour 2x upscale.
QByteArray halve420(const QByteArray &frame, int width, int height)
{
  QByteArray out;
  const auto *p = reinterpret_cast<const unsigned char *>(frame.constData());
  for (int plane = 0; plane < 3; ++plane)
  {
    const int w = plane ? width / 2 : width, h = plane ? height / 2 : height;
    for (int y = 0; y < h; y += 2)
      for (int x = 0; x < w; x += 2)
        out.append(char(p[y * w + x]));
    p += w * h;
  }
  return out;
}

class Loader : public QThread
{
public:
  Loader(playlistItemCompressedVideo &item, int frame) : item(item), frame(frame) {}
  void run() override { this->item.loadFrame(this->frame, false, true, false); }

private:
  playlistItemCompressedVideo &item;
  int                          frame;
};

void runStream(const std::string &path, const std::string &md5Path, bool checkResidual)
{
  std::cout << "== " << path.substr(path.rfind('/') + 1) << " ==" << std::endl;
  const auto ref = readReference(md5Path);
  const int  n   = int(ref.enhanced.size());

  playlistItemCompressedVideo item(QString::fromStdString(path));
  auto *props   = item.getPropertiesWidget();
  auto *decoder = props->findChild<QComboBox *>("comboBoxDecoder");
  auto *signal  = props->findChild<QComboBox *>("comboBoxDisplaySignal");
  if (!decoder || !signal)
  {
    check(false, "the properties widget has its combo boxes");
    return;
  }
  check(decoder->findText("LCEVC (LTM, analysis)") >= 0 && decoder->findText("LCEVC (LCEVCdec)") >= 0,
        "the decoder list offers both LCEVC engines");
  check(decoder->currentText() == "LCEVC (LTM, analysis)", "it opens under LTM (" + decoder->currentText().toStdString() + ")");
  const auto size = item.getFrameHandler()->getFrameSize();
  check(size.width == 512 && size.height == 288, "the picture has the enhanced size 512x288");
  check(item.properties().startEndRange.second == n - 1, "frame count " + std::to_string(n));

  std::vector<std::string> got;
  for (int f = 0; f < n; ++f)
    got.push_back(md5(decodeFrame(item, f)));
  int same = 0;
  for (int f = 0; f < n; ++f)
    same += got[std::size_t(f)] == ref.enhanced[std::size_t(f)];
  check(same == n, "LTM: every frame in order equals ModelDecoder (" + std::to_string(same) + "/" + std::to_string(n) + ")" +
                       listMismatches(got, ref.enhanced));

  same = 0;
  for (const int f : {n - 1, 1, n / 2, 0})
    same += md5(decodeFrame(item, f)) == ref.enhanced[std::size_t(f)];
  check(same == 4, "LTM: frames out of order (backward seeks) still equal ModelDecoder");

  signal->setCurrentIndex(signal->findText("Base"));
  got.clear();
  for (int f = 0; f < n; ++f)
    got.push_back(md5(halve420(decodeFrame(item, f), 512, 288)));
  same = 0;
  for (int f = 0; f < n; ++f)
    same += got[std::size_t(f)] == ref.base[std::size_t(f)];
  check(same == n, "the Base signal is the base picture, scaled 2x (" + std::to_string(same) + "/" + std::to_string(n) + ")" +
                       listMismatches(got, ref.base));

  if (checkResidual)
  {
    // The Frame tab: the IDR's picture configuration, from the syntax (no decoding needed).
    const auto info = item.getFrameInfo(0);
    auto       text = [&info](const std::string &name) {
      for (const auto &i : info.items)
        if (i.name == name)
          return i.text;
      return std::string("?");
    };
    check(info.title == "LCEVC" && text("nal_unit_type") == "29 (LCEVC_IDR)" &&
              text("step_width LoQ-1 / LoQ-0") == "512 / 1024" && text("Resolution") == "256x144 -> 512x288",
          "Frame tab info: IDR, step widths 512 / 1024, 256x144 -> 512x288 (" + text("nal_unit_type") + ", " +
              text("step_width LoQ-1 / LoQ-0") + ", " + text("Resolution") + ")");
    FileInfoWidget widget;
    widget.setFrameInfo(info);
    auto *label = widget.findChild<QLabel *>("frameItemInfo:step_width LoQ-1 / LoQ-0");
    auto *tabs  = widget.findChild<QTabWidget *>();
    check(label && label->text() == "512 / 1024" && tabs && tabs->isTabEnabled(1),
          "the Frame tab shows it and is enabled without an OBU model");
    widget.setFrameInfo(InfoData());
    check(!tabs->isTabEnabled(1), "and is disabled again for an item without frame info");

    signal->setCurrentIndex(signal->findText("LoQ-0 residual"));
    const auto residual = decodeFrame(item, 0);
    int        moved    = 0;
    for (int i = 0; i < 512 * 288 && i < residual.size(); ++i)
      moved += residual[i] != char(128);
    check(moved > 1000, "the LoQ-0 residual signal shows residuals (" + std::to_string(moved) + " luma samples off 128)");
    auto *yuv = dynamic_cast<video::yuv::videoHandlerYUV *>(item.getFrameHandler());
    check(yuv && yuv->showPixelValuesAsDiff, "and is shown as a difference");
    signal->setCurrentIndex(signal->findText("Enhanced"));

    item.setBlockInfoRequested(true);
    Loader loader(item, 0);
    loader.start();
    loader.wait();
    int blocks = 0;
    for (int y = 0; y < 288; y += 4)
      for (int x = 0; x < 512; x += 4)
        for (const auto &e : item.getBlockInfoAt(QPoint(x, y), 0).entries)
          blocks += e.typeID == 400;
    check(blocks > 100, "the LoQ-0 residual overlay has blocks (" + std::to_string(blocks) + " hits)");
  }
  else
    signal->setCurrentIndex(signal->findText("Enhanced"));

  decoder->setCurrentIndex(decoder->findText("LCEVC (LCEVCdec)"));
  same = 0;
  for (int f = 0; f < n; ++f)
    same += md5(decodeFrame(item, f)) == ref.enhanced[std::size_t(f)];
  check(same == n, "LCEVCdec: every frame equals ModelDecoder (" + std::to_string(same) + "/" + std::to_string(n) + ")");

  decoder->setCurrentIndex(decoder->findText("FFMpeg"));
  const auto baseSize = item.getFrameHandler()->getFrameSize();
  check(baseSize.width == 256 && baseSize.height == 144, "the FFmpeg base decoder goes back to 256x144");
  check(md5(decodeFrame(item, 0)) == ref.base[0], "and to the base picture");
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear();
  if (argc < 5)
  {
    std::cerr << "usage: 55-lcevc-decode <hevc.lvc> <hevc md5> <h264.lvc> <h264 md5>\n";
    return 2;
  }
  runStream(argv[1], argv[2], false);
  runStream(argv[3], argv[4], true);
  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
