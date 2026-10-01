#include "BlockSyntaxDiff.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace bda::diff
{

const char *blockDiffKindName(const BlockDiffKind kind)
{
  switch (kind)
  {
  case BlockDiffKind::MissingInA:      return "no block in A";
  case BlockDiffKind::MissingInB:      return "no block in B";
  case BlockDiffKind::GeometryDiffers: return "partition differs";
  case BlockDiffKind::SyntaxDiffers:   return "syntax differs";
  }
  return "";
}

namespace
{

/* Identity of a reported pair, so one block is not reported once per MI position it covers.
 *
 * Both rects are part of the key: a block in A that pairs with two different blocks in B (because
 * B split where A did not) is two different findings, not one.
 */
using PairKey = std::tuple<int, int, int, int, int, int, int, int, int>;

PairKey pairKey(const BlockSyntax &a, const BlockSyntax &b, const BlockDiffKind kind)
{
  return {static_cast<int>(kind), a.x, a.y, a.w, a.h, b.x, b.y, b.w, b.h};
}

std::vector<SyntaxElement> select(const std::vector<SyntaxElement> &elements,
                                  const std::set<std::string> &     names,
                                  const bool                        keepNamed)
{
  std::vector<SyntaxElement> out;
  out.reserve(elements.size());
  for (const auto &e : elements)
    if ((names.count(e.name) > 0) == keepNamed)
      out.push_back(e);
  return out;
}

} // namespace

BlockDiffResult
compareBlockMaps(const BlockMap &a, const BlockMap &b, const BlockDiffOptions &options)
{
  BlockDiffResult result;
  if (a.miRows != b.miRows || a.miCols != b.miCols)
  {
    result.error = "the two frames are not the same size: A " + std::to_string(a.miRows) + "x" +
                   std::to_string(a.miCols) + " MI, B " + std::to_string(b.miRows) + "x" +
                   std::to_string(b.miCols) + " MI";
    return result;
  }
  if (a.sbSizeMi != b.sbSizeMi)
  {
    result.error = "the two streams use different superblock sizes";
    return result;
  }
  if (a.miRows <= 0 || a.miCols <= 0)
  {
    result.error = "empty frame";
    return result;
  }

  for (int sbRow = 0; sbRow < a.sbRows(); ++sbRow)
    for (int sbCol = 0; sbCol < a.sbCols(); ++sbCol)
    {
      std::size_t compared = 0;
      auto        sb       = compareSuperblock(a, b, sbRow, sbCol, options, &compared);
      result.comparedPositions += compared;
      result.differingPositions += sb.differingPositions;
      if (sb.differs())
        result.sbs.push_back(std::move(sb));
    }

  return result;
}

SbDiff compareSuperblock(const BlockMap &        a,
                         const BlockMap &        b,
                         const int               sbRow,
                         const int               sbCol,
                         const BlockDiffOptions &options,
                         std::size_t *           comparedPositions)
{
  const std::set<std::string> sbNames(options.superblockElements.begin(),
                                      options.superblockElements.end());
  const auto                  sbSize = a.sbSizeMi;

  SbDiff sb;
  sb.sbRow = sbRow;
  sb.sbCol = sbCol;

  // Where each pair already landed in sb.blocks, so repeats only bump its position count.
  std::map<PairKey, std::size_t> seen;
  /* The superblock's own values, taken from the first position that has them. They are the same
   * on every block of the superblock by construction, so one sample is the whole answer.
   */
  bool                       haveSbValues = false;
  std::vector<SyntaxElement> sbValuesA, sbValuesB;

  const auto miRowEnd = std::min(a.miRows, (sbRow + 1) * sbSize);
  const auto miColEnd = std::min(a.miCols, (sbCol + 1) * sbSize);
  for (int miRow = sbRow * sbSize; miRow < miRowEnd; ++miRow)
    for (int miCol = sbCol * sbSize; miCol < miColEnd; ++miCol)
    {
      const auto &pa = a.at(miRow, miCol);
      const auto &pb = b.at(miRow, miCol);
      if (!pa.valid && !pb.valid)
        continue; // Neither side coded anything here; nothing to say about it.
      if (comparedPositions != nullptr)
        ++*comparedPositions;

      if (!haveSbValues && !sbNames.empty() && pa.valid && pb.valid)
      {
        sbValuesA    = select(pa.elements, sbNames, true);
        sbValuesB    = select(pb.elements, sbNames, true);
        haveSbValues = true;
      }

      const auto blockA = sbNames.empty() ? pa.elements : select(pa.elements, sbNames, false);
      const auto blockB = sbNames.empty() ? pb.elements : select(pb.elements, sbNames, false);

      BlockDiff diff;
      diff.kind = BlockDiffKind::SyntaxDiffers;
      if (!pa.valid)
        diff.kind = BlockDiffKind::MissingInA;
      else if (!pb.valid)
        diff.kind = BlockDiffKind::MissingInB;
      else if (!pa.sameGeometry(pb))
        diff.kind = BlockDiffKind::GeometryDiffers;
      else
      {
        diff.syntax = compareSyntax(blockA, blockB, options.syntax);
        if (diff.syntax.identical())
          continue;
      }
      ++sb.differingPositions;

      const auto key = pairKey(pa, pb, diff.kind);
      if (const auto it = seen.find(key); it != seen.end())
      {
        ++sb.blocks[it->second].positions;
        continue;
      }

      diff.miRow = miRow;
      diff.miCol = miCol;
      diff.sbRow = sbRow;
      diff.sbCol = sbCol;
      diff.a     = pa;
      diff.b     = pb;
      /* A partition difference is still worth an element comparison: the two blocks are not the
       * same block, but seeing that one is INTRA and the other INTER says more than the rects.
       */
      if (pa.valid && pb.valid && diff.kind == BlockDiffKind::GeometryDiffers)
        diff.syntax = compareSyntax(blockA, blockB, options.syntax);
      diff.positions = 1;

      seen.emplace(key, sb.blocks.size());
      sb.blocks.push_back(std::move(diff));
    }

  if (haveSbValues)
    sb.superblock = compareSyntax(sbValuesA, sbValuesB, options.syntax);

  return sb;
}

} // namespace bda::diff
