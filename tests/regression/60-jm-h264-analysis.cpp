// Regression: H.264 macroblock analysis with the JM decoder (patch 0078, src/integration/DecoderJm).
//
// libbda-jm-analyzer.so (scripts/setup-jm-analyzer.sh) is checked against JM's own trace in
// third_party/jm-analyzer; this checks what the item makes of it:
//   * the "JM (analysis)" engine is offered for H.264 and decodes every frame like FFmpeg does
//     (CABAC with B-frames as Annex-B and MP4, CAVLC, four slices per picture);
//   * on a known pan (2 samples per frame, one reference) most list 0 vectors are (+2, 0);
//   * an IDR picture is all intra macroblocks;
//   * CAVLC: the macroblocks' bit ranges follow each other without gap or overlap, and the first
//     macroblock of a picture starts right after a slice NAL unit header in the bytes the hexdump
//     shows - in the file (Annex-B) and in the packet (MP4);
//   * MP4 with B-frames: the hexdump of a display frame is the packet that codes it, not the packet
//     of the same index.
//
// args: <cabac-b.h264> <cabac-b.mp4> <pan.h264> <cavlc.h264> <slices.h264>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QSettings>
#include <QThread>

#include <iostream>
#include <map>
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

void loadWithStatistics(playlistItemCompressedVideo &item, int frame)
{
  item.setBlockInfoRequested(true);
  Loader loader(item, frame);
  loader.start();
  loader.wait();
}

bool selectEngine(playlistItemCompressedVideo &item, const QString &name)
{
  auto *combo = item.getPropertiesWidget()->findChild<QComboBox *>("comboBoxDecoder");
  if (!combo || combo->findText(name) < 0)
    return false;
  combo->setCurrentIndex(combo->findText(name));
  return combo->currentText() == name;
}

// Every frame decoded by JM equals the FFmpeg decode of the same file.
void compareWithFfmpeg(const char *path, const std::string &what)
{
  playlistItemCompressedVideo ffmpeg(path);
  playlistItemCompressedVideo jm(path);
  const bool offered = selectEngine(jm, "JM (analysis)");
  check(offered, what + ": the JM engine is offered and selected");
  if (!offered)
    return;
  const int n    = ffmpeg.properties().startEndRange.second + 1;
  int       same = 0;
  for (int f = 0; f < n; ++f)
  {
    const auto a = decodeFrame(ffmpeg, f), b = decodeFrame(jm, f);
    same += !a.isEmpty() && a == b;
  }
  check(same == n && n > 0, what + ": every frame equals FFmpeg's (" + std::to_string(same) + "/" + std::to_string(n) + ")");
}

std::map<std::string, int> valuesOfType(playlistItemCompressedVideo &item, int frame, int typeId, int step)
{
  std::map<std::string, int> values;
  const auto size = item.getFrameHandler()->getFrameSize();
  for (int y = step / 2; y < int(size.height); y += step)
    for (int x = step / 2; x < int(size.width); x += step)
      for (const auto &e : item.getBlockInfoAt(QPoint(x, y), frame).entries)
        if (e.typeID == typeId)
          ++values[e.valueText.toStdString()];
  return values;
}

// The first macroblock's range starts right after a slice NAL unit header in `bytes`.
bool startsAfterSliceHeader(const QByteArray &bytes, std::uint64_t startBit)
{
  const auto start = qsizetype(startBit / 8);
  for (qsizetype i = start; i >= 0 && i > start - 48; --i)
  {
    const auto b = std::uint8_t(bytes[i]);
    const int  type = b & 0x1f;
    if ((b & 0x80) || (type != 1 && type != 5))
      continue;
    // Annex-B: 00 00 01 before it; MP4: a 4-byte length in front of it (the dump is a window of the
    // packet, so the length may reach past its end).
    if (i >= 3 && bytes[i - 1] == 1 && bytes[i - 2] == 0 && bytes[i - 3] == 0)
      return true;
    if (i >= 4)
    {
      const auto len = (std::uint32_t(std::uint8_t(bytes[i - 4])) << 24) | (std::uint32_t(std::uint8_t(bytes[i - 3])) << 16) |
                       (std::uint32_t(std::uint8_t(bytes[i - 2])) << 8) | std::uint8_t(bytes[i - 1]);
      if (len > std::uint32_t(start - i) && len < (1u << 24))
        return true;
    }
  }
  return false;
}

