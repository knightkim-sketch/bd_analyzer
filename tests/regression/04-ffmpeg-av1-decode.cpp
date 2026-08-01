// Test the DEFAULT AV1 decode path in the GUI: DecoderEngine::FFMpeg over a Libav
// (container) input. Mirrors playlistItemCompressedVideo::allocateDecoder's else-branch
// (decoderFFmpeg(getVideoCodecPar())) and loadRawData's pushAVPacket loop.
#include <QApplication>
#include <QWidget>
#include <iostream>
#include <unistd.h>

#include "decoder/decoderFFmpeg.h"
#include "filesource/FileSourceFFmpegFile.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
    return 2;

  QWidget              mainWindow;
  FileSourceFFmpegFile file;
  if (!file.openFile(argv[1], &mainWindow, nullptr, true))
  {
    std::cout << "RESULT: FAIL - openFile\n";
    return 1;
  }

  decoder::decoderFFmpeg dec(file.getVideoCodecPar());
  if (dec.errorInDecoder())
  {
    std::cout << "decoder error: " << dec.decoderErrorString().toStdString() << std::endl;
    std::cout << "RESULT: FAIL - decoder init\n";
    return 1;
  }
  std::cout << "decoder     : " << dec.getDecoderName().toStdString() << std::endl;
  std::cout << "codec       : " << dec.getCodecName().toStdString() << std::endl;

  int  framesDecoded = 0, pushes = 0;
  bool repush = false;
  while (framesDecoded < 3)
  {
    if (dec.state() == decoder::DecoderState::NeedsMoreData)
    {
      auto pkt = file.getNextPacket(repush, true);
      repush   = false;
      if (!pkt)
      {
        // EOF: flush the decoder with an empty push, as the item does.
        QByteArray empty;
        dec.pushData(empty);
        std::cout << "EOF after " << pushes << " packets\n";
        if (dec.state() != decoder::DecoderState::RetrieveFrames)
          break;
        continue;
      }
      ++pushes;
      repush = !dec.pushAVPacket(pkt);
    }
    else if (dec.state() == decoder::DecoderState::RetrieveFrames)
    {
      if (dec.decodeNextFrame())
      {
        const auto raw = dec.getRawFrameData();
        const auto sz  = dec.getFrameSize();
        std::cout << "  frame " << framesDecoded << ": " << sz.width << "x" << sz.height << ", "
                  << raw.size() << " bytes, fmt="
                  << (dec.getRawFormat() == video::RawFormat::YUV ? "YUV" : "RGB") << " "
                  << dec.getPixelFormatYUV().getName() << std::endl;
        ++framesDecoded;
      }
    }
    else
    {
      std::cout << "stopped: "
                << (dec.errorInDecoder() ? dec.decoderErrorString().toStdString() : "unknown state")
                << std::endl;
      break;
    }
  }

  std::cout << "frames decoded: " << framesDecoded << std::endl;
  std::cout << "RESULT: " << (framesDecoded > 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  // Skip local destructors: the two FFmpegVersionHandler instances tear down the
  // shared libraries in an order this probe does not control (YUView destroys the
  // file source before the decoder; see playlistItemCompressedVideo.h:126 vs :153).
  _exit(framesDecoded > 0 ? 0 : 1);
}
