// Regression: the path from raw YUV frames to a frame of ME results.
//
// This is what the ME panel calls on a worker thread, so it is checked here as a whole:
//
//   * the frame range rules, including the signed interval reaching forwards as well as back;
//   * the 8-bit conversion, which shifts a deeper source down rather than rescaling it - that is
//     what odyssey's open-loop ME does and what SVT's kernels need;
//   * failures come back with a reason instead of an empty result, because an empty overlay leaves
//     the user guessing;
//   * cancellation reaches through the runner into the estimator.
#include <QApplication>
#include <QByteArray>
#include <QSettings>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "integration/MeFrameSource.h"
#include "integration/MeRunner.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
void checkEq(long long got, long long want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (got " << got << ", want " << want << ")";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

constexpr unsigned kW = 128;
constexpr unsigned kH = 128;

std::uint8_t texel(int x, int y)
{
  const double v = 100.0 + 50.0 * std::sin(x / 9.0) * std::sin(y / 11.0) +
                   20.0 * std::sin(x / 3.0 + y / 5.0) +
                   (static_cast<double>(x) * x + static_cast<double>(y) * y) / 512.0;
  const int clamped = static_cast<int>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
  return static_cast<std::uint8_t>(clamped);
}

// A whole 4:2:0 8-bit frame: luma plane then two chroma planes, which the reader must skip past.
QByteArray rawFrame420(int shiftX, int shiftY)
{
  QByteArray data;
  data.resize(static_cast<int>(kW * kH + 2 * (kW / 2) * (kH / 2)));
  auto *p = reinterpret_cast<std::uint8_t *>(data.data());
  for (unsigned y = 0; y < kH; ++y)
    for (unsigned x = 0; x < kW; ++x)
      p[y * kW + x] = texel(static_cast<int>(x) - shiftX, static_cast<int>(y) - shiftY);
  // Chroma left as whatever resize gave us - nothing here reads it, and that is the point.
  return data;
}

// The same luma, 10 bit little endian, so the shift-down path gets exercised.
QByteArray rawFrame420Ten(int shiftX, int shiftY)
{
  QByteArray data;
  data.resize(static_cast<int>(2 * (kW * kH + 2 * (kW / 2) * (kH / 2))));
  auto *p = reinterpret_cast<std::uint8_t *>(data.data());
  for (unsigned y = 0; y < kH; ++y)
    for (unsigned x = 0; x < kW; ++x)
    {
      // Put the 8-bit value in the top bits so that >> 2 brings it back exactly.
      const unsigned v   = static_cast<unsigned>(texel(static_cast<int>(x) - shiftX,
                                                     static_cast<int>(y) - shiftY))
                         << 2;
      const auto     idx = (y * kW + x) * 2;
      p[idx]             = static_cast<std::uint8_t>(v & 0xFF);
      p[idx + 1]         = static_cast<std::uint8_t>(v >> 8);
    }
  return data;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  using namespace bda;
  const Size                       size(kW, kH);
  const video::yuv::PixelFormatYUV fmt8("YUV 4:2:0 8-bit");
  const video::yuv::PixelFormatYUV fmt10("YUV 4:2:0 10-bit LE");

  // --- the frame range rules ------------------------------------------------------------------
  {
    std::string reason;
    check(integration::meFrameRangeOk(5, 1, 10, &reason), "a normal backward reference is fine");

    reason.clear();
    check(!integration::meFrameRangeOk(0, 1, 10, &reason), "frame 0 has no past reference");
    check(!reason.empty(), "with a reason: " + reason);

    // The interval is signed, so a negative one reaches forwards and runs off the other end.
    reason.clear();
    check(integration::meFrameRangeOk(0, -1, 10, &reason), "a negative interval works at frame 0");
    reason.clear();
    check(!integration::meFrameRangeOk(9, -1, 10, &reason), "and runs out at the last frame");
    check(!reason.empty(), "with a reason: " + reason);

    reason.clear();
    check(!integration::meFrameRangeOk(5, 0, 10, &reason), "an interval of 0 is refused");
    check(!reason.empty(), "matching a frame against itself is pointless: " + reason);

    reason.clear();
    check(!integration::meFrameRangeOk(0, 1, 1, &reason), "a single-frame item is refused");

    checkEq(integration::meReferenceFrameIdx(5, 2), 3, "a positive interval reaches back");
    checkEq(integration::meReferenceFrameIdx(5, -2), 7, "a negative one reaches forward");
  }

  // --- 8 bit: the luma plane is found past no chroma, and the search works -------------------
  {
    integration::MeRunRequest req;
    req.currentFrame   = rawFrame420(0, 0);
    req.referenceFrame = rawFrame420(3, 2);
    req.format         = fmt8;
    req.frameSize      = size;
    req.frameIdx       = 1;
    req.refFrameIdx    = 0;
    req.params.algorithm    = me::Algorithm::SvtIntegerMe;
    req.params.frameInterval = 1;
    req.params.staticBypass = false;
    req.params.blockSizes   = me::BlockSizeSet();
    req.params.blockSizes.add(me::BlockSize::Blk64);

    const auto result = integration::runMe(req);
    check(result.ok(), "the run succeeds: " + result.error);
    checkEq(result.frameIdx, 1, "and reports the frame it was asked about");
    checkEq(result.refFrameIdx, 0, "and its reference");

    bool found = false;
    for (const auto &b : result.blocks)
      if (b.block.x == 0 && b.block.y == 0)
        found = b.mv == me::MotionVector::fromFullPel(3, 2) && b.commonSad == 0;
    check(found, "the displacement is recovered from the raw frames");
  }

  /* --- 10 bit is shifted down, not rescaled --------------------------------------------------
   *
   * The 10-bit frames hold the same picture with the samples moved up two bits, so shifting down
   * by two has to reproduce the 8-bit result exactly. Rescaling to 0..255 instead would change
   * every SAD - and would still "work", which is why this is worth pinning.
   */
  {
    integration::MeRunRequest ten;
    ten.currentFrame   = rawFrame420Ten(0, 0);
    ten.referenceFrame = rawFrame420Ten(3, 2);
    ten.format         = fmt10;
    ten.frameSize      = size;
    ten.params.algorithm     = me::Algorithm::SvtIntegerMe;
    ten.params.frameInterval = 1;
    ten.params.staticBypass  = false;
    ten.params.blockSizes    = me::BlockSizeSet();
    ten.params.blockSizes.add(me::BlockSize::Blk64);

    const auto result = integration::runMe(ten);
    if (!result.ok())
    {
      // A build where this pixel format name is not recognised would make the test vacuous.
      check(false, "the 10-bit run succeeds: " + result.error);
    }
    else
    {
      bool found = false;
      for (const auto &b : result.blocks)
        if (b.block.x == 0 && b.block.y == 0)
          found = b.mv == me::MotionVector::fromFullPel(3, 2) && b.commonSad == 0;
      check(found, "a 10-bit source gives the same answer after the shift down");
    }
  }

  // --- failures carry a reason -----------------------------------------------------------------
  {
    integration::MeRunRequest req;
    req.currentFrame   = rawFrame420(0, 0);
    req.referenceFrame = rawFrame420(3, 2);
    req.format         = fmt8;
    req.frameSize      = size;
    req.params.blockSizes = me::BlockSizeSet(); // nothing checked

    auto result = integration::runMe(req);
    check(!result.ok() && !result.error.empty(), "no block size selected: " + result.error);

    req.params.blockSizes.add(me::BlockSize::Blk64);
    req.params.algorithm = me::Algorithm::OdysseyClosedLoop;
    result               = integration::runMe(req);
    check(!result.ok() && !result.error.empty(),
          "the unimplemented algorithm says so: " + result.error);

    req.params.algorithm  = me::Algorithm::SvtIntegerMe;
    req.referenceFrame    = QByteArray(16, '\0'); // far too short for a frame
    result                = integration::runMe(req);
    check(!result.ok() && !result.error.empty(), "a truncated frame says so: " + result.error);
  }

  // --- cancellation reaches through the runner --------------------------------------------------
  {
    integration::MeRunRequest req;
    req.currentFrame   = rawFrame420(0, 0);
    req.referenceFrame = rawFrame420(3, 2);
    req.format         = fmt8;
    req.frameSize      = size;
    req.params.staticBypass = false;
    req.params.blockSizes   = me::BlockSizeSet();
    req.params.blockSizes.add(me::BlockSize::Blk64);

    me::CancelToken token;
    token.cancel();
    req.params.cancel = &token;

    const auto result = integration::runMe(req);
    check(result.cancelled, "a cancelled token stops the run");
    check(!result.ok(), "and the result is not usable");
    check(result.error.empty(), "cancellation is not reported as an error");
  }

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
