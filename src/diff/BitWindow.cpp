#include "BitWindow.h"

#include <algorithm>
#include <map>

namespace bda::diff
{

bool readBitWindow(const std::uint8_t *data,
                   const std::size_t   size,
                   const std::uint64_t startBit,
                   const unsigned      count,
                   std::uint64_t &     out)
{
  if (data == nullptr || count == 0 || count > 64)
    return false;
  const auto endBit = startBit + count;
  if (endBit > std::uint64_t(size) * 8)
    return false;

  std::uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i)
  {
    const auto bit      = startBit + i;
    const auto byte     = data[bit / 8];
    const auto bitInTop = 7u - unsigned(bit % 8); // bit 0 is the most significant bit of byte 0
    value               = (value << 1) | std::uint64_t((byte >> bitInTop) & 1u);
  }
  out = value;
  return true;
}

std::vector<SuperblockBitDiff> compareBitWindows(const std::vector<SuperblockProbe> &probesA,
                                                 const std::uint8_t *                dataA,
                                                 const std::size_t                   sizeA,
                                                 const std::vector<SuperblockProbe> &probesB,
                                                 const std::uint8_t *                dataB,
                                                 const std::size_t                   sizeB,
                                                 const BitWindowOptions &            options)
{
  // Keyed by (y, x) so iteration comes out in raster order without a separate sort.
  std::map<std::pair<int, int>, SuperblockProbe> byPosA, byPosB;
  for (const auto &p : probesA)
    byPosA[{p.y, p.x}] = p;
  for (const auto &p : probesB)
    byPosB[{p.y, p.x}] = p;

  std::vector<std::pair<int, int>> positions;
  positions.reserve(byPosA.size() + byPosB.size());
  for (const auto &[key, _] : byPosA)
    positions.push_back(key);
  for (const auto &[key, _] : byPosB)
    if (byPosA.find(key) == byPosA.end())
      positions.push_back(key);
  std::sort(positions.begin(), positions.end());

  std::vector<SuperblockBitDiff> out;
  out.reserve(positions.size());
  for (const auto &key : positions)
  {
    SuperblockBitDiff diff;
    diff.y = key.first;
    diff.x = key.second;

    const auto a = byPosA.find(key);
    const auto b = byPosB.find(key);
    if (a == byPosA.end() || b == byPosB.end() || !a->second.hasBits || !b->second.hasBits)
    {
      diff.result = WindowCompare::Missing;
      out.push_back(diff);
      continue;
    }

    std::uint64_t valueA = 0, valueB = 0;
    const bool    okA = readBitWindow(dataA, sizeA, a->second.startBit, options.windowBits, valueA);
    const bool    okB = readBitWindow(dataB, sizeB, b->second.startBit, options.windowBits, valueB);
    if (!okA || !okB)
    {
      diff.result = WindowCompare::Unreadable;
      out.push_back(diff);
      continue;
    }

    diff.bitsA  = valueA;
    diff.bitsB  = valueB;
    diff.result = (valueA == valueB) ? WindowCompare::Equal : WindowCompare::Differ;
    out.push_back(diff);
  }
  return out;
}

const SuperblockBitDiff *firstDiffering(const std::vector<SuperblockBitDiff> &diffs)
{
  for (const auto &d : diffs)
    if (d.result == WindowCompare::Differ)
      return &d;
  return nullptr;
}

} // namespace bda::diff
