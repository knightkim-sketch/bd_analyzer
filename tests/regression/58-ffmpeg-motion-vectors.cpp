// Regression: the FFmpeg decoder's motion vector overlay on H.264 (patch 0077).
//
// Two defects hid it:
//   * AVMotionVectorWrapper knew avutil 54-57 only. FFmpeg 7.1 is avutil 59, so the side data was
//     never read and the overlay stayed empty whatever the stream;
//   * the vector was the whole-sample dst - src difference, entered into types registered in
//     quarter samples (vector scaling 4) - a quarter of its length, and of the opposite sign to the
//     codec's motion vector, which the other decoders report.
//
// The stream is a known pan: a 176x144 window over testsrc2 moving 2 samples right per frame,
// x264 without B-frames. A block of frame n shows what frame n-1 showed 2 samples further right,
// so its motion vector (reference position minus block position) is (+2, 0).
//
// args: <pan .h264>
#include <QApplication>
#include <QSettings>
#include <QThread>

#include <iostream>
#include <map>
#include <string>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"

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

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear();
  if (argc < 2)
  {
    std::cerr << "usage: 58-ffmpeg-motion-vectors <pan.h264>\n";
    return 2;
  }

  playlistItemCompressedVideo item(argv[1]);
  item.setBlockInfoRequested(true);
  for (const int frame : {3, 6})
  {
    Loader loader(item, frame);
    loader.start();
    loader.wait();
    std::map<std::string, int> vectors;
    int                        total = 0;
    for (int y = 8; y < 136; y += 16)
      for (int x = 8; x < 168; x += 16)
        for (const auto &e : item.getBlockInfoAt(QPoint(x, y), frame).entries)
          if (e.typeID == 2)
          {
            ++vectors[e.valueText.toStdString()];
            ++total;
          }
    std::string mostCommon = "-";
    int         count      = 0;
    for (const auto &[text, n] : vectors)
      if (n > count)
        mostCommon = text, count = n;
    for (const auto &[text, n] : vectors)
      std::cout << "    " << text << " x" << n << std::endl;
    std::cout << "  frame " << frame << ": " << total << " past-reference vectors, most common " << mostCommon
              << " (" << count << ")" << std::endl;
    check(total > 20, "frame " + std::to_string(frame) + ": the motion vector overlay has vectors");
    check(mostCommon == "(2,0)" && count * 2 > total,
          "frame " + std::to_string(frame) + ": most are (+2, 0), the pan, in samples");
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
