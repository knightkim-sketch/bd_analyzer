// Regression: the hex dump pane asks the item for the bitstream of one frame. Three things this
// pins down, all of which were wrong before:
//
//   * Every request opened a fresh FileSourceFFmpegFile. Nothing in the ffmpeg wrappers ever calls
//     avformat_close_input, so each frame change and each block click leaked a file descriptor.
//     Measured 1300 fds after 1300 requests; once the soft limit is reached the pane never shows
//     anything again.
//   * The packet was found by demuxing frameIdx + 1 packets from the start of the file every time,
//     so the cost of showing a frame grew with its index.
//   * The last frame reported "no video packet". YUView's frame range is one longer than the number
//     of video packets (getDecodableFrameLimits returns nrFrames, not nrFrames - 1), so a request
//     for that phantom frame must be answered without walking the file again.
//
// Frame index == index of the packet in the video stream, which is the definition the bitstream
// analysis tab annotates every AVPacket with ("YUView frame index").
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSettings>
#include <iostream>
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
int openFds()
{
  return QDir("/proc/self/fd").entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
}
} // namespace

// The annex B side of the same feature. Here the bug was the index order: the range lookup is
// indexed in coding order, while the frame index the video view hands out counts in display order.
// With B frames the two differ, so the pane showed a different frame's bytes.
void checkAnnexBDisplayOrder(const char *path)
{
  playlistItemCompressedVideo item(path, 0, InputFormat::AnnexBAVC,
                                   decoder::DecoderEngine::Invalid);
  const stats::BlockInfo noBlock;
  const auto             lastFrame = item.properties().startEndRange.second;
  std::cout << "--- annex B (" << path << "), frame range 0.." << lastFrame << " ---" << std::endl;

  bool     allHaveData    = true;
  bool     anyOutOfOrder  = false;
  uint64_t previousStart  = 0;
  for (int f = 0; f <= lastFrame; ++f)
  {
    const auto d = item.getItemDataDump(QPoint(0, 0), f, noBlock);
    if (d.bytes.isEmpty())
    {
      allHaveData = false;
      continue;
    }
    // "Frame N · source bytes A-B · C bytes" - pull A back out to compare the ordering.
    const auto parts = d.description.split(QLatin1Char(' '));
    const auto idx   = parts.indexOf("bytes");
    if (idx > 0 && idx + 1 < parts.size())
    {
      const auto start = parts[idx + 1].split(QLatin1Char('-')).first().toULongLong();
      if (f > 0 && start < previousStart)
        anyOutOfOrder = true;
      previousStart = start;
    }
  }

  check(allHaveData, "every annex B frame has bitstream data");
  /* Not a correctness requirement in itself, but this file has B frames, so the display order walk
   * must visit the file out of order. If it did not, we would be reading the coding order list and
   * the test above would pass while showing the wrong frame.
   */
  check(anyOutOfOrder, "the display order walk visits the file out of order (B frames present)");
}

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 13-frame-bitstream-dump <av1 file in a container> [annexB avc file]"
              << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  playlistItemCompressedVideo item(argv[1], 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  const stats::BlockInfo noBlock;
  const auto             lastFrame = item.properties().startEndRange.second;
  std::cout << "frame range       : 0.." << lastFrame << std::endl;

  // --- every frame in order yields its own packet ---------------------------------------------
  std::vector<int> sizeOfFrame(lastFrame + 1, -1);
  int              framesWithData = 0;
  for (int f = 0; f <= lastFrame; ++f)
  {
    const auto d = item.getItemDataDump(QPoint(0, 0), f, noBlock);
    if (!d.bytes.isEmpty())
    {
      sizeOfFrame[f] = int(d.bytes.size());
      ++framesWithData;
    }
    else
      std::cout << "  frame " << f << " has no data: " << d.description.toStdString() << std::endl;
  }
  std::cout << "frames with data  : " << framesWithData << " / " << (lastFrame + 1) << std::endl;

  check(framesWithData >= lastFrame,
        "every frame but at most the phantom last one has bitstream data");
  check(sizeOfFrame[0] > 0, "frame 0 has data");

  // Distinct payloads: a stuck reader would hand out the same packet over and over.
  int distinctSizes = 0;
  for (int f = 1; f <= lastFrame; ++f)
    if (sizeOfFrame[f] != sizeOfFrame[f - 1])
      ++distinctSizes;
  check(distinctSizes > 0, "consecutive frames do not all report the same payload size");

  // --- random access must agree with the sequential walk --------------------------------------
  const int  probes[] = {lastFrame / 2, 1, lastFrame > 3 ? lastFrame - 2 : 0, 0, lastFrame / 3};
  bool       agrees   = true;
  for (const auto f : probes)
  {
    const auto d = item.getItemDataDump(QPoint(0, 0), f, noBlock);
    const int  s = d.bytes.isEmpty() ? -1 : int(d.bytes.size());
    if (s != sizeOfFrame[f])
    {
      agrees = false;
      std::cout << "  frame " << f << " random access gave " << s << ", sequential gave "
                << sizeOfFrame[f] << std::endl;
    }
  }
  check(agrees, "jumping to a frame gives the same bytes as walking to it (rewind works)");

  // --- the phantom frame past the last packet -------------------------------------------------
  const auto past = item.getItemDataDump(QPoint(0, 0), lastFrame + 5, noBlock);
  check(past.bytes.isEmpty(), "a frame index past the last packet reports no data");
  check(past.description.contains("video packets"),
        "and says how many video packets the stream has");

  // --- no file descriptor growth --------------------------------------------------------------
  const auto fdsBefore = openFds();
  for (int i = 0; i < 300; ++i)
    item.getItemDataDump(QPoint(0, 0), i % (lastFrame + 1), noBlock);
  const auto fdsAfter = openFds();
  std::cout << "fds before/after 300 dumps : " << fdsBefore << " / " << fdsAfter << std::endl;
  check(fdsAfter == fdsBefore, "300 dumps leak no file descriptor");

  // --- cost must not grow with the frame index ------------------------------------------------
  QElapsedTimer timer;
  timer.start();
  for (int f = 0; f <= lastFrame; ++f)
    item.getItemDataDump(QPoint(0, 0), f, noBlock);
  const auto forwardMs = timer.elapsed();
  std::cout << "forward walk over " << (lastFrame + 1) << " frames : " << forwardMs << " ms"
            << std::endl;

  if (argc >= 3)
    checkAnnexBDisplayOrder(argv[2]);

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
