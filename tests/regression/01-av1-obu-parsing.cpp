// Headless verification that YUView's own AV1 analysis path works against the
// freshly built FFmpeg 7.1.2 shared libraries.
//
// Exercises the real production path:
//   FFmpegVersionHandler::loadFFmpegLibraries()  -> dlopen + symbol bind
//   ParserAVFormat::runParsingOfFile()           -> libavformat demux
//                                                -> ParserAV1OBU on the OBU payloads
#include <QApplication>
#include <QAbstractItemModel>
#include <iostream>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: av1_parse_check <file>\n";
    return 2;
  }

  FFmpeg::FFmpegVersionHandler ff;
  ff.loadFFmpegLibraries();
  for (const auto &l : ff.getLog())
    std::cout << "  [ffmpeg] " << l.toStdString() << "\n";

  if (!ff.loadingSuccessfull())
  {
    std::cout << "RESULT: FAIL - ffmpeg libraries did not load\n";
    return 1;
  }
  std::cout << "ffmpeg libraries loaded: " << ff.getLibVersionString().toStdString() << "\n";

  parser::ParserAVFormat p;
  p.enableModel();
  const auto ok = p.runParsingOfFile(std::filesystem::path(argv[1]));
  // PacketItemModel::rowCount() reports a cached counter that only this call
  // refreshes; the GUI drives it from the modelDataUpdated() signal.
  p.updateNumberModelItems();

  std::cout << "runParsingOfFile      : " << (ok ? "true" : "false") << "\n";
  std::cout << "nr streams            : " << p.getNrStreams() << "\n";
  std::cout << "video stream index    : " << p.getVideoStreamIndex() << "\n";
  std::cout << "stream description    : "
            << p.getShortStreamDescription(p.getVideoStreamIndex()) << "\n";

  auto *model = p.getPacketItemModel();
  const auto topLevel = model ? model->rowCount() : 0;
  std::cout << "top-level packet items: " << topLevel << "\n";

  // Walk into the first few packets and print the OBU children, which is the
  // part that proves ParserAV1OBU actually ran.
  int obuChildren = 0;
  for (int i = 0; i < topLevel && i < 5; ++i)
  {
    const auto packetIdx = model->index(i, 0);
    const auto name      = model->data(packetIdx).toString();
    const auto nChildren = model->rowCount(packetIdx);
    obuChildren += nChildren;
    std::cout << "  packet[" << i << "] \"" << name.toStdString() << "\" children=" << nChildren
              << "\n";
    for (int j = 0; j < nChildren && j < 4; ++j)
    {
      const auto childIdx = model->index(j, 0, packetIdx);
      std::cout << "      - " << model->data(childIdx).toString().toStdString()
                << "  (grandchildren=" << model->rowCount(childIdx) << ")\n";
    }
  }

  const bool pass = ok && topLevel > 0 && obuChildren > 0;
  std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
  return pass ? 0 : 1;
}
