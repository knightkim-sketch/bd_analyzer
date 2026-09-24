// Compare two AV1 streams superblock by superblock, on a fixed window of the coded bits.
//
//   stream-diff-sb <a> <b> [--frames N] [--start N] [--sb 64|128] [--bits 24]
//
// This is layer B of Find diff. It answers "roughly where did these two streams stop agreeing",
// and deliberately nothing more:
//
//   * The bit offsets are the arithmetic decoder's read positions, not symbol boundaries. Measured
//     on real streams the per-superblock spans are monotonic in raster order but can overlap by a
//     few bits, so the window start is approximate.
//   * Once one symbol differs, every later bit differs too, whatever the syntax says. So a window
//     mismatch means "the divergence is at or before here", never "this is the symbol that broke".
//
// The block that actually differs is found by looking at syntax after this point - layer C.
//
// KNOWN LIMITATION - only sound for streams whose display frames are their coded frames.
//
// The offsets are relative to the packet payload the decoder read them from, and getItemDataDump()
// hands back the packet of the *display* frame. Those are the same packet only while no frame is
// hidden. Measured on a stream with alternate-reference frames: display frame 2's dump is five
// bytes (a show_existing_frame packet) while its blocks report bits 74003..82560, which live in
// the packet before it. Rebasing against the wrong buffer shows up as "unreadable" superblocks,
// which is why that count is printed rather than folded into the totals - but a frame whose window
// happens to fit the wrong buffer would compare wrong bits silently.
//
// The fix is to walk coded frames rather than display frames; until then, read the unreadable
// count first and distrust any frame where it is not zero.
#include <QApplication>
#include <QSettings>

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "diff/BitWindow.h"
#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{

// loadFrame() is written for the application's loader threads and asserts it is not on the GUI
// thread. A release build drops the assert; the contract still holds.
void loadFrameOffMainThread(playlistItemCompressedVideo &item, int frameIdx)
{
  std::thread loader([&item, frameIdx] { item.loadFrame(frameIdx, false, true, false); });
  loader.join();
}

struct FrameProbes
{
  std::vector<bda::diff::SuperblockProbe> probes;
  QByteArray                              bytes;
  long                                    coveredPx{};
  bool                                    ok{};
};

/* Collect one probe per superblock: the earliest bit position any block inside it reported.
 *
 * Offsets come back stream-absolute while the dump's bytes start at baseOffset, so they are
 * rebased here - comparing a stream-absolute offset against a buffer that starts elsewhere would
 * read the wrong bytes without ever failing.
 */
