// Regression: motion estimation on a compressed stream, over the original YUV attached for PSNR.
//
// The point of the feature is the comparison: the bitstream's own vectors and the reproduced ones
// on the same picture, in the same coordinates, so a disagreement is visible. That needs no
// decoding of a reference frame - the attached original is the source the stream was coded from,
// which is the picture both encoders' open-loop ME searches.
//
// What this pins down, all of it measured before it was written:
//
//   * The estimate lives in a container of its own, not the decoder's. Putting it in the decoder's
//     went wrong three ways: StatisticsData::needsLoading() reports LoadingNeeded for a rendered
//     type with no data this frame, so an unfinished estimate made the item re-decode the frame
//     forever (observed: LoadingNeeded survived five loadFrame() rounds); setFrameIndex() runs on
//     the decoder's loading thread while the result is written from the GUI thread, on an unguarded
//     std::map; and it clears the cache on every frame change, dropping the estimate (observed).
//   * A Y4M original is indexed, not multiplied into. Y4M carries a file header and a per-frame
//     marker, so a flat offset lands mid-frame - and produces a plausible wrong vector rather than
//     an error. Before the index went in, the same content as .yuv and as .y4m gave different
//     vectors; they must now agree exactly.
//   * A mismatched original is refused with a reason rather than compared against.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "integration/MeStatisticsAdapter.h"
#include "playlistitem/playlistItemCompressedVideo.h"
#include "statistics/StatisticsType.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

constexpr int kW = 176; // test.ivf
constexpr int kH = 144;

QByteArray lumaFrame(int n)
{
  QByteArray luma(kW * kH, '\0');
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
    {
      // Panning texture, so the estimate has something to find rather than a frame of zeros.
      const double v = 128.0 + 60.0 * std::sin((x + 3.0 * n) / 7.3) * std::cos((y + 2.0 * n) / 5.1) +
                       35.0 * std::sin((x + 3.0 * n + 2.0 * (y + 2.0 * n)) / 11.7);
      luma[y * kW + x] = char(static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : int(v))));
    }
  return luma;
}

