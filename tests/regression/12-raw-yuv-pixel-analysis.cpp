// Regression: the raw YUV pixel analysis - per 64x64 block luma statistics, their disk cache under
// .YUViewBD, the luma histogram, and the 3x3 convolution filter.
//
// What this pins down:
//   * The statistics must equal an independent computation over the same samples. They are stored as
//     float32 in a cache file, so a layout or endianness slip would show up here.
//   * The cache file must be reused: a fresh item for the same source has to answer immediately,
//     without recomputing. That is the whole point of writing it to disk.
//   * The convolution is luma only, normalized by the coefficient sum, and the identity kernel is a
//     no-op. Chroma must come out untouched.
//   * The filter changes the samples, so it is part of the cache key: statistics computed with a
//     different kernel must not be served for the current one.
#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QElapsedTimer>
#include <QTimer>
#include <cmath>
#include <iostream>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "playlistitem/playlistItemRawFile.h"
#include "video/yuv/videoHandlerYUV.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

// Independent reference: read the luma plane straight from the file and compute the stats by hand.
struct Ref
{
  double mean{}, variance{};
  int    minimum{}, maximum{};
};
Ref referenceBlock(const QString &path, int w, int h, int frameIdx, int bx, int by, int blockSize)
{
  QFile f(path);
  f.open(QIODevice::ReadOnly);
  const qint64 bytesPerFrame = qint64(w) * h * 3 / 2; // 4:2:0 8 bit
  f.seek(bytesPerFrame * frameIdx);
  const auto luma = f.read(qint64(w) * h);
  double     sum = 0, sumSq = 0;
  int        mn = 255, mx = 0, n = 0;
  for (int y = by * blockSize; y < std::min((by + 1) * blockSize, h); y++)
    for (int x = bx * blockSize; x < std::min((bx + 1) * blockSize, w); x++)
    {
      const int v = uint8_t(luma[y * w + x]);
      sum += v;
      sumSq += double(v) * v;
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      ++n;
    }
  Ref r;
  r.mean     = sum / n;
  r.variance = sumSq / n - r.mean * r.mean;
  r.minimum  = mn;
  r.maximum  = mx;
  return r;
}

class Probe : public playlistItemRawFile
{
public:
  using playlistItemRawFile::playlistItemRawFile;
  video::yuv::videoHandlerYUV *yuv()
  {
    return dynamic_cast<video::yuv::videoHandlerYUV *>(this->video.get());
  }
};

