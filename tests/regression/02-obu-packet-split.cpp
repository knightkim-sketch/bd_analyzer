// The GUI feeds AV1 to dav1d via getNextUnit() (playlistItemCompressedVideo.cpp:770,
// branch "FFmpeg input + non-FFmpeg decoder"). Check what that actually returns.
#include <QApplication>
#include <QWidget>
#include <iostream>
#include <unistd.h>
#include "filesource/FileSourceFFmpegFile.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QWidget mainWindow;
  FileSourceFFmpegFile f;
  if (!f.openFile(argv[1], &mainWindow, nullptr, true)) { std::cout << "openFile FAIL\n"; return 1; }
  std::cout << "opened, codec=" << f.getVideoStreamCodecID().getCodecName().toStdString()
            << std::endl;
  f.seekFileToBeginning();
  int empties = 0, nonEmpty = 0;
  for (int i = 0; i < 12; ++i)
  {
    auto d = f.getNextUnit(false);
    std::cout << "  getNextUnit[" << i << "] size=" << d.size()
              << (d.isEmpty() ? "   <-- EMPTY" : "") << "  atEnd=" << f.atEnd() << std::endl;
    if (d.isEmpty()) { if (++empties >= 4) break; } else ++nonEmpty;
  }
  std::cout << "non-empty units: " << nonEmpty << ", empties: " << empties << std::endl;
  const bool pass = nonEmpty > 0 && empties == 0;
  std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(pass ? 0 : 1);
}