void checkBitRanges(const char *path, const std::string &what, int frame, bool cavlc)
{
  playlistItemCompressedVideo item(path);
  selectEngine(item, "JM (analysis)");
  loadWithStatistics(item, frame);
  const auto size = item.getFrameHandler()->getFrameSize();
  const int  mbs  = int(size.width / 16), rows = int(size.height / 16);
  std::uint64_t previousEnd = 0;
  int           contiguous = 0, pairs = 0;
  std::optional<stats::BitstreamRange> first;
  for (int my = 0; my < rows; ++my)
    for (int mx = 0; mx < mbs; ++mx)
    {
      const auto info = item.getBlockInfoAt(QPoint(mx * 16 + 8, my * 16 + 8), frame);
      if (!info.bitstreamRange)
        continue;
      if (!first)
        first = info.bitstreamRange;
      else if (cavlc)
      {
        ++pairs;
        contiguous += info.bitstreamRange->startBit == previousEnd;
      }
      previousEnd = info.bitstreamRange->endBit;
    }
  check(first.has_value(), what + ": macroblocks carry bit ranges");
  if (!first)
    return;
  if (cavlc)
    check(pairs > 0 && contiguous == pairs,
          what + ": CAVLC ranges are contiguous (" + std::to_string(contiguous) + "/" + std::to_string(pairs) + ")");
  const auto info = item.getBlockInfoAt(QPoint(8, 8), frame);
  const auto dump = item.getItemDataDump(QPoint(8, 8), frame, info);
  check(dump.title == "Block Bitstream", what + ": the hexdump shows the block (" + dump.title.toStdString() + ")");
  auto range = *info.bitstreamRange;
  if (range.origin == "source file")
  {
    // The dump rebased it; the frame's bytes start at its first NAL unit.
    const auto pos = dump.description.indexOf("source bytes ");
    const auto from = pos >= 0 ? dump.description.mid(pos + 13).section('-', 0, 0).toULongLong() : 0;
    range.startBit -= from * 8;
  }
  check(startsAfterSliceHeader(dump.bytes, range.startBit),
        what + ": the first macroblock starts right after a slice NAL unit header (" + range.origin.toStdString() + ")");
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear();
  if (argc < 6)
  {
    std::cerr << "usage: 60-jm-h264-analysis <cabac-b.h264> <cabac-b.mp4> <pan.h264> <cavlc.h264> <slices.h264>\n";
    return 2;
  }

  std::cout << "== decoding ==" << std::endl;
  compareWithFfmpeg(argv[1], "CABAC B-frames, Annex-B");
  compareWithFfmpeg(argv[2], "CABAC B-frames, MP4");
  compareWithFfmpeg(argv[4], "CAVLC");
  compareWithFfmpeg(argv[5], "four slices");

  std::cout << "== macroblock data ==" << std::endl;
  {
    playlistItemCompressedVideo item(argv[3]);
    selectEngine(item, "JM (analysis)");
    loadWithStatistics(item, 0);
    int intra = 0, total = 0;
    for (const auto &[text, n] : valuesOfType(item, 0, 1, 16))
    {
      total += n;
      intra += text.rfind("INTRA", 0) == 0 ? n : 0;
    }
    check(total > 0 && intra == total, "pan, IDR: every macroblock is intra (" + std::to_string(intra) + "/" + std::to_string(total) + ")");
    loadWithStatistics(item, 4);
    const auto vectors = valuesOfType(item, 4, 6, 8);
    int        total4 = 0;
    for (const auto &[text, n] : vectors)
      total4 += n;
    const int pan = vectors.count("(2,0)") ? vectors.at("(2,0)") : 0;
    check(total4 > 0 && pan * 2 > total4, "pan, frame 4: most list 0 vectors are (+2, 0) (" + std::to_string(pan) + "/" + std::to_string(total4) + ")");
    const auto qps = valuesOfType(item, 4, 9, 16);
    check(!qps.empty(), "QP is reported per macroblock (" + std::to_string(qps.size()) + " distinct values)");
    item.setBlockInfoRequested(false);
  }

  std::cout << "== bit ranges and hexdump ==" << std::endl;
  checkBitRanges(argv[4], "CAVLC, Annex-B", 2, true);
  checkBitRanges(argv[1], "CABAC, Annex-B", 1, false);
  checkBitRanges(argv[2], "CABAC, MP4", 1, false);

  std::cout << "== macroblock bits for the RD plot / BD-rate ==" << std::endl;
  {
    // CAVLC, Annex-B: the bits of all macroblocks of a picture are its slice data - the NAL units
    // less their headers - so they come close to the frame's bytes and never exceed them.
    playlistItemCompressedVideo item(argv[4]);
    selectEngine(item, "JM (analysis)");
    check(item.providesSuperblockBits(), "the JM engine provides bits per 16x16 block");
    loadWithStatistics(item, 2);
    const auto     blocks = item.getSuperblockBits(2);
    std::uint64_t  sum    = 0;
    for (const auto &b : blocks)
      sum += b.bits;
    const auto info      = item.getBlockInfoAt(QPoint(8, 8), 2);
    const auto dump      = item.getItemDataDump(QPoint(8, 8), 2, {});
    const auto frameBits = std::uint64_t(dump.bytes.size()) * 8;
    std::cout << "    frame 2: " << blocks.size() << " blocks, " << sum << " bits of " << frameBits << " frame bits" << std::endl;
    check(blocks.size() == 99 && sum > frameBits * 7 / 10 && sum <= frameBits,
          "one entry per macroblock, the sum within the frame's bytes and above 70% of them");
  }

  std::cout << "== MP4 hexdump follows the coding order ==" << std::endl;
  {
    playlistItemCompressedVideo item(argv[2]);
    selectEngine(item, "JM (analysis)");
    loadWithStatistics(item, 1);
    const auto info = item.getBlockInfoAt(QPoint(8, 8), 1);
    const auto dump = item.getItemDataDump(QPoint(8, 8), 1, info);
    check(dump.description.contains("Frame 1 · packet 3 (coding order)"),
          "display frame 1 (a B picture) shows packet 3: " + dump.description.left(80).toStdString());
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
