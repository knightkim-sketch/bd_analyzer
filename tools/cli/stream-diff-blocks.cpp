// Compare one decoded frame of two AV1 streams, block by block.
//
//   stream-diff-blocks <a> <b> --frame N [--sb 64|128] [--max-sb N] [--all]
//
// Step 4 of Find diff. Step 3 (stream-diff-payload) says which OBU payload first differs and at
// which byte; that byte is a position in the arithmetic-coded data and says nothing about where in
// the picture the divergence is. This decodes that one frame on both sides and reports the answer
// as coordinates: SB(row, col) and MI(row, col).
//
// Only the named frame is decoded. Decoding a 4K frame and querying every MI position costs
// seconds per stream, so sweeping a whole sequence here would be the wrong tool; step 3 has already
// narrowed it to one frame.
//
// Coordinate convention: SB and MI are (row, col). Pixel positions are (x, y).
#include <QApplication>
#include <QSettings>

#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>

#include "diff/BlockSyntaxDiff.h"
#include "integration/StreamDiffSteps.h"

namespace
{

constexpr int MiSize = 4; // The smallest AV1 block, and the unit MI positions are counted in.

void printBlock(const char *side, const bda::diff::BlockSyntax &b)
{
  if (!b.valid)
  {
    std::cout << "      " << side << ": no block" << std::endl;
    return;
  }
  std::cout << "      " << side << ": " << b.w << "x" << b.h << " at pixel (" << b.x << ", " << b.y
            << "), MI(" << (b.y / MiSize) << ", " << (b.x / MiSize) << ")" << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  std::string pathA, pathB;
  int         frameIdx = -1, sbSize = 0, maxSb = 5;
  int         tu = -1, obu = -1;
  bool        all = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--frame" && i + 1 < argc)
      frameIdx = std::stoi(argv[++i]);
    else if (a == "--tu" && i + 1 < argc)
      tu = std::stoi(argv[++i]);
    else if (a == "--obu" && i + 1 < argc)
      obu = std::stoi(argv[++i]);
    else if (a == "--sb" && i + 1 < argc)
      sbSize = std::stoi(argv[++i]);
    else if (a == "--max-sb" && i + 1 < argc)
      maxSb = std::stoi(argv[++i]);
    else if (a == "--all")
      all = true;
    else if (pathA.empty())
      pathA = a;
    else if (pathB.empty())
      pathB = a;
  }
  if (pathA.empty() || pathB.empty() || (frameIdx < 0 && (tu < 0 || obu < 0)))
  {
    std::cerr << "usage: stream-diff-blocks <a> <b> (--tu N --obu N | --frame N)\n"
                 "                          [--sb 64|128] [--max-sb N] [--all]\n"
                 "\n"
                 "  --tu N --obu N  the coded frame step 3 named; the display frame is worked out\n"
                 "  --frame N       a display frame index directly"
              << std::endl;
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerBlockDiff");
  QCoreApplication::setApplicationName("bdAnalyzerBlockDiff");
  QSettings().clear();

  /* --tu/--obu is the form that matches what step 3 reports. Resolving it here rather than asking
   * the reader to do it: the arithmetic looks easy (GOP start + order_hint) and is wrong whenever a
   * hidden frame is involved, which is exactly when this tool gets used.
   */
  if (frameIdx < 0)
  {
    std::string error;
    frameIdx = bda::integration::resolveDisplayIndex(QString::fromStdString(pathA), tu, obu,
                                                     error);
    if (frameIdx < 0)
    {
      std::cerr << "could not place TU " << tu << " / OBU " << obu << ": " << error << std::endl;
      return 2;
    }
    std::cout << "TU " << tu << " / OBU " << obu << " is displayed as frame " << frameIdx
              << std::endl;
  }

  /* Walk superblocks in raster order and stop at the first one whose blocks differ.
   *
   * Not a binary search over the superblock index. That needs "every superblock before the
   * boundary is identical and every one after it differs", and the second half is false: once the
   * arithmetic decoder has diverged most superblocks differ, but not all. Measured on this 4K pair
   * at frame 17, 86 of 2040 superblocks are identical while the first difference is at SB(0, 30) -
   * so 56 identical superblocks lie *after* the divergence. A probe landing on one of them reads
   * as "still identical here" and sends the search past the answer, silently.
   */
  bda::integration::BlockStepOptions options;
  options.sbSize   = sbSize;
  options.all      = all;
  options.progress = [](const std::string &text) { std::cerr << text << std::endl; };
  const auto step  = bda::integration::runBlockStep(QString::fromStdString(pathA),
                                                    QString::fromStdString(pathB), frameIdx,
                                                    options);
  if (!step.ok())
  {
    std::cerr << step.error << std::endl;
    _exit(2);
  }

  std::cout << "A  " << pathA << std::endl;
  std::cout << "B  " << pathB << std::endl;
  std::cout << "frame " << frameIdx << ", " << step.width << "x" << step.height
            << ", superblock " << step.sbSize << "x" << step.sbSize
            << (step.sbSizeGiven ? " (given)" : " (probed)") << std::endl;

  std::cout << std::fixed << std::setprecision(1) << "superblocks scanned: " << step.scannedSbs
            << " of " << step.sbTotal
            << (step.stoppedEarly ? " (stopped at the first difference)" : "") << std::endl;
  std::cout << "decoder queries: A " << step.queriesA << ", B " << step.queriesB
            << " (one per coding block, not one per MI position)" << std::endl;
  std::cout << "MI positions covered, of the whole frame: A " << step.coverageA << "%, B "
            << step.coverageB << "%"
            << (step.stoppedEarly ? " (only the scanned part was loaded)" : "") << std::endl;
  if (step.staleA > 0 || step.staleB > 0)
    std::cout << "  " << step.staleA << " / " << step.staleB
              << " queries answered for another frame and were dropped" << std::endl;
  if (!step.stoppedEarly && (step.coverageA < 95.0 || step.coverageB < 95.0))
    std::cout << "  WARNING coverage is low - differences below may be missing data rather than a "
                 "real divergence."
              << std::endl;

  const auto &differing = step.differing;
  if (differing.empty())
  {
    std::cout << "\nEvery block in frame " << frameIdx << " is identical ("
              << step.comparedPositions << " MI positions compared)." << std::endl;
    std::cout << "If step 3 pointed at this frame, the divergence is in syntax the decoder does "
                 "not export per block - check the frame header comparison."
              << std::endl;
    _exit(0);
  }

  const auto *firstBlock = step.firstWithBlockDiff();
  if (firstBlock != nullptr)
  {
    const auto &blk = firstBlock->blocks.front();
    std::cout << "\nfirst superblock with a differing block: SB(" << firstBlock->sbRow << ", "
              << firstBlock->sbCol << ")" << std::endl;
    std::cout << "  first differing block:                MI(" << blk.miRow << ", " << blk.miCol
              << ")  [" << bda::diff::blockDiffKindName(blk.kind) << "]" << std::endl;
  }
  else
    std::cout << "\nNo block syntax differs in the part of the frame that was scanned."
              << std::endl;

  /* The two answers are not always the same superblock, and the difference is the useful part: a
   * superblock whose bit count differs while every exported block value matches coded the same
   * decisions with a different residual - real, but not where the decision parted ways.
   */
  if (const auto *totalsOnly = step.firstTotalsOnlyBefore())
    std::cout << "  earliest superblock differing at all:  SB(" << totalsOnly->sbRow << ", "
              << totalsOnly->sbCol << ")  (superblock totals only, no block syntax differs "
                 "there)"
              << std::endl;

  std::cout << "\n" << differing.size() << " differing superblocks in the " << step.scannedSbs
            << " scanned, " << step.differingPositions << " of " << step.comparedPositions
            << " MI positions." << std::endl;
  if (step.stoppedEarly)
    std::cout << "Pass --all to scan the whole frame and count them all." << std::endl;

  const auto &result_sbs = differing;
  const auto limit = all ? result_sbs.size() : std::min<std::size_t>(result_sbs.size(), maxSb);
  for (std::size_t i = 0; i < limit; ++i)
  {
    const auto &sb = result_sbs[i];
    std::cout << "\nSB(" << sb.sbRow << ", " << sb.sbCol << ")  " << sb.blocks.size()
              << " differing blocks, " << sb.differingPositions << " MI positions" << std::endl;
    for (const auto &d : sb.superblock.diffs)
      std::cout << "  [superblock] " << d.name << ": A " << d.valueA << "  |  B " << d.valueB
                << std::endl;
    for (const auto &blk : sb.blocks)
    {
      std::cout << "  MI(" << blk.miRow << ", " << blk.miCol << ")  "
                << bda::diff::blockDiffKindName(blk.kind) << ", covers " << blk.positions
                << " MI positions" << std::endl;
      if (blk.kind != bda::diff::BlockDiffKind::SyntaxDiffers)
      {
        printBlock("A", blk.a);
        printBlock("B", blk.b);
      }
      for (const auto &d : blk.syntax.diffs)
      {
        std::cout << "      " << d.name << ": ";
        switch (d.kind)
        {
        case bda::diff::DiffKind::ValueMismatch:
          std::cout << "A " << d.valueA << "  |  B " << d.valueB;
          break;
        case bda::diff::DiffKind::OnlyInA: std::cout << "only in A (" << d.valueA << ")"; break;
        case bda::diff::DiffKind::OnlyInB: std::cout << "only in B (" << d.valueB << ")"; break;
        }
        std::cout << std::endl;
      }
    }
  }
  if (limit < result_sbs.size())
    std::cout << "\n... " << (result_sbs.size() - limit)
              << " more differing superblocks; pass --all to see them." << std::endl;

  std::cout.flush();
  // The decoder keeps threads and dlopened libraries alive; Qt teardown after that is not worth
  // the risk in a batch tool whose output is already written.
  _exit(0);
}
