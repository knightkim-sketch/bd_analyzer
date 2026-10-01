// Compare two decoded frames block by block, and say which superblock first disagrees.
//
// Step 4 of the Find diff pipeline. Step 3 narrows the divergence to one OBU payload and an exact
// byte offset inside it; that offset is a position in the arithmetic-coded data, which is not a
// position in the picture. This step decodes that one frame on both sides and compares what the
// decoder actually read, so the answer comes out as coordinates: SB(row, col) and MI(row, col).
//
// The picture is held as an MI grid - one entry per 4x4 position, each naming the coding block that
// covers it. Comparing per position rather than per block is what makes a partition difference
// legible: when A splits a 32x32 and B does not, there is no block in A to pair with B's, but every
// MI position still has exactly one block on each side.
//
// Qt-free; the caller fills the grid from whatever decoder it has.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "SyntaxDiff.h"

namespace bda::diff
{

/* The coding block covering one MI position, flattened to name/value pairs. */
struct BlockSyntax
{
  bool                       valid{};
  int                        x{}, y{}, w{}, h{}; //!< Coding block rect, in luma pixels.
  std::vector<SyntaxElement> elements;

  bool sameGeometry(const BlockSyntax &other) const
  {
    return this->x == other.x && this->y == other.y && this->w == other.w && this->h == other.h;
  }
};

/* One decoded frame as an MI grid.
 *
 * sbSizeMi is 16 for a 64x64 superblock and 32 for 128x128. It only groups the report; the
 * comparison itself is per MI position either way.
 */
struct BlockMap
{
  int                      miRows{}, miCols{};
  int                      sbSizeMi{16};
  std::vector<BlockSyntax> positions; //!< miRows * miCols, row-major.

  void resize(int rows, int cols)
  {
    this->miRows = rows;
    this->miCols = cols;
    this->positions.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), {});
  }
  BlockSyntax &at(const int miRow, const int miCol)
  {
    return this->positions[static_cast<std::size_t>(miRow) * this->miCols + miCol];
  }
  const BlockSyntax &at(const int miRow, const int miCol) const
  {
    return this->positions[static_cast<std::size_t>(miRow) * this->miCols + miCol];
  }
  int sbRows() const { return (this->miRows + this->sbSizeMi - 1) / this->sbSizeMi; }
  int sbCols() const { return (this->miCols + this->sbSizeMi - 1) / this->sbSizeMi; }
};

enum class BlockDiffKind
{
  MissingInA,      //!< No block covers the position in A, one does in B.
  MissingInB,
  GeometryDiffers, //!< Both have a block, with different rects - the partition parted ways.
  SyntaxDiffers,   //!< Same rect, different syntax.
};

const char *blockDiffKindName(BlockDiffKind kind);

/* One differing block pair, reported at the first MI position that showed it.
 *
 * A 32x32 block covers 64 MI positions and would otherwise be reported 64 times. The pair is
 * collapsed to its first position in raster order, with the count of positions it covers.
 */
struct BlockDiff
{
  int           miRow{}, miCol{};
  int           sbRow{}, sbCol{};
  BlockDiffKind kind{};
  BlockSyntax   a, b;
  SyntaxDiffResult syntax;    //!< Element-level differences; empty unless both blocks exist.
  std::size_t      positions{}; //!< MI positions inside this SB that reported this same pair.
};

struct SbDiff
{
  int                    sbRow{}, sbCol{};
  std::vector<BlockDiff> blocks;             //!< Raster order inside the superblock.
  std::size_t            differingPositions{};
  /* Values that describe the whole superblock rather than any block in it - see
   * BlockDiffOptions::superblockElements. Compared once here instead of once per block.
   */
  SyntaxDiffResult       superblock;

  bool hasBlockDiff() const { return !this->blocks.empty(); }
  bool differs() const { return this->hasBlockDiff() || !this->superblock.identical(); }
};

struct BlockDiffResult
{
  std::vector<SbDiff> sbs; //!< Differing superblocks, raster order.
  std::size_t         comparedPositions{};
  std::size_t         differingPositions{};
  std::string         error; //!< Set when the two grids cannot be compared at all.

  bool          ok() const { return this->error.empty(); }
  bool          identical() const { return this->sbs.empty(); }
  const SbDiff *firstSb() const { return this->sbs.empty() ? nullptr : &this->sbs.front(); }

  /* The first superblock in which a block really differs.
   *
   * Not the same as firstSb(): a superblock whose bit count differs while every exported block
   * value matches coded the same decisions with a different residual. That is worth reporting, but
   * it is not the block that parted ways, and answering with it would send the reader to the wrong
   * place in the picture.
   */
  const SbDiff *firstSbWithBlockDiff() const
  {
    for (const auto &sb : this->sbs)
      if (sb.hasBlockDiff())
        return &sb;
    return nullptr;
  }
};

struct BlockDiffOptions
{
  SyntaxDiffOptions syntax;
  /* Names the decoder attaches to every block of a superblock although they describe the whole
   * superblock - dav1d reports sb_bitcount and sb_qindex that way.
   *
   * Left per block they swamp the report: one differing block makes its superblock's total differ,
   * and then every one of the superblock's blocks is reported as differing too. The caller names
   * them because the core is kept free of any decoder's vocabulary.
   */
  std::vector<std::string> superblockElements;
};

/* Compare two MI grids.
 *
 * Superblocks are visited in raster order, and MI positions inside a superblock in raster order
 * too. Raster, not the bitstream's z-scan: the coding order inside a superblock follows the
 * partition tree, which the decoder does not export, and guessing it with a Morton code is wrong
 * for a vertical split. So "first" here means topmost-then-leftmost, and every differing block in
 * the first differing superblock is reported rather than just one.
 */
BlockDiffResult compareBlockMaps(const BlockMap &        a,
                                 const BlockMap &        b,
                                 const BlockDiffOptions &options = {});

/* Compare one superblock.
 *
 * Exists so a caller can walk superblocks in raster order and stop at the first one that differs,
 * without building both whole frames first. Finding the *first* difference needs every superblock
 * before it compared - that is what makes it first - so a left-to-right scan that stops on the
 * first hit is the least work an exact answer can cost. Measured on a 4K frame whose first
 * differing superblock is SB(0, 30): 31 superblocks instead of 2040.
 *
 * Both grids must already hold this superblock's positions. Positions outside it are not read.
 */
SbDiff compareSuperblock(const BlockMap &        a,
                         const BlockMap &        b,
                         int                     sbRow,
                         int                     sbCol,
                         const BlockDiffOptions &options,
                         std::size_t *           comparedPositions = nullptr);

} // namespace bda::diff
