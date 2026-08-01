// Hypothesis: destroying one decoderFFmpeg unloads the ffmpeg libraries process-wide, breaking
// a FileSourceFFmpegFile that is still open and holding resolved function pointers.
#include <QApplication>
#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>
#include "decoder/decoderFFmpeg.h"
#include "filesource/FileSourceFFmpegFile.h"

static int countMaps(const char *needle)
{
  std::ifstream m("/proc/self/maps");
  std::string   line;
  int           n = 0;
  while (std::getline(m, line))
    if (line.find(needle) != std::string::npos)
      ++n;
  return n;
}

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QWidget      w;

  auto *file = new FileSourceFFmpegFile();
  if (!file->openFile(argv[1], &w, nullptr, true))
  {
    std::cout << "openFile failed" << std::endl;
    _exit(1);
  }
  std::cout << "after file open      : libavcodec maps = " << countMaps("libavcodec.so") << std::endl;

  auto *dec = new decoder::decoderFFmpeg(file->getVideoCodecPar());
  std::cout << "after decoder create : libavcodec maps = " << countMaps("libavcodec.so") << std::endl;

  delete dec;
  const int mapsAfter = countMaps("libavcodec.so");
  std::cout << "after decoder delete : libavcodec maps = " << mapsAfter
            << "   <-- 0 means unloaded process-wide" << std::endl;

  std::cout << "now calling the still-open file source ..." << std::endl;
  std::cout.flush();
  file->seekFileToBeginning();
  auto pkt = file->getNextPacket(false, true);
  const int pktSize = pkt ? int(pkt.getDataSize()) : -1;
  std::cout << "getNextPacket size   : " << pktSize << std::endl;

  const bool pass = mapsAfter > 0 && pktSize > 0;
  if (mapsAfter == 0)
    std::cout << "FAIL: the ffmpeg libraries were unloaded process-wide" << std::endl;
  if (pktSize <= 0)
    std::cout << "FAIL: the still-open file source could not read a packet" << std::endl;
  std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(pass ? 0 : 1);
}
