// Headless per-block syntax dump for one AV1 stream.
//
//   av1-block-dump <stream> [--start N] [--frames N] [--out file.csv] [--step N]
//
// Writes one CSV row per coding block: frame, block rect, the decoder's bit range for that block,
// and every syntax value the block-info pane would show, as name=value pairs.
//
// Exists so two encodings of the same source can be compared block by block: the GUI shows this
// for one clicked pixel at a time, which does not scale to "find the first block that differs".
// Drives the same production path as the pane (playlistItemCompressedVideo::getBlockInfoAt), so
// what is dumped is what the pane would show.
//
// The picture is walked on a 4x4 grid (the smallest AV1 block) and blocks are deduplicated by
// their coding-block rect, because one query returns the block covering that pixel.
#include <QApplication>
#include <QSettings>

#include <thread>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{
struct Row
{
  int         x{}, y{}, w{}, h{};
  uint64_t    bitStart{}, bitEnd{};
  bool        hasBits{};
  std::string syntax;
};

/* loadFrame() opens with Q_ASSERT(currentThread() != the GUI thread): it is written for the
 * loader threads the application runs it on, not for the thread that owns the widgets. A release
 * build compiles that assert out, so calling it from main() goes unnoticed - it is still the wrong
 * thread. One short-lived thread per frame keeps the contract without pulling in the application's
 * whole loading machinery.
 */
void loadFrameOffMainThread(playlistItemCompressedVideo &item, int frameIdx)
{
  std::thread loader([&item, frameIdx] { item.loadFrame(frameIdx, false, true, false); });
  loader.join();
}

std::string entriesToText(const stats::BlockInfo &info)
{
  // entries are already deduplicated and ascending by typeID, so the text is stable between runs.
  std::string out;
  for (const auto &e : info.entries)
  {
    if (!out.empty())
      out += "|";
    out += e.typeName.toStdString() + "=" + e.valueText.toStdString();
  }
  return out;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  std::string path, outPath;
  int         startFrame = 0, nrFrames = -1, step = 4;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--start" && i + 1 < argc)
      startFrame = std::stoi(argv[++i]);
    else if (a == "--frames" && i + 1 < argc)
      nrFrames = std::stoi(argv[++i]);
    else if (a == "--step" && i + 1 < argc)
      step = std::stoi(argv[++i]);
    else if (a == "--out" && i + 1 < argc)
      outPath = argv[++i];
    else if (path.empty())
      path = a;
  }
  if (path.empty())
  {
    std::cerr << "usage: av1-block-dump <stream> [--start N] [--frames N] [--out file.csv] "
                 "[--step N]"
              << std::endl;
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerBlockDump");
  QCoreApplication::setApplicationName("bdAnalyzerBlockDump");
  QSettings().clear();

  playlistItemCompressedVideo item(QString::fromStdString(path), 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  item.setBlockInfoRequested(true);

  const auto size  = item.getSize();
  const auto range = item.properties().startEndRange;
  if (size.width() <= 0 || range.second < range.first)
  {
    std::cerr << "could not open " << path << std::endl;
    return 2;
  }
  const int lastFrame =
      (nrFrames > 0) ? std::min(range.second, startFrame + nrFrames - 1) : range.second;

  std::ofstream file;
  if (!outPath.empty())
    file.open(outPath);
  std::ostream &os = outPath.empty() ? std::cout : file;

  os << "frame,x,y,w,h,bit_start,bit_end,syntax\n";
  std::cerr << "picture " << size.width() << "x" << size.height() << ", frames " << startFrame
            << ".." << lastFrame << std::endl;

  for (int frameIdx = startFrame; frameIdx <= lastFrame; ++frameIdx)
  {
    loadFrameOffMainThread(item, frameIdx);

    std::set<std::tuple<int, int, int, int>> seen;
    std::vector<Row>                         rows;
    /* Completeness is measured in pixels, not in rows.
     *
     * Block counts swing wildly and legitimately: an intra frame of this clip yields 792 small
     * blocks, its inter frames a few dozen large ones covering the same picture. Counting rows
     * therefore says nothing about whether the dump is whole - the area the blocks cover does.
     */
    long coveredPx = 0, staleQueries = 0;
    for (int y = 0; y < size.height(); y += step)
      for (int x = 0; x < size.width(); x += step)
      {
        const auto info = item.getBlockInfoAt(QPoint(x, y), frameIdx);
        // Statistics can lag the displayed frame; only keep what really belongs to this frame.
        if (!info.isValid)
          continue;
        if (info.frameIndex != frameIdx)
        {
          ++staleQueries;
          continue;
        }
        coveredPx += long(step) * step;
        const auto r   = info.codingBlockRect;
        const auto key = std::make_tuple(r.x(), r.y(), r.width(), r.height());
        if (!seen.insert(key).second)
          continue;
        Row row;
        row.x      = r.x();
        row.y      = r.y();
        row.w      = r.width();
        row.h      = r.height();
        row.syntax = entriesToText(info);
        if (info.bitstreamRange && info.bitstreamRange->isValid())
        {
          row.hasBits  = true;
          row.bitStart = info.bitstreamRange->startBit;
          row.bitEnd   = info.bitstreamRange->endBit;
        }
        rows.push_back(row);
      }

    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
      return std::tie(a.y, a.x) < std::tie(b.y, b.x);
    });
    for (const auto &r : rows)
    {
      os << frameIdx << "," << r.x << "," << r.y << "," << r.w << "," << r.h << ",";
      if (r.hasBits)
        os << r.bitStart << "," << r.bitEnd;
      else
        os << ",";
      os << ",\"" << r.syntax << "\"\n";
    }
    const double coverage = 100.0 * double(coveredPx) / (double(size.width()) * size.height());
    std::cerr << "frame " << frameIdx << ": " << rows.size() << " blocks, " << std::fixed
              << std::setprecision(1) << coverage << "% of the picture covered";
    if (staleQueries > 0)
      std::cerr << ", " << staleQueries << " queries answered for another frame";
    std::cerr << std::endl;
    // Low coverage is the symptom that means the statistics really are incomplete. Say so rather
    // than writing a short dump that looks like a finished one.
    if (coverage < 95.0)
      std::cerr << "  WARNING frame " << frameIdx << " is incomplete - " << coverage
                << "% covered. Comparing this frame against another stream would report "
                   "differences that are really missing data."
                << std::endl;
  }

  os.flush();
  // The decoder keeps threads and dlopened libraries around; Qt teardown after that is not worth
  // the risk in a batch tool that has already written its output.
  _exit(0);
}
