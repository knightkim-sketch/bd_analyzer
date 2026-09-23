// Unit test: reading a bit window out of a buffer, and comparing two streams at superblock starts.
//
// The reader is the part worth testing. Bit 0 is the most significant bit of byte 0 - the order the
// decoder counts in - and getting that backwards still produces plausible-looking numbers, so the
// expected values here are written out by hand from the bit pattern rather than computed.
#include "diff/BitWindow.h"

#include <iostream>
#include <string>

using namespace bda::diff;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
  if (!ok)
    ++failures;
}

template <typename T> void checkEqual(const T &got, const T &want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok   " : "  FAIL ") << what << ": " << got;
  if (!ok)
    std::cout << " (expected " << want << ")";
  std::cout << "\n";
  if (!ok)
    ++failures;
}
} // namespace

int main()
{
  // 1010'1100  0011'0101  1111'0000
  const std::uint8_t data[] = {0xAC, 0x35, 0xF0};
  const auto         size   = sizeof(data);

  {
    std::uint64_t v = 0;
    check(readBitWindow(data, size, 0, 8, v), "read the first byte");
    checkEqual(v, std::uint64_t(0xAC), "bit 0 is the top bit of byte 0");
  }
  {
    std::uint64_t v = 0;
    check(readBitWindow(data, size, 0, 1, v), "read one bit");
    checkEqual(v, std::uint64_t(1), "the top bit of 0xAC is 1");
  }
  {
    std::uint64_t v = 0;
    check(readBitWindow(data, size, 1, 3, v), "read across no boundary");
    checkEqual(v, std::uint64_t(0b010), "bits 1..3 of 1010'1100");
  }
  {
    // Straddle the byte boundary: last four bits of 0xAC, first four of 0x35.
    std::uint64_t v = 0;
    check(readBitWindow(data, size, 4, 8, v), "read across a byte boundary");
    checkEqual(v, std::uint64_t(0b11000011), "1100 then 0011");
  }
  {
    std::uint64_t v = 0;
    check(readBitWindow(data, size, 0, 24, v), "read the whole buffer");
    checkEqual(v, std::uint64_t(0xAC35F0), "all three bytes in order");
  }
  {
    std::uint64_t v = 123;
    check(!readBitWindow(data, size, 1, 24, v), "a window past the end is refused");
    checkEqual(v, std::uint64_t(123), "and leaves the output untouched");
    check(!readBitWindow(data, size, 0, 0, v), "zero width is refused");
    check(!readBitWindow(data, size, 0, 65, v), "more than 64 bits is refused");
    check(!readBitWindow(nullptr, 0, 0, 8, v), "a null buffer is refused");
  }

  // ------------------------------------------------------------------ superblock comparison
  const std::uint8_t other[] = {0xAC, 0x35, 0xF1}; // same but for the very last bit
  {
    const std::vector<SuperblockProbe> a{{0, 0, 0, true}, {64, 0, 8, true}};
    const auto d = compareBitWindows(a, data, size, a, data, size, {16});
    checkEqual(d.size(), std::size_t(2), "one result per superblock");
    check(d[0].result == WindowCompare::Equal, "identical buffers compare equal");
    check(firstDiffering(d) == nullptr, "and report no first difference");
  }
  {
    const std::vector<SuperblockProbe> a{{0, 0, 0, true}, {64, 0, 8, true}};
    const auto d = compareBitWindows(a, data, size, a, other, sizeof(other), {24});
    check(d[0].result == WindowCompare::Differ, "a changed last bit is seen at offset 0");
    const auto *first = firstDiffering(d);
    check(first != nullptr, "a first difference is found");
    checkEqual(first->x, 0, "and it is the superblock at x=0");
  }
  {
    // Raster order: (y=0,x=64) must come before (y=64,x=0) regardless of input order.
    const std::vector<SuperblockProbe> a{{0, 64, 0, true}, {64, 0, 0, true}, {0, 0, 0, true}};
    const auto d = compareBitWindows(a, data, size, a, other, sizeof(other), {24});
    checkEqual(d.size(), std::size_t(3), "all three superblocks");
    check(d[0].y == 0 && d[0].x == 0, "raster order starts at the origin");
    check(d[1].y == 0 && d[1].x == 64, "then the next column");
    check(d[2].y == 64, "then the next row");
  }
  {
    // A superblock only one side reports is a gap, not a divergence.
    const std::vector<SuperblockProbe> a{{0, 0, 0, true}, {64, 0, 0, true}};
    const std::vector<SuperblockProbe> b{{0, 0, 0, true}};
    const auto d = compareBitWindows(a, data, size, b, data, size, {16});
    checkEqual(d.size(), std::size_t(2), "the unmatched superblock is still listed");
    check(d[1].result == WindowCompare::Missing, "and marked missing");
    check(firstDiffering(d) == nullptr, "missing is not a first difference");
  }
  {
    // A probe whose window runs past the buffer is unreadable, not different.
    const std::vector<SuperblockProbe> a{{0, 0, 20, true}};
    const auto d = compareBitWindows(a, data, size, a, other, sizeof(other), {24});
    check(d[0].result == WindowCompare::Unreadable, "a short read is unreadable");
    check(firstDiffering(d) == nullptr, "unreadable is not a first difference");
  }
  {
    const std::vector<SuperblockProbe> a{{0, 0, 0, false}};
    const auto d = compareBitWindows(a, data, size, a, data, size, {16});
    check(d[0].result == WindowCompare::Missing, "a probe without a bit range is missing");
  }

  std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
  return failures == 0 ? 0 : 1;
}
