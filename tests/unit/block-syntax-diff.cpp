// Step 4 of Find diff: comparing two decoded frames as MI grids.
//
// The grids here are written by hand rather than decoded, because the behaviour worth locking in is
// the bookkeeping around the comparison, not the decoder:
//
//   * A superblock-level value (dav1d's sb_bitcount) is attached to every block of the superblock.
//     Compared per block it turns one real finding into one finding per block of that superblock -
//     measured on a real stream, 4 findings became 4 findings each carrying a phantom line, and the
//     differing-position count went from 2594 to 8208.
//   * A block covering many MI positions must be reported once, not once per position.
//   * "The partition differs" and "a value differs" are different answers and must not be merged.
#include <iostream>
#include <string>

#include "diff/BlockSyntaxDiff.h"

namespace
{
int failures = 0;

void check(const bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++failures;
}

using namespace bda::diff;

/* Fill a rectangle of MI positions with one block, the way a decoder query would answer for every
 * pixel inside a coding block.
 */
void putBlock(BlockMap &                        map,
              const int                         miRow,
              const int                         miCol,
              const int                         miH,
              const int                         miW,
              const std::vector<SyntaxElement> &elements)
{
  for (int r = miRow; r < miRow + miH; ++r)
    for (int c = miCol; c < miCol + miW; ++c)
    {
      auto &e    = map.at(r, c);
      e.valid    = true;
      e.x        = miCol * 4;
      e.y        = miRow * 4;
      e.w        = miW * 4;
      e.h        = miH * 4;
      e.elements = elements;
    }
}

// One 64x64 superblock, split into four 32x32 blocks.
BlockMap fourBlockSb(const std::string &bitcount, const std::string &mv3)
{
  BlockMap map;
  map.resize(16, 16);
  map.sbSizeMi = 16;
  putBlock(map, 0, 0, 8, 8, {{"sb_bitcount", bitcount}, {"Motion Vector", "L0 (0,0)"}});
  putBlock(map, 0, 8, 8, 8, {{"sb_bitcount", bitcount}, {"Motion Vector", "L0 (1,1)"}});
  putBlock(map, 8, 0, 8, 8, {{"sb_bitcount", bitcount}, {"Motion Vector", "L0 (2,2)"}});
  putBlock(map, 8, 8, 8, 8, {{"sb_bitcount", bitcount}, {"Motion Vector", mv3}});
  return map;
}

BlockDiffOptions withSbElements()
{
  BlockDiffOptions o;
  o.superblockElements = {"sb_bitcount", "sb_qindex"};
  return o;
}
} // namespace