FrameProbes collectFrame(playlistItemCompressedVideo &item, int frameIdx, int sbSize, int step)
{
  FrameProbes out;
  loadFrameOffMainThread(item, frameIdx);

  const auto size = item.getSize();
  if (size.width() <= 0)
    return out;

  const auto dump = item.getItemDataDump(QPoint(0, 0), frameIdx, {});
  if (dump.bytes.isEmpty())
    return out;
  out.bytes                = dump.bytes;
  const auto baseOffsetBit = std::uint64_t(dump.baseOffset) * 8;

  std::map<std::pair<int, int>, std::uint64_t> earliest; // (sbY, sbX) -> smallest startBit
  for (int y = 0; y < size.height(); y += step)
    for (int x = 0; x < size.width(); x += step)
    {
      const auto info = item.getBlockInfoAt(QPoint(x, y), frameIdx);
      if (!info.isValid || info.frameIndex != frameIdx)
        continue;
      out.coveredPx += long(step) * step;
      if (!info.bitstreamRange || !info.bitstreamRange->isValid())
        continue;
      const auto abs = info.bitstreamRange->startBit;
      if (abs < baseOffsetBit)
        continue; // outside the dumped buffer; cannot be read here
      const auto key = std::make_pair((y / sbSize) * sbSize, (x / sbSize) * sbSize);
      const auto rel = abs - baseOffsetBit;
      auto       it  = earliest.find(key);
      if (it == earliest.end() || rel < it->second)
        earliest[key] = rel;
    }

  for (const auto &[key, startBit] : earliest)
    out.probes.push_back({key.second, key.first, startBit, true});
  out.ok = true;
  return out;
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  std::vector<std::string> files;
  int                      startFrame = 0, nrFrames = 8, sbSize = 64, step = 4;
  unsigned                 windowBits = 24;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--frames" && i + 1 < argc)      nrFrames   = std::stoi(argv[++i]);
    else if (a == "--start" && i + 1 < argc)  startFrame = std::stoi(argv[++i]);
    else if (a == "--sb" && i + 1 < argc)     sbSize     = std::stoi(argv[++i]);
    else if (a == "--bits" && i + 1 < argc)   windowBits = unsigned(std::stoi(argv[++i]));
    else if (a == "--step" && i + 1 < argc)   step       = std::stoi(argv[++i]);
    else                                      files.push_back(a);
  }
  if (files.size() != 2)
  {
    std::cerr << "usage: stream-diff-sb <a> <b> [--frames N] [--start N] [--sb 64|128] "
                 "[--bits 24]\n";
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerStreamDiffSb");
  QCoreApplication::setApplicationName("bdAnalyzerStreamDiffSb");
  QSettings().clear();

  playlistItemCompressedVideo itemA(QString::fromStdString(files[0]), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
  playlistItemCompressedVideo itemB(QString::fromStdString(files[1]), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
  itemA.setBlockInfoRequested(true);
  itemB.setBlockInfoRequested(true);

  const auto sizeA = itemA.getSize();
  const auto sizeB = itemB.getSize();
  if (sizeA != sizeB)
  {
    std::cerr << "pictures differ in size - refusing to compare\n";
    return 1;
  }
  const auto range    = itemA.properties().startEndRange;
  const int  lastFrame = std::min(range.second, startFrame + nrFrames - 1);
  std::cout << "picture " << sizeA.width() << "x" << sizeA.height() << ", superblock " << sbSize
            << ", window " << windowBits << " bits, frames " << startFrame << ".." << lastFrame
            << "\n\n";

  bool anyDiff = false;
  for (int frame = startFrame; frame <= lastFrame; ++frame)
  {
    const auto a = collectFrame(itemA, frame, sbSize, step);
    const auto b = collectFrame(itemB, frame, sbSize, step);
    if (!a.ok || !b.ok)
    {
      std::cout << "frame " << frame << ": could not read a frame dump on both sides\n";
      continue;
    }

    const auto total    = double(sizeA.width()) * sizeA.height();
    const auto coverage = std::min(a.coveredPx, b.coveredPx) / total * 100.0;

    const auto diffs = bda::diff::compareBitWindows(
        a.probes, reinterpret_cast<const std::uint8_t *>(a.bytes.constData()),
        std::size_t(a.bytes.size()), b.probes,
        reinterpret_cast<const std::uint8_t *>(b.bytes.constData()),
        std::size_t(b.bytes.size()), {windowBits});

    std::size_t differ = 0, unreadable = 0, missing = 0;
    for (const auto &d : diffs)
      switch (d.result)
      {
      case bda::diff::WindowCompare::Differ:     ++differ; break;
      case bda::diff::WindowCompare::Unreadable: ++unreadable; break;
      case bda::diff::WindowCompare::Missing:    ++missing; break;
      case bda::diff::WindowCompare::Equal:      break;
      }

    std::cout << "frame " << frame << ": " << diffs.size() << " superblocks, " << differ
              << " windows differ";
    if (unreadable) std::cout << ", " << unreadable << " unreadable";
    if (missing)    std::cout << ", " << missing << " missing";
    std::cout << "   (block coverage " << int(coverage) << "%)\n";
    if (coverage < 95.0)
      std::cout << "    WARNING low block coverage - the statistics for this frame look "
                   "incomplete, so absence of a difference means little\n";

    if (const auto *first = bda::diff::firstDiffering(diffs))
    {
      anyDiff = true;
      std::cout << "    first differing superblock at (" << first->x << "," << first->y << ")"
                << "   A=0x" << std::hex << first->bitsA << "  B=0x" << first->bitsB << std::dec
                << "\n"
                << "    -> the divergence is at or before this superblock; layer C decides which "
                   "block it is\n";
      break;
    }
  }
  if (!anyDiff)
    std::cout << "\nno superblock window differed in the frames examined\n";

  std::cout.flush();
  // The decoder keeps threads and dlopened libraries; teardown after the output is written is not
  // worth the risk in a batch tool.
  _exit(0);
}
