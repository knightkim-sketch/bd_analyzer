// How long does it take to reach a late frame of a 4K stream, and does it survive?
//
// Step 4 of the Find diff pipeline decodes the one frame whose payload differs. On this sample
// that is frame 90 of 3840x2160 10-bit, and a sequence sweep has run out of memory on this code
// before (patch 0041), so the cost is measured before the design leans on it.
#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>

#include <cstdio>
#include <iostream>
#include <thread>

#include "playlistitem/playlistItemCompressedVideo.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3) { std::cerr << "usage: probe-decode-cost <stream> <frameIdx>\n"; return 2; }
  const int target = std::stoi(argv[2]);

  QCoreApplication::setOrganizationName("bdAnalyzerProbeDecodeCost");
  QCoreApplication::setApplicationName("bdAnalyzerProbeDecodeCost");
  QSettings().clear();

  playlistItemCompressedVideo item(QString::fromUtf8(argv[1]), 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  item.setBlockInfoRequested(true);
  const auto size = item.getSize();
  if (size.width() <= 0) { std::cerr << "could not open\n"; return 2; }
  printf("picture %dx%d, target frame %d\n", size.width(), size.height(), target);

  QElapsedTimer timer;
  timer.start();
  std::thread loader([&item, target] { item.loadFrame(target, false, true, false); });
  loader.join();
  const auto ms = timer.elapsed();

  const auto info = item.getBlockInfoAt(QPoint(0, 0), target);
  printf("loadFrame(%d): %lld ms, block info valid=%d frameIndex=%d\n",
         target, (long long) ms, int(info.isValid), info.frameIndex);
  fflush(stdout);
  _exit(0);
}
