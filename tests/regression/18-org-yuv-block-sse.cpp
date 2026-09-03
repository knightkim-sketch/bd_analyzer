// Regression: the original ("org") YUV attached to an item, and the per 64x64 block SSE against it
// that ends up in every block's statistics record - what the "Load Org YUV" button in the playlist
// panel drives.
//
// What this pins down:
//   * With no original attached, sse must stay negative. That is what tells the UI to print "-"
//     instead of a wrong 0, which would read as "these frames are identical".
//   * With one attached, every block's sse must equal an independent sum of squared luma differences.
//   * Attaching an original invalidates what was computed without it: the records carry the SSE, so
//     serving the old ones would show "-" forever.
//   * The SSE survives the on-disk cache. It is the only double in the record, so a layout slip
//     between writing and reading would show up here.
//   * A file that is too short for even one frame must be refused, with a message.
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QTimer>
#include <cstdint>
#include <iostream>

#include "playlistitem/playlistItemRawFile.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

class Probe : public playlistItemRawFile
{
public:
  using playlistItemRawFile::playlistItemRawFile;
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

QByteArray readLumaPlane(const QString &path, int w, int h, int frameIdx)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  const qint64 bytesPerFrame = qint64(w) * h * 3 / 2; // 4:2:0 8 bit
  file.seek(bytesPerFrame * frameIdx);
  return file.read(qint64(w) * h);
}

// Independent reference: the sum of squared luma differences over one block.
double referenceSSE(const QByteArray &lumaA,
                    const QByteArray &lumaB,
                    int               w,
                    int               h,
                    int               bx,
                    int               by,
                    int               blockSize)
{
  double sse = 0;
  for (int y = by * blockSize; y < std::min((by + 1) * blockSize, h); y++)
    for (int x = bx * blockSize; x < std::min((bx + 1) * blockSize, w); x++)
    {
      const int delta = int(uint8_t(lumaA[y * w + x])) - int(uint8_t(lumaB[y * w + x]));
      sse += double(delta) * delta;
    }
  return sse;
}

/* An "original" that differs from the item in a way that varies across the picture, so a block that
 * read its neighbour's samples - or the wrong frame - cannot pass by accident.
 */