// The same content twice: flat planar, and wrapped in Y4M. They must read back identically.
bool writeOriginals(const QString &flatPath, const QString &y4mPath, int frames)
{
  QFile flat(flatPath), y4m(y4mPath);
  if (!flat.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      !y4m.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;

  const QByteArray chroma(2 * (kW / 2) * (kH / 2), '\x80');
  y4m.write(QString("YUV4MPEG2 W%1 H%2 F25:1 Ip A1:1 C420jpeg\n").arg(kW).arg(kH).toLatin1());
  for (int n = 0; n < frames; ++n)
  {
    const auto luma = lumaFrame(n);
    flat.write(luma);
    flat.write(chroma);
    y4m.write("FRAME\n");
    y4m.write(luma);
    y4m.write(chroma);
  }
  return true;
}

class Probe : public playlistItemCompressedVideo
{
public:
  using playlistItemCompressedVideo::playlistItemCompressedVideo;
  decoder::DecoderEngine engine() const { return this->decoderEngine; }
  //!< The decoder's container - the one the loading state machine drives.
  stats::StatisticsData &decoderData() { return this->statisticsData; }
  void                   load(int f) { this->loadFrame(f, false, true, false); }
};

const char *stateName(ItemLoadingState s)
{
  return s == ItemLoadingState::LoadingNeeded ? "LoadingNeeded" : "LoadingNotNeeded";
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 29-me-on-compressed-stream <av1 file>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  const auto flatPath = QDir::tempPath() + "/bd-me-org-flat.yuv";
  const auto y4mPath  = QDir::tempPath() + "/bd-me-org.y4m";
  check(writeOriginals(flatPath, y4mPath, 12), "the two originals were written");

  Probe item(argv[1], 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);

  // --- without an original there is nothing to estimate from -----------------------------------
  check(!item.supportsMotionEstimation(),
        "a stream with no original attached cannot be estimated");
  check(item.getMotionEstimationData() != nullptr, "but it does own a container for one");
  check(item.getMotionEstimationData() != &item.decoderData(),
        "and it is NOT the decoder's - see the header for the three ways sharing it failed");

  // --- attaching the original ------------------------------------------------------------------
  QString error;
  check(item.setOriginalYUVSource(flatPath, &error), "the flat original attaches: " +
                                                         error.toStdString());
  check(item.supportsMotionEstimation(), "and that is the whole requirement");

  const auto flat3 = item.readRawFrame(3);
  const auto flat4 = item.readRawFrame(4);
  check(!flat3.isEmpty() && !flat4.isEmpty() && flat3 != flat4,
        "two different source frames come back");

  /* --- Y4M reads the same content --------------------------------------------------------------
   *
   * This is the check the whole Y4M index exists for. A flat offset into a Y4M lands inside the
   * previous frame, which reads as a picture and estimates to a plausible wrong vector - the
   * failure is silent, so it has to be caught by comparison rather than by an error.
   */
  check(item.setOriginalYUVSource(y4mPath, &error), "the Y4M original attaches: " +
                                                        error.toStdString());
  check(item.readRawFrame(3) == flat3 && item.readRawFrame(4) == flat4,
        "and gives byte for byte the same frames as the flat file");
  check(item.readRawFrame(11) == lumaFrame(11) + QByteArray(2 * (kW / 2) * (kH / 2), '\x80'),
        "including the last one, so the index is not drifting");
  check(item.readRawFrame(12).isEmpty(), "and a frame past the end is empty, not the last one again");

  // --- a mismatched original is refused --------------------------------------------------------
  {
    const auto wrongPath = QDir::tempPath() + "/bd-me-org-wrong.y4m";
    QFile      f(wrongPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      f.write("YUV4MPEG2 W320 H240 F25:1 C420jpeg\n");
      const QByteArray frame(320 * 240 * 3 / 2, '\x80');
      for (int n = 0; n < 4; ++n)
      {
        f.write("FRAME\n");
        f.write(frame);
      }
      f.close();

      error.clear();
      check(!item.setOriginalYUVSource(wrongPath, &error),
            "a Y4M of the wrong size is refused");
      check(!error.isEmpty(), "with a reason that names both sizes: " + error.toStdString());
      QFile::remove(wrongPath);
    }
  }

  // Back to a usable one for the rest.
  check(item.setOriginalYUVSource(y4mPath, &error), "the good original attaches again");

  /* --- the decoder's loading state machine must not see the ME types --------------------------
   *
   * needsLoading() reports LoadingNeeded for any rendered type with no data this frame, and the
   * item re-decodes to satisfy it. An estimate is filled from outside and is not there yet while it
   * runs, so a rendered ME type in that container asks for a decode that can never satisfy it.
   */
  if (item.engine() == decoder::DecoderEngine::Dav1d)
  {
    const int frameIdx = 4;
    item.setBlockInfoRequested(true);
    item.load(frameIdx);
    const auto before = item.decoderData().needsLoading(frameIdx);
    check(before == ItemLoadingState::LoadingNotNeeded,
          std::string("baseline: the decoder is satisfied (") + stateName(before) + ")");

    bda::me::BlockSizeSet sizes;
    sizes.add(bda::me::BlockSize::Blk64);
    bda::integration::syncMeStatTypes(*item.getMotionEstimationData(),
                                      bda::me::Algorithm::SvtIntegerMe,
                                      sizes);
    const int vecId = bda::integration::meVectorTypeId(bda::me::Algorithm::SvtIntegerMe,
                                                       bda::me::BlockSize::Blk64);
    bool      registered = false;
    for (const auto &t : item.getMotionEstimationData()->getStatisticsTypes())
      if (t.typeID == vecId)
        registered = t.render;
    check(registered, "the ME vector type is registered and rendered");

    const auto after = item.decoderData().needsLoading(frameIdx);
    check(after == ItemLoadingState::LoadingNotNeeded,
          std::string("and the decoder is still satisfied (") + stateName(after) +
              ") - no decode loop");

    bool inDecoderContainer = false;
    for (const auto &t : item.decoderData().getStatisticsTypes())
      if (t.typeID == vecId)
        inDecoderContainer = true;
    check(!inDecoderContainer, "the ME type is not in the decoder's container at all");

    /* And the decoder moving on must not take the estimate with it: the two containers keep their
     * own frame index, which is what made the overlay vanish when they shared one.
     */
    item.getMotionEstimationData()->setFrameIndex(frameIdx);
    item.getMotionEstimationData()->at(vecId).addBlockVector(0, 0, 64, 64, 8, 16);
    item.load(frameIdx + 1);
    check(item.getMotionEstimationData()->hasDataForTypeID(vecId),
          "decoding the next frame leaves the estimate alone");

    // The click query lists the stream's vectors and the reproduced one together.
    item.getMotionEstimationData()->setFrameIndex(frameIdx + 1);
    item.getMotionEstimationData()->at(vecId).addBlockVector(0, 0, 64, 64, 8, 16);
    const auto info = item.getBlockInfoAt(QPoint(10, 10), frameIdx + 1);
    check(info.isValid, "clicking a block on the stream still answers");
    bool hasStream = false, hasMe = false;
    for (const auto &e : info.entries)
    {
      if (e.typeID < bda::integration::kMeStatTypeBase)
        hasStream = true;
      if (e.typeID == vecId)
        hasMe = true;
    }
    check(hasStream, "with the bitstream's own syntax");
    check(hasMe, "and the reproduced vector in the same pane");
  }
  else
    std::cout << "  (dav1d not available - the loading-state checks need its statistics)"
              << std::endl;

  QFile::remove(flatPath);
  QFile::remove(y4mPath);
  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
