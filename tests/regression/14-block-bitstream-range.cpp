// Regression: the hex dump pane can show the bytes a single block's symbols were read from.
//
// The chain is long and every link used to be missing or wrong:
//   * The dav1d analyzer library reports Av1Block::bitstream_start_bit/end_bit. Before, the fields
//     existed only behind an #if that nothing defined, so the pane always fell back to the whole
//     frame.
//   * The positions are in packet coordinates. YUView pushes one OBU at a time, so it declares that
//     OBU's offset in Dav1dData.m.offset and dav1d carries it to the tile data. A temporal unit with
//     several frame OBUs (hidden ALTREF frames) is the case that catches a wrong base: the displayed
//     frame's blocks must point into the last frame OBU, not the first.
//   * sizeof(Av1Block) has to agree with the library. Disagreement shifts every block read and
//     produces plausible looking garbage, so decoderDav1d refuses to load a mismatching library -
//     which means an available Dav1d engine here already proves the sizes match.
//
// AV1 codes block syntax with an arithmetic coder, so a block has no exact bit range. What must hold
// is that the reported intervals are inside the frame's tile data and together account for
// essentially all of it.
#include <QApplication>
#include <QSettings>
#include <iostream>
#include <map>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 14-block-bitstream-range <av1 file in a container>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  playlistItemCompressedVideo item(argv[1], 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  item.setBlockInfoRequested(true);

  const auto size = item.getSize();
  std::cout << "picture " << size.width() << "x" << size.height() << std::endl;

  // Frame 1 of the test stream is a temporal unit with several frame OBUs, which is the interesting
  // case; frame 0 is a plain key frame temporal unit.
  for (const int frameIdx : {0, 1})
  {
    std::cout << "--- frame " << frameIdx << " ---" << std::endl;
    item.loadFrame(frameIdx, false, true, false);

    const auto packet = item.getItemDataDump(QPoint(0, 0), frameIdx, stats::BlockInfo{});
    if (packet.bytes.isEmpty())
    {
      check(false, "the frame packet is available");
      continue;
    }
    const auto packetBits = uint64_t(packet.bytes.size()) * 8;

    // One entry per distinct coding block, keyed so the same block is not counted twice.
    std::map<std::pair<int, int>, stats::BitstreamRange> ranges;
    int                                                 blocks = 0, withRange = 0;
    for (int y = 0; y < size.height(); y += 4)
      for (int x = 0; x < size.width(); x += 4)
      {
        const auto info = item.getBlockInfoAt(QPoint(x, y), frameIdx);
        if (!info.isValid)
          continue;
        ++blocks;
        if (!info.bitstreamRange)
          continue;
        ++withRange;
        ranges[{info.codingBlockRect.y(), info.codingBlockRect.x()}] = *info.bitstreamRange;
      }

    std::cout << "  positions queried with a block: " << blocks << ", of those with a range: "
              << withRange << ", distinct blocks: " << ranges.size() << std::endl;

    check(blocks > 0, "blocks are found at all (statistics are being collected)");
    if (blocks == 0)
    {
      // Nothing else is meaningful, and it is worth saying why: a library whose Av1Block layout does
      // not match ours is refused, which leaves the item on the FFmpeg engine with no block data.
      std::cout << "  (no block statistics - is decoder/libdav1d-internals.so the build from "
                   "third_party/dav1d? see its README)"
                << std::endl;
      continue;
    }
    check(withRange == blocks, "every block reports a bitstream range");

    uint64_t first = ~uint64_t(0), last = 0, covered = 0;
    int      invalid = 0, outside = 0;
    for (const auto &kv : ranges)
    {
      const auto &r = kv.second;
      if (r.endBit <= r.startBit)
        ++invalid;
      if (r.endBit > packetBits)
        ++outside;
      covered += r.endBit - r.startBit;
      first = std::min(first, r.startBit);
      last  = std::max(last, r.endBit);
    }
    std::cout << "  byte span " << first / 8 << ".." << (last + 7) / 8 << " of "
              << packet.bytes.size() << ", bits accounted for " << covered << " of "
              << (last - first) << " in the span" << std::endl;

    check(invalid == 0, "no range ends at or before it starts");
    check(outside == 0, "no range reaches past the end of the packet");
    check(last <= packetBits && first < last, "the span lies inside the packet");

    /* The blocks of one frame must account for nearly all of the bits between the first and the last
     * of them - that is the tile data. A wrong base offset or wrong bit accounting shows up here as
     * a span that is far larger than what the blocks cover.
     */
    check(covered * 100 / std::max<uint64_t>(1, last - first) >= 80,
          "the blocks account for at least 80% of the bits they span");

    // The dump for a clicked block must be a slice of the packet, not the whole thing.
    const int  probeX = size.width() / 2, probeY = size.height() / 2;
    const auto info   = item.getBlockInfoAt(QPoint(probeX, probeY), frameIdx);
    const auto blockDump = item.getItemDataDump(QPoint(probeX, probeY), frameIdx, info);
    std::cout << "  block dump at (" << probeX << "," << probeY << "): '"
              << blockDump.title.toStdString() << "' " << blockDump.bytes.size() << " bytes, base "
              << blockDump.baseOffset << std::endl;
    check(blockDump.title == "Block Bitstream", "clicking a block gives a block dump");
    check(blockDump.bytes.size() < packet.bytes.size(),
          "the block dump is a slice of the packet, not the whole packet");
    check(blockDump.baseOffset + uint64_t(blockDump.bytes.size()) <= uint64_t(packet.bytes.size()),
          "the slice lies inside the packet");
    if (info.bitstreamRange)
    {
      // The block's own first byte has to be inside what is shown.
      const auto blockFirstByte = info.bitstreamRange->startBit / 8;
      check(blockFirstByte >= blockDump.baseOffset &&
                blockFirstByte < blockDump.baseOffset + uint64_t(blockDump.bytes.size()),
            "the shown window contains the block's own first byte");
    }
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
