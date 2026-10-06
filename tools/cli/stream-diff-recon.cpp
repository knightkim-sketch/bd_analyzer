// Compare the reconstructed pictures of two AV1 streams, frame by frame.
//
//   stream-diff-recon <a> <b> [--first] [--max N]
//
// Layer D of Find diff. Steps 3 and 4 say where the bitstreams first part company; this says where
// the pictures do, and the two need not agree. A divergence inside a hidden alternate reference
// stays invisible until a displayed frame predicts from it, and a residual coded differently can
// reconstruct to the same samples.
//
// Decodes both streams in display order. --first stops at the first frame that differs, which is
// the cheap question; without it every frame is compared, which on a long 4K sequence is minutes.
#include <QApplication>
#include <QSettings>

#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/StreamDiffSteps.h"

namespace
{
void printFrame(const bda::integration::ReconFrame &f, const bda::diff::FrameLayout &layout)
{
  printf("  frame %4d ", f.frameIdx);
  for (std::size_t p = 0; p < f.diff.planes.size(); ++p)
  {
    const auto &plane = f.diff.planes[p];
    printf("  %s %-9s", layout.planes[p].name.c_str(),
           bda::integration::formatPlanePsnr(plane, layout.bitDepth).c_str());
  }
  const auto &luma = f.diff.planes.front();
  if (!luma.identical())
    printf("  Y SSE %llu, %llu samples differ, first at pixel (%d, %d)",
           static_cast<unsigned long long>(luma.sse),
           static_cast<unsigned long long>(luma.differingSamples), *luma.firstX, *luma.firstY);
  printf("\n");
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  std::string pathA, pathB;
  bool        first = false;
  int         maxShown = 20;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--first")
      first = true;
    else if (a == "--max" && i + 1 < argc)
      maxShown = std::stoi(argv[++i]);
    else if (pathA.empty())
      pathA = a;
    else if (pathB.empty())
      pathB = a;
  }
  if (pathA.empty() || pathB.empty())
  {
    std::cerr << "usage: stream-diff-recon <a> <b> [--first] [--max N]" << std::endl;
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerReconDiff");
  QCoreApplication::setApplicationName("bdAnalyzerReconDiff");
  QSettings().clear();

  bda::integration::ReconStepOptions options;
  options.stopAtFirst = first;
  // Only to a terminal: piped into a log, a carriage return per frame is just noise.
  if (isatty(STDERR_FILENO))
    options.progress = [](const std::string &text) { std::cerr << "\r" << text << std::flush; };
  const auto step     = bda::integration::runReconStep(QString::fromStdString(pathA),
                                                      QString::fromStdString(pathB), options);
  std::cerr << std::endl;
  if (!step.ok())
  {
    std::cerr << step.error << std::endl;
    _exit(2);
  }

  printf("A  %s\nB  %s\n", pathA.c_str(), pathB.c_str());
  printf("%d display frames, %u-bit, %d planes; compared %zu%s\n", step.frameCount,
         step.layout.bitDepth, int(step.layout.planes.size()), step.frames.size(),
         first ? " (stopping at the first difference)" : "");

  const auto firstIdx = step.firstDiffering();
  if (firstIdx < 0)
  {
    printf("\nevery compared frame reconstructs identically\n");
    fflush(stdout); // _exit() skips the stdio flush
    _exit(0);
  }

  std::size_t differing = 0;
  for (const auto &f : step.frames)
    if (!f.diff.identical())
      ++differing;

  const auto &f = step.frames[firstIdx];
  printf("\nfirst frame whose pictures differ: %d\n", f.frameIdx);
  printFrame(f, step.layout);
  if (!first)
    printf("\n%zu of %zu frames differ, showing the first %d:\n", differing, step.frames.size(),
           maxShown);

  int shown = 0;
  if (!first)
    for (const auto &frame : step.frames)
      if (!frame.diff.identical() && shown++ < maxShown)
        printFrame(frame, step.layout);

  fflush(stdout);
  // Same reason as the other stream-diff tools: the decoder keeps threads and dlopened libraries
  // alive, and Qt teardown after that is not worth the risk once the output is written.
  _exit(0);
}