int main()
{
  {
    const auto a = fourBlockSb("100", "L0 (3,3)");
    const auto r = compareBlockMaps(a, a, withSbElements());
    check(r.ok(), "two identical frames compare without an error");
    check(r.identical(), "two identical frames report no difference");
    check(r.comparedPositions == 256, "every MI position of the superblock was compared");
  }

  {
    // One block differs. The superblock total differs with it, as it does in a real stream.
    const auto a = fourBlockSb("100", "L0 (3,3)");
    const auto b = fourBlockSb("104", "L0 (4,4)");
    const auto r = compareBlockMaps(a, b, withSbElements());

    check(r.sbs.size() == 1, "one superblock differs");
    const auto *sb = r.firstSb();
    check(sb != nullptr && sb->sbRow == 0 && sb->sbCol == 0, "it is SB(0, 0)");
    /* The whole point of splitting superblock values out: without it all four blocks carry the
     * differing sb_bitcount and the report names the first of them, which is not the block that
     * changed.
     */
    check(sb != nullptr && sb->blocks.size() == 1, "only the block that really differs is reported");
    check(sb != nullptr && sb->blocks.front().miRow == 8 && sb->blocks.front().miCol == 8,
          "it is reported at MI(8, 8)");
    check(sb != nullptr && sb->blocks.front().positions == 64,
          "the 32x32 block is reported once, covering 64 MI positions");
    check(sb != nullptr && sb->superblock.diffs.size() == 1,
          "the superblock total is reported once, at the superblock");
    check(r.differingPositions == 64, "only the differing block's positions count as differing");
  }

  {
    // Only the superblock total differs - same decisions, different residual.
    const auto a = fourBlockSb("100", "L0 (3,3)");
    const auto b = fourBlockSb("104", "L0 (3,3)");
    const auto r = compareBlockMaps(a, b, withSbElements());

    check(!r.identical(), "a superblock-only difference is still reported");
    check(r.firstSb() != nullptr, "it names a superblock");
    check(r.firstSbWithBlockDiff() == nullptr,
          "but no superblock is named as having a differing block");
    check(r.differingPositions == 0, "no MI position is counted as differing");
  }

  {
    // Without the option, the superblock total is treated as block syntax - the behaviour the
    // option exists to avoid. Locked in so the two paths cannot quietly become one.
    const auto a = fourBlockSb("100", "L0 (3,3)");
    const auto b = fourBlockSb("104", "L0 (4,4)");
    const auto r = compareBlockMaps(a, b, {});
    check(r.firstSb() != nullptr && r.firstSb()->blocks.size() == 4,
          "with no superblock elements named, all four blocks are flagged");
  }

  {
    // A splits where B does not.
    BlockMap a;
    a.resize(16, 16);
    a.sbSizeMi = 16;
    putBlock(a, 0, 0, 16, 16, {{"Pred Mode", "INTRA (0)"}});

    BlockMap b;
    b.resize(16, 16);
    b.sbSizeMi = 16;
    putBlock(b, 0, 0, 8, 16, {{"Pred Mode", "INTRA (0)"}});
    putBlock(b, 8, 0, 8, 16, {{"Pred Mode", "SINGLE_REF_FWD (1)"}});

    const auto r  = compareBlockMaps(a, b, withSbElements());
    const auto *sb = r.firstSb();
    check(sb != nullptr && sb->blocks.size() == 2,
          "a horizontal split against one block is two findings, one per pairing");
    if (sb != nullptr && sb->blocks.size() == 2)
    {
      check(sb->blocks[0].kind == BlockDiffKind::GeometryDiffers,
            "the first is reported as a partition difference, not a value difference");
      check(sb->blocks[0].miRow == 0 && sb->blocks[0].miCol == 0, "found at MI(0, 0)");
      check(sb->blocks[1].miRow == 8 && sb->blocks[1].miCol == 0, "and at MI(8, 0)");
      // Geometry differs, so the elements are compared too: that is how "A is INTRA where B is
      // inter" reaches the report at all.
      check(!sb->blocks[1].syntax.identical(),
            "a partition difference still reports the values that differ");
    }
  }

  {
    // B coded nothing where A did. Not a normal stream, which is exactly why it must be said.
    auto a = fourBlockSb("100", "L0 (3,3)");
    auto b = fourBlockSb("100", "L0 (3,3)");
    for (int r = 8; r < 16; ++r)
      for (int c = 8; c < 16; ++c)
        b.at(r, c) = {};

    const auto r   = compareBlockMaps(a, b, withSbElements());
    const auto *sb = r.firstSb();
    check(sb != nullptr && sb->blocks.size() == 1, "the missing region is one finding");
    check(sb != nullptr && sb->blocks.front().kind == BlockDiffKind::MissingInB,
          "reported as no block in B");
  }

  {
    /* compareSuperblock() must give exactly what compareBlockMaps() gives for that superblock.
     *
     * The CLI walks superblocks and stops at the first difference, so this is the entry point that
     * produces the reported answer; if the two ever disagree, the fast path would answer something
     * the full comparison does not.
     */
    BlockMap a, b;
    a.resize(32, 32);
    b.resize(32, 32);
    a.sbSizeMi = b.sbSizeMi = 16;
    for (int sbR = 0; sbR < 2; ++sbR)
      for (int sbC = 0; sbC < 2; ++sbC)
      {
        const bool differs = (sbR == 1 && sbC == 0);
        putBlock(a, sbR * 16, sbC * 16, 16, 16, {{"sb_bitcount", "10"}, {"skip", "0"}});
        putBlock(b, sbR * 16, sbC * 16, 16, 16,
                 {{"sb_bitcount", "10"}, {"skip", differs ? "1" : "0"}});
      }

    const auto whole = compareBlockMaps(a, b, withSbElements());
    check(whole.sbs.size() == 1, "the full comparison finds one differing superblock");

    std::size_t compared = 0;
    const auto  one      = compareSuperblock(a, b, 1, 0, withSbElements(), &compared);
    check(one.hasBlockDiff(), "comparing that superblock alone finds the same difference");
    check(one.blocks.size() == whole.sbs.front().blocks.size() &&
              one.blocks.front().miRow == whole.sbs.front().blocks.front().miRow &&
              one.blocks.front().miCol == whole.sbs.front().blocks.front().miCol,
          "and reports it at the same MI position");
    check(compared == 256, "counting only that superblock's positions");

    // Walking in raster order and stopping at the first hit must reach SB(1, 0) and no earlier one.
    check(!compareSuperblock(a, b, 0, 0, withSbElements()).differs(), "SB(0, 0) is clean");
    check(!compareSuperblock(a, b, 0, 1, withSbElements()).differs(), "SB(0, 1) is clean");
  }

  {
    BlockMap a, b;
    a.resize(16, 16);
    b.resize(16, 32);
    const auto r = compareBlockMaps(a, b, withSbElements());
    check(!r.ok(), "frames of different sizes are refused");
    check(r.identical(), "and report no differences rather than a partial comparison");
  }

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
  return failures == 0 ? 0 : 1;
}
