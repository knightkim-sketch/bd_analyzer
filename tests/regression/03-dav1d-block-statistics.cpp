// Headless verification that the vendored dav1d 0.2.2 fork actually yields
// AV1 block-level statistics through YUView's decoderDav1d path.
//
// Exercises the real production path:
//   decoderDav1d ctor          -> QLibrary load of libdav1d-internals.so + symbol resolve
//   statisticsSupported()      -> internalsSupported, i.e. the analyzer API was found
//   FileSourceFFmpegFile       -> demux OBUs
//   pushData / decodeNextFrame -> decode
//   StatisticsData             -> per-block stats harvested from Dav1dPicture.blk_data
#include <QApplication>
#include <iostream>

#include "decoder/decoderDav1d.h"
#include "filesource/FileSourceFFmpegFile.h"
#include "statistics/StatisticsData.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: dav1d_allframes_check <av1 file>\n";
    return 2;
  }

  decoder::decoderDav1d dec(0, false);
  if (dec.errorInDecoder())
  {
    std::cout << "decoder error: " << dec.decoderErrorString().toStdString() << "\n";
    std::cout << "RESULT: FAIL - library not loaded\n";
    return 1;
  }
  for (const auto &p : dec.getLibraryPaths())
    std::cout << "  lib: " << p.toStdString() << "\n";
  std::cout << "decoder            : " << dec.getDecoderName().toStdString() << "\n";
  std::cout << "statisticsSupported: " << (dec.statisticsSupported() ? "TRUE" : "FALSE")
            << "   <- analyzer API (block stats gate)\n";
  std::cout << "nrSignalsSupported : " << dec.nrSignalsSupported() << "\n";

  stats::StatisticsData statsData;
  dec.enableStatisticsRetrieval(&statsData);
  // Required: allocateNewDecoder() only sets export_blkdata when statistics are
  // already enabled, so the decoder built in the ctor must be re-allocated.
  // decoderBase.h documents this ("activate it, reset the decoder").
  dec.resetDecoder();
  // decoderDav1d re-declares this override as private; it is public on the base.
  static_cast<decoder::decoderBase &>(dec).fillStatisticList(statsData);
  std::cout << "declared stat types: " << statsData.getStatisticsTypes().size() << "\n";

  FileSourceFFmpegFile file;
  // parseFile=false: the bitstream scan drives a QProgressDialog built from the
  // mainWindow argument, which we do not have in a headless harness. That also
  // skips the initial seek, so do it explicitly.
  if (!file.openFile(argv[1], nullptr, nullptr, false))
  {
    std::cout << "RESULT: FAIL - could not open input via ffmpeg\n";
    return 1;
  }
  file.seekFileToBeginning();

  // Decode the first frame. We push whole AVPackets (= AV1 temporal units), which is
  // what dav1d_send_data expects, rather than splitting into individual OBUs.
  int  framesDecoded = 0;
  bool repush        = false;
  int  pushes        = 0;
  while (framesDecoded < 999)
  {
    if (dec.state() == decoder::DecoderState::NeedsMoreData)
    {
      auto pkt = file.getNextPacket(repush, true);
      if (!pkt)
      {
        std::cout << "no more packets after " << pushes << " pushes (atEnd="
                  << file.atEnd() << ")\n";
        break;
      }
      auto data = QByteArray::fromRawData((const char *) pkt.getData(), pkt.getDataSize());
      if (pushes == 0)
        std::cout << "first TU size    : " << data.size() << " bytes\n";
      ++pushes;
      repush = !dec.pushData(data);
    }
    else if (dec.state() == decoder::DecoderState::RetrieveFrames)
    {
      if (dec.decodeNextFrame())
      {
        const auto raw = dec.getRawFrameData();
        const auto sz  = dec.getFrameSize();
        std::cout << "frame " << framesDecoded << ": " << sz.width << "x" << sz.height
                  << ", " << raw.size() << " bytes" << std::endl;
        ++framesDecoded;
      }
    }
    else
    {
      std::cout << "decoder stopped in state "
                << (dec.errorInDecoder() ? dec.decoderErrorString().toStdString() : "EOF") << "\n";
      break;
    }
  }

  // Report which statistic types actually carry per-block data for this frame.
  int typesWithData = 0, totalValues = 0, totalVectors = 0;
  for (const auto &t : statsData.getStatisticsTypes())
  {
    if (!statsData.hasDataForTypeID(t.typeID))
      continue;
    const auto d = statsData.getFrameTypeData(t.typeID);
    if (d.valueData.empty() && d.vectorData.empty())
      continue;
    ++typesWithData;
    totalValues += int(d.valueData.size());
    totalVectors += int(d.vectorData.size());
    if (typesWithData <= 8)
      std::cout << "    [" << t.typeID << "] " << t.typeName.toStdString()
                << "  values=" << d.valueData.size() << " vectors=" << d.vectorData.size() << "\n";
  }
  std::cout << "types with data    : " << typesWithData << "\n";
  std::cout << "total block values : " << totalValues << "\n";
  std::cout << "total block vectors: " << totalVectors << "\n";

  const bool pass = dec.statisticsSupported() && framesDecoded > 0 && typesWithData > 0;
  std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
  return pass ? 0 : 1;
}