// Load the frame and let the background statistics job finish.
bool computeFrame(Probe &item, int frameIdx)
{
  item.loadFrame(frameIdx, false, true, false);
  QEventLoop loop;
  QObject::connect(&item, &playlistItemRawFile::pixelStatisticsReady, &loop, &QEventLoop::quit);
  QTimer::singleShot(15000, &loop, &QEventLoop::quit); // do not hang if it never arrives
  item.requestPixelStatistics(frameIdx);
  if (item.getPixelBlockStats(QPoint(0, 0), frameIdx))
    return true; // already cached
  loop.exec();
  return item.getPixelBlockStats(QPoint(0, 0), frameIdx).has_value();
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerProbe");
  QCoreApplication::setApplicationName("bdAnalyzerProbe");
  QSettings().clear();

  if (argc < 2)
  {
    std::cerr << "usage: 12-raw-yuv-pixel-analysis <yuv file> [width] [height]" << std::endl;
    return 2;
  }
  const QString path = argv[1];
  const int     w = 176, h = 144, blockSize = 64, frameIdx = 3;

  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    check(item.supportsPixelStatistics(), "raw YUV item reports pixel statistics support");
    check(item.getDefaultGridSize() == 64, "default grid size is 64");
    check(item.getPixelStatisticsBlockSize() == 64, "statistics block size is 64");

    check(computeFrame(item, frameIdx), "statistics became available for the frame");

    // Every 64x64 block against the reference.
    int mismatches = 0;
    for (int by = 0; by < (h + 63) / 64; by++)
      for (int bx = 0; bx < (w + 63) / 64; bx++)
      {
        const auto got = item.getPixelBlockStats(QPoint(bx * 64, by * 64), frameIdx);
        if (!got)
        {
          ++mismatches;
          continue;
        }
        const auto ref = referenceBlock(path, w, h, frameIdx, bx, by, blockSize);
        if (std::abs(got->mean - ref.mean) > 0.01 ||
            std::abs(got->variance - ref.variance) > 1.0 ||
            int(got->minimum) != ref.minimum || int(got->maximum) != ref.maximum)
        {
          std::cout << "    block (" << bx << "," << by << ") got mean " << got->mean << " var "
                    << got->variance << " min " << got->minimum << " max " << got->maximum
                    << " / ref mean " << ref.mean << " var " << ref.variance << " min "
                    << ref.minimum << " max " << ref.maximum << std::endl;
          ++mismatches;
        }
      }
    check(mismatches == 0, "all 3x3 blocks match an independent computation");

    // A block outside the picture must not answer.
    check(!item.getPixelBlockStats(QPoint(1000, 1000), frameIdx),
          "a position outside the picture has no statistics");

    const auto histogram = item.getLumaHistogram(frameIdx);
    check(histogram.size() == 256, "the histogram has 256 bins");
    uint64_t total = 0;
    for (const auto count : histogram)
      total += count;
    check(total == uint64_t(w) * h, "the histogram counts every luma sample");
  }

  // --- the cache file must be reused by a fresh item -------------------------------------------
  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    item.loadFrame(frameIdx, false, true, false);
    item.requestPixelStatistics(frameIdx);
    const auto got = item.getPixelBlockStats(QPoint(0, 0), frameIdx);
    check(got.has_value(), "a new item reads the statistics back from the cache file immediately");
  }

  // --- convolution filter ----------------------------------------------------------------------
  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    item.loadFrame(frameIdx, false, true, false);
    int  loaded = -1;
    auto before = item.yuv()->getCurrentRawYUVData(loaded);

    video::yuv::ConvolutionFilter identity;
    identity.enabled = true; // kernel is the identity by default
    item.yuv()->setConvolutionFilter(identity);
    item.loadFrame(frameIdx, false, true, false);
    auto afterIdentity = item.yuv()->getCurrentRawYUVData(loaded);
    check(afterIdentity == before, "an identity kernel leaves the samples untouched");

    video::yuv::ConvolutionFilter blur;
    blur.enabled = true;
    blur.kernel  = {{1, 1, 1, 1, 1, 1, 1, 1, 1}};
    item.yuv()->setConvolutionFilter(blur);
    item.loadFrame(frameIdx, false, true, false);
    auto afterBlur = item.yuv()->getCurrentRawYUVData(loaded);
    check(afterBlur != before, "a box blur changes the samples");

    // One interior pixel by hand: mean of its 3x3 neighborhood, rounded.
    const int x = 100, y = 70;
    long      sum = 0;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        sum += uint8_t(before[(y + dy) * w + (x + dx)]);
    const auto expected = std::lround(double(sum) / 9.0);
    const auto actual   = int(uint8_t(afterBlur[y * w + x]));
    check(actual == expected,
          "the blurred pixel equals the 3x3 mean (" + std::to_string(actual) + " vs " +
              std::to_string(expected) + ")");

    // Chroma must be untouched: only luma is filtered.
    const qint64 lumaBytes = qint64(w) * h;
    check(afterBlur.mid(lumaBytes) == before.mid(lumaBytes), "chroma planes are left alone");
  }

  // --- the filter is part of the cache key ------------------------------------------------------
  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    video::yuv::ConvolutionFilter blur;
    blur.enabled = true;
    blur.kernel  = {{1, 1, 1, 1, 1, 1, 1, 1, 1}};
    item.yuv()->setConvolutionFilter(blur);
    check(computeFrame(item, frameIdx), "statistics are recomputed for the filtered samples");
    const auto filtered = item.getPixelBlockStats(QPoint(64, 64), frameIdx);
    Probe plain(path, QSize(w, h), "YUV 4:2:0 8-bit");
    check(computeFrame(plain, frameIdx), "the unfiltered statistics are available again");
    const auto unfiltered = plain.getPixelBlockStats(QPoint(64, 64), frameIdx);
    check(filtered && unfiltered && std::abs(filtered->variance - unfiltered->variance) > 1.0,
          "the filtered and unfiltered statistics differ, so the cache key separates them");
  }

  // --- how long one frame of statistics takes ---------------------------------------------------
  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    item.loadFrame(5, false, true, false);
    int  loaded = -1;
    auto raw    = item.yuv()->getCurrentRawYUVData(loaded);
    QElapsedTimer timer;
    timer.start();
    const auto blocks = stats::computeLumaBlockStats(
        raw, item.yuv()->getPixelFormatYUV(), item.yuv()->getFrameSize(), 64);
    const auto elapsedUs = timer.nsecsElapsed() / 1000;
    std::cout << "timing            : " << blocks.size() << " blocks of " << w << "x" << h << " in "
              << elapsedUs << " us" << std::endl;
    check(!blocks.empty(), "the timing run produced statistics");
  }

  /* --- the same analysis must work on a decoded stream ------------------------------------------
   * The machinery lives in playlistItemWithVideo, so a compressed item gets it for free. What is
   * item specific is the block size: it follows the coding grid, which for AV1 is the superblock.
   */
  if (argc > 2)
  {
    playlistItemCompressedVideo av1(argv[2], 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
    av1.loadFrame(frameIdx, false, true, false);
    check(av1.supportsPixelStatistics(), "a decoded AV1 item reports pixel statistics support");
    check(av1.getPixelStatisticsBlockSize() == av1.getDefaultGridSize() &&
              av1.getDefaultGridSize() > 0,
          "the AV1 statistics block size is the superblock grid size");

    QEventLoop loop;
    QObject::connect(&av1, &playlistItem::pixelStatisticsReady, &loop, &QEventLoop::quit);
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    av1.requestPixelStatistics(frameIdx);
    if (!av1.getPixelBlockStats(QPoint(0, 0), frameIdx))
      loop.exec();

    const auto block = av1.getPixelBlockStats(QPoint(64, 64), frameIdx);
    check(block.has_value(), "AV1 block statistics are available");
    check(block && block->maximum >= block->minimum, "the AV1 values are self consistent");
    const auto hist = av1.getLumaHistogram(frameIdx);
    uint64_t   sum  = 0;
    for (const auto count : hist)
      sum += count;
    check(hist.size() == 256 && sum == uint64_t(w) * h,
          "the AV1 histogram counts every luma sample");
  }
  else
    std::cout << "  skip  AV1 checks (no compressed stream given)" << std::endl;

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
