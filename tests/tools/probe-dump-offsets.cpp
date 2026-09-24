// What buffer are the block bit offsets actually relative to?
//
// compareBitWindows reported most superblocks unreadable, which means the rebased offsets fall
// outside dump.bytes. This prints the two things that have to agree: the extent of the dump and
// the extent of the block offsets the decoder reported.
#include <QApplication>
#include <QSettings>

#include <cstdio>
#include <iostream>
#include <thread>

#include "playlistitem/playlistItemCompressedVideo.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2) { std::cerr << "usage: probe-dump-offsets <stream> [frames]\n"; return 2; }
  const int nrFrames = argc >= 3 ? std::stoi(argv[2]) : 4;

  QCoreApplication::setOrganizationName("bdAnalyzerProbeDumpOffsets");
  QCoreApplication::setApplicationName("bdAnalyzerProbeDumpOffsets");
  QSettings().clear();

  playlistItemCompressedVideo item(QString::fromUtf8(argv[1]), 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  item.setBlockInfoRequested(true);
  const auto size = item.getSize();
  if (size.width() <= 0) { std::cerr << "could not open\n"; return 2; }

  for (int f = 0; f < nrFrames; ++f)
  {
    std::thread loader([&item, f] { item.loadFrame(f, false, true, false); });
    loader.join();

    const auto dump = item.getItemDataDump(QPoint(0, 0), f, {});
    std::uint64_t lo = ~0ull, hi = 0;
    int withRange = 0;
    for (int y = 0; y < size.height(); y += 4)
      for (int x = 0; x < size.width(); x += 4)
      {
        const auto info = item.getBlockInfoAt(QPoint(x, y), f);
        if (!info.isValid || info.frameIndex != f) continue;
        if (!info.bitstreamRange || !info.bitstreamRange->isValid()) continue;
        ++withRange;
        lo = std::min(lo, info.bitstreamRange->startBit);
        hi = std::max(hi, info.bitstreamRange->endBit);
      }
    printf("frame %d: dump bytes=%lld baseOffset=%llu (covers bits %llu..%llu)\n",
           f, (long long) dump.bytes.size(), (unsigned long long) dump.baseOffset,
           (unsigned long long) dump.baseOffset * 8,
           (unsigned long long) (dump.baseOffset + std::uint64_t(dump.bytes.size())) * 8);
    if (withRange)
      printf("         block bit range: %llu .. %llu  (%d blocks)   origin=\"%s\"\n",
             (unsigned long long) lo, (unsigned long long) hi, withRange,
             item.getBlockInfoAt(QPoint(0, 0), f).bitstreamRange
                 ? item.getBlockInfoAt(QPoint(0, 0), f).bitstreamRange->origin.toUtf8().constData()
                 : "");
    else
      printf("         no block reported a bit range\n");
  }
  std::cout.flush();
  fflush(stdout);
  _exit(0);
}
