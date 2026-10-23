// Regression: HEVC with the HM decoder (scripts/setup-hm.sh -> decoder/libHMDecoder.so, patches 0082-0083).
//
//   * an HEVC file opens under HM by default (libde265 is not shipped, so preferring it left the item
//     unusable);
//   * every frame decoded by HM equals the FFmpeg decode of the same file, 8-bit and 10-bit, with
//     B-frames (output reordering) - including the last ones, which only the end-of-stream flush
//     outputs (third_party/libhm/patches/0001-eof-flush.patch and patch 0083);
//   * HM's block statistics reach the item: types are listed and a click on the picture shows entries.
//
// args: <8bit.hevc> <10bit.hevc>
#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QThread>

#include <iostream>
#include <string>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "video/videoHandler.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << std::endl;
  if (!condition)
    ++failures;
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

QByteArray decodeFrame(playlistItemCompressedVideo &item, int frame)
{
  QMetaObject::invokeMethod(&item, "loadRawData", Qt::DirectConnection, Q_ARG(int, frame), Q_ARG(bool, false));
  auto *video = dynamic_cast<video::videoHandler *>(item.getFrameHandler());
  return video && video->rawData_frameIndex == frame ? video->rawData : QByteArray();
}

bool selectEngine(playlistItemCompressedVideo &item, const QString &name)
{
  auto *combo = item.getPropertiesWidget()->findChild<QComboBox *>("comboBoxDecoder");
  if (!combo || combo->findText(name) < 0)
    return false;
  combo->setCurrentIndex(combo->findText(name));
  return combo->currentText() == name;
}

// Every frame decoded by HM equals the FFmpeg decode of the same file.
void compareWithFfmpeg(const char *path, const std::string &what)
{
  playlistItemCompressedVideo ffmpeg(path);
  playlistItemCompressedVideo hm(path);
  check(selectEngine(ffmpeg, "FFMpeg"), what + ": FFmpeg is offered as well");
  const bool offered = selectEngine(hm, "HM");
  check(offered, what + ": the HM engine is offered and selected");
  if (!offered)
    return;
  const int n    = ffmpeg.properties().startEndRange.second + 1;
  int       same = 0;
  for (int f = 0; f < n; ++f)
  {
    const auto a = decodeFrame(ffmpeg, f), b = decodeFrame(hm, f);
    same += !a.isEmpty() && a == b;
    if (a.isEmpty() || a != b)
      std::cout << "    frame " << f << ": FFmpeg " << a.size() << " bytes, HM " << b.size() << " bytes" << std::endl;
  }
  check(same == n && n > 0, what + ": every frame equals FFmpeg's (" + std::to_string(same) + "/" + std::to_string(n) + ")");
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
    std::cerr << "usage: 62-hm-hevc-decode <8bit.hevc> <10bit.hevc>\n";
    return 2;
  }

  std::cout << "== opening: HM by default ==" << std::endl;
  {
    playlistItemCompressedVideo item(argv[1]);
    auto *combo = item.getPropertiesWidget()->findChild<QComboBox *>("comboBoxDecoder");
    check(combo && combo->currentText() == "HM", "HEVC opens under HM (" +
                                                     (combo ? combo->currentText().toStdString() : std::string("no combo")) + ")");
    check(!decodeFrame(item, 0).isEmpty(), "the first frame decodes");
  }

  std::cout << "== decoding ==" << std::endl;
  compareWithFfmpeg(argv[1], "8-bit 4:2:0, B-frames");
  compareWithFfmpeg(argv[2], "10-bit 4:2:0, B-frames");

  std::cout << "== block statistics ==" << std::endl;
  {
    playlistItemCompressedVideo item(argv[1]);
    selectEngine(item, "HM");
    item.setBlockInfoRequested(true);
    Loader loader(item, 1);
    loader.start();
    loader.wait();
    const auto info = item.getBlockInfoAt(QPoint(40, 40), 1);
    check(info.isValid && !info.entries.empty(),
          "a click on frame 1 shows HM block data (" + std::to_string(info.entries.size()) + " entries)");
    item.setBlockInfoRequested(false);
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
