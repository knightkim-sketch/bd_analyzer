// Unit test for the assistant context serialization. No Qt, no library - see
// src/assist/AssistContext.h.
//
// What is worth pinning here is not formatting for its own sake. The panel shows this text to the
// user as "what was sent", and the model answers from it, so the cases that matter are the ones
// where an absent number means something specific: a lossless superblock, a stream with no
// original attached, and a sequence header that has not been parsed. Collapsing those three into
// a blank produces a confident wrong answer, which is the failure this test exists to catch.
#include <cmath>
#include <iostream>
#include <string>

#include "assist/AssistContext.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

void checkContains(const std::string &haystack, const std::string &needle, const std::string &what)
{
  const bool ok = haystack.find(needle) != std::string::npos;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (missing \"" << needle << "\")";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

void checkAbsent(const std::string &haystack, const std::string &needle, const std::string &what)
{
  const bool ok = haystack.find(needle) == std::string::npos;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (unexpectedly present: \"" << needle << "\")";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

using namespace bda::assist;

StreamInfo sampleStream(const std::string &original)
{
  StreamInfo stream;
  stream.path           = "/data/work/fast_q30.ivf";
  stream.width          = 176;
  stream.height         = 144;
  stream.superblockSize = 64;
  stream.codec          = "AV1";
  stream.decoder        = "dav1d";
  stream.originalPath   = original;
  return stream;
}

} // namespace

int main()
{
  std::cout << "assist context" << std::endl;

  // --- nothing open -----------------------------------------------------------------------
  {
    const AssistContext empty;
    check(empty.empty(), "a default context is empty");
    checkContains(renderContext(empty), "Nothing is open", "empty context says so plainly");
  }

  // --- PSNR from SSE ----------------------------------------------------------------------
  {
    /* Chosen so the answer is exact: MSE of 1 over the samples puts PSNR at 20*log10(255), and
     * anything that fumbles the sample count moves it visibly.
     */
    RegionStats stats;
    stats.sse         = 4096.0;
    stats.sampleCount = 4096;
    const auto psnr   = stats.psnr();
    check(psnr.has_value(), "PSNR exists when SSE and sample count do");
    check(psnr && std::abs(*psnr - 48.1308) < 1e-3, "PSNR matches 10*log10(255^2) for MSE 1");
    check(!stats.lossless(), "a nonzero SSE is not lossless");
  }
  {
    RegionStats stats;
    stats.sse         = 0.0;
    stats.sampleCount = 4096;
    check(!stats.psnr().has_value(), "lossless has no finite PSNR");
    check(stats.lossless(), "zero SSE reports lossless");
  }
  {
    RegionStats stats;
    stats.sse         = 100.0;
    stats.sampleCount = 0;
    check(!stats.psnr().has_value(), "a zero sample count yields no PSNR rather than a divide");
  }

  // --- the three absences read differently ------------------------------------------------
  {
    AssistContext context;
    context.stream          = sampleStream("/data/work/src.y4m");
    SuperblockInfo lossless;
    lossless.column      = 1;
    lossless.row         = 1;
    lossless.stats.bits  = 0.0;
    lossless.stats.sse   = 0.0;
    lossless.stats.sampleCount = 4096;
    context.superblock   = lossless;

    const auto text = renderContext(context);
    checkContains(text, "lossless", "a lossless superblock says lossless");
    checkAbsent(text, "no original attached", "and does not blame a missing original");
  }
  {
    AssistContext context;
    context.stream = sampleStream(""); // nothing attached
    SuperblockInfo sb;
    sb.stats.bits = 2276.0;
    context.superblock = sb;

    const auto text = renderContext(context);
    checkContains(text, "no original attached", "no original is named as the reason");
    checkContains(text, "original: none attached", "and the stream section says so too");
    checkAbsent(text, "lossless", "a missing original is not reported as lossless");
  }
  {
    AssistContext context;
    auto          stream      = sampleStream("/data/work/src.y4m");
    stream.superblockSize     = 0; // header not parsed
    context.stream            = stream;
    checkContains(renderContext(context),
                  "sequence header not parsed",
                  "an unknown superblock size names the cause, not just the absence");
  }

  // --- a full context ---------------------------------------------------------------------
  {
    AssistContext context;
    context.stream   = sampleStream("/data/work/src.y4m");
    context.frameIdx = 7;

    RegionStats frame;
    frame.bits         = 6435.0;
    frame.sse          = 51000.0;
    frame.sampleCount  = 176 * 144;
    context.frameStats = frame;

    SuperblockInfo sb;
    sb.column            = 1;
    sb.row               = 1;
    sb.pixelX            = 64;
    sb.pixelY            = 64;
    sb.stats.bits        = 2276.0;
    sb.stats.sse         = 900.0;
    sb.stats.sampleCount = 4096;
    sb.syntax            = {{"partition", "PARTITION_SPLIT"}, {"skip", "0"}};
    context.superblock   = sb;

    context.bdRate = {{"fast", "anchor"}, {"slow", "-3.21 %"}};

    const auto text = renderContext(context);
    check(!context.empty(), "a filled context is not empty");
    checkContains(text, "176x144", "resolution is rendered");
    checkContains(text, "64x64", "superblock size is rendered");
    checkContains(text, "index: 7", "frame index is rendered");
    checkContains(text, "bits 6435", "frame bits are rendered as an integer");
    checkContains(text, "column 1, row 1", "superblock grid position is rendered");
    checkContains(text, "(64, 64)", "superblock pixel position is rendered");
    checkContains(text, "partition: PARTITION_SPLIT", "block syntax is rendered");
    checkContains(text, "slow: -3.21 %", "BD-rate figures are rendered verbatim");
    checkContains(text, "decoder: dav1d", "the decoder is named");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
