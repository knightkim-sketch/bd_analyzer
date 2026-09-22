// Why does a per-block dump thin out after the first frame?
//
// av1-block-dump drops a queried position when getBlockInfoAt() reports isValid == false OR when
// it reports a different frame than the one asked for. Those are very different causes - the first
// is normal (a skip block carries no syntax), the second means the statistics have not arrived and
// the dump is silently incomplete. The tool cannot tell them apart; this counts them separately.
#include <QApplication>
#include <QSettings>
#include <iostream>
#include <cstdio>
#include <set>
#include <tuple>

#include "playlistitem/playlistItemCompressedVideo.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2) { std::cerr << "usage: probe-blockinfo-coverage <stream> [frames]\n"; return 2; }
  const int nrFrames = argc >= 3 ? std::stoi(argv[2]) : 6;

  QCoreApplication::setOrganizationName("bdAnalyzerProbeCoverage");
  QCoreApplication::setApplicationName("bdAnalyzerProbeCoverage");
  QSettings().clear();

  playlistItemCompressedVideo item(QString::fromUtf8(argv[1]), 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  item.setBlockInfoRequested(true);
  const auto size = item.getSize();
  if (size.width() <= 0) { std::cerr << "could not open\n"; return 2; }

  std::cout << "picture " << size.width() << "x" << size.height() << "\n";
  std::cout << "frame  queried  invalid  wrongFrame  ok  distinctBlocks  coveredPx%\n";
  for (int f = 0; f < nrFrames; ++f)
  {
    item.loadFrame(f, false, true, false);
    int queried = 0, invalid = 0, wrongFrame = 0, ok = 0;
    long coveredPx = 0;
    std::set<std::tuple<int, int, int, int>> blocks;
    for (int y = 0; y < size.height(); y += 4)
      for (int x = 0; x < size.width(); x += 4)
      {
        ++queried;
        const auto info = item.getBlockInfoAt(QPoint(x, y), f);
        if (!info.isValid)            { ++invalid;    continue; }
        if (info.frameIndex != f)     { ++wrongFrame; continue; }
        ++ok;
        coveredPx += 16;
        const auto r = info.codingBlockRect;
        blocks.insert(std::make_tuple(r.x(), r.y(), r.width(), r.height()));
      }
    printf("%5d  %7d  %7d  %10d  %4d  %14zu  %9.1f\n", f, queried, invalid, wrongFrame, ok,
           blocks.size(), 100.0 * coveredPx / (size.width() * (double) size.height()));
  }
  std::cout.flush();
  fflush(stdout);   // _exit skips stdio flushing, and the decoder makes normal teardown risky
  _exit(0);
}