bool writePerturbedCopy(const QString &sourcePath, const QString &targetPath, int w, int h)
{
  QFile source(sourcePath);
  QFile target(targetPath);
  if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  auto data = source.readAll();

  const qint64 bytesPerFrame = qint64(w) * h * 3 / 2;
  const auto   nrFrames      = data.size() / bytesPerFrame;
  for (qint64 frameIdx = 0; frameIdx < nrFrames; frameIdx++)
    for (qint64 y = 0; y < h; y++)
      for (qint64 x = 0; x < w; x++)
      {
        // Only the luma plane: chroma is not part of these statistics.
        const auto offset = frameIdx * bytesPerFrame + y * w + x;
        const auto value  = int(uint8_t(data[offset]));
        const auto delta  = int((x / 7 + y / 5 + frameIdx) % 11) - 5;
        data[offset]      = char(uint8_t(std::clamp(value + delta, 0, 255)));
      }
  return target.write(data) == data.size();
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
    std::cerr << "usage: 18-org-yuv-block-sse <yuv file>" << std::endl;
    return 2;
  }
  const QString path = argv[1];
  const int     w = 176, h = 144, blockSize = 64, frameIdx = 3;
  const int     blocksPerRow = (w + blockSize - 1) / blockSize;
  const int     blocksPerCol = (h + blockSize - 1) / blockSize;

  const QString orgPath = QDir::tempPath() + "/bd-org-yuv-sse.yuv";
  if (!writePerturbedCopy(path, orgPath, w, h))
  {
    std::cerr << "cannot write " << orgPath.toStdString() << std::endl;
    return 2;
  }

  const auto itemLuma = readLumaPlane(path, w, h, frameIdx);
  const auto orgLuma  = readLumaPlane(orgPath, w, h, frameIdx);
  if (itemLuma.size() != qint64(w) * h || orgLuma.size() != qint64(w) * h)
  {
    std::cerr << "the input does not have " << frameIdx + 1 << " frames of " << w << "x" << h
              << std::endl;
    return 2;
  }

  // --- no original attached ---------------------------------------------------------------------
  {
    Probe item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    check(item.getOriginalYUVSource().isEmpty(), "a fresh item has no original attached");
    check(computeFrame(item, frameIdx), "statistics became available without an original");
    const auto got = item.getPixelBlockStats(QPoint(0, 0), frameIdx);
    check(got && got->sse < 0.0, "the SSE is reported as unavailable, not as zero");
  }

  // --- with an original ------------------------------------------------------------------------
  {
    Probe   item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    QString error;
    check(item.setOriginalYUVSource(orgPath, &error), "the original YUV is accepted: " + error.toStdString());
    check(item.getOriginalYUVSource() == orgPath, "the item reports the attached original");
    check(computeFrame(item, frameIdx), "statistics became available with an original");

    int mismatches = 0;
    for (int by = 0; by < blocksPerCol; by++)
      for (int bx = 0; bx < blocksPerRow; bx++)
      {
        const auto got = item.getPixelBlockStats(QPoint(bx * blockSize, by * blockSize), frameIdx);
        const auto ref = referenceSSE(itemLuma, orgLuma, w, h, bx, by, blockSize);
        if (!got || got->sse != ref)
        {
          std::cout << "    block (" << bx << "," << by << ") got sse "
                    << (got ? got->sse : -1.0) << " / ref " << ref << std::endl;
          ++mismatches;
        }
      }
    check(mismatches == 0, "every 64x64 block's SSE matches an independent computation");

    // The perturbation touches every block, so a zero anywhere means nothing was compared.
    const auto first = item.getPixelBlockStats(QPoint(0, 0), frameIdx);
    check(first && first->sse > 0.0, "the SSE is not trivially zero");
  }

  // --- the SSE comes back from the disk cache ---------------------------------------------------
  {
    Probe   item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    QString error;
    // Attach before the cache is opened, so the file written above is the one that gets read.
    check(item.setOriginalYUVSource(orgPath, &error), "the original is attached again");
    item.loadFrame(frameIdx, false, true, false);
    item.requestPixelStatistics(frameIdx);
    const auto got = item.getPixelBlockStats(QPoint(0, 0), frameIdx);
    check(got.has_value(), "a new item reads the statistics back from the cache file immediately");
    const auto ref = referenceSSE(itemLuma, orgLuma, w, h, 0, 0, blockSize);
    check(got && got->sse == ref, "the SSE survives the round trip through the cache file");
  }

  // --- detaching goes back to "unavailable" -----------------------------------------------------
  {
    Probe   item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    QString error;
    check(item.setOriginalYUVSource(orgPath, &error), "the original is attached once more");
    check(computeFrame(item, frameIdx), "statistics are available with the original attached");

    check(item.setOriginalYUVSource(QString(), &error), "the original can be detached");
    check(item.getOriginalYUVSource().isEmpty(), "the item reports no original after detaching");
    check(computeFrame(item, frameIdx), "statistics are recomputed after detaching");
    const auto detached = item.getPixelBlockStats(QPoint(0, 0), frameIdx);
    check(detached && detached->sse < 0.0, "the SSE is unavailable again after detaching");
  }

  // --- a file that cannot be used --------------------------------------------------------------
  {
    const QString shortPath = QDir::tempPath() + "/bd-org-yuv-short.yuv";
    QFile         shortFile(shortPath);
    shortFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    shortFile.write(QByteArray(1024, '\x80'));
    shortFile.close();

    Probe   item(path, QSize(w, h), "YUV 4:2:0 8-bit");
    QString error;
    check(!item.setOriginalYUVSource(shortPath, &error), "a file shorter than one frame is refused");
    check(!error.isEmpty(), "the refusal comes with a message: " + error.toStdString());
    check(item.getOriginalYUVSource().isEmpty(), "nothing is attached after a refusal");

    error.clear();
    check(!item.setOriginalYUVSource(QDir::tempPath() + "/bd-does-not-exist.yuv", &error),
          "a missing file is refused");
    check(!error.isEmpty(), "the missing file comes with a message");

    QFile::remove(shortPath);
  }

  QFile::remove(orgPath);
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
