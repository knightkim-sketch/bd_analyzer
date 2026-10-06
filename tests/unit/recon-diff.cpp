// Unit test: comparing two reconstructed frames sample by sample (Find diff, layer D).
//
// The arithmetic is simple; what earns a test is the bookkeeping around it. A plane offset off by
// one plane compares U against V and still produces a plausible SSE, and a 16-bit sample read in
// the wrong byte order makes a difference of 1 look like a difference of 256. Both are checked
// with buffers whose expected answers were written down by hand.
#include "diff/ReconDiff.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

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

// 4x2 luma, 2x1 chroma per plane: a 4:2:0 frame small enough to write out by hand.
FrameLayout layout420(unsigned bitDepth)
{
  FrameLayout l;
  l.planes   = {{"Y", 4, 2}, {"U", 2, 1}, {"V", 2, 1}};
  l.bitDepth = bitDepth;
  return l;
}
} // namespace

int main()
{
  std::cout << "== layout ==\n";
  checkEqual(layout420(8).bytesPerFrame(), std::size_t(12), "8-bit 4x2 4:2:0 frame bytes");
  checkEqual(layout420(10).bytesPerFrame(), std::size_t(24), "10-bit frame is two bytes a sample");

  std::cout << "== identical ==\n";
  {
    const std::vector<std::uint8_t> a = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const auto d = compareFrames(a.data(), a.data(), layout420(8));
    check(d.identical(), "a frame against itself is identical");
    check(d.planes.size() == 3, "one result per plane");
    check(!psnr(d.planes[0], 8).has_value(), "and has no finite PSNR rather than infinity");
  }

  std::cout << "== 8-bit, one difference per plane ==\n";
  {
    //                               Y . . . . . . .   U  .   V  .
    std::vector<std::uint8_t> a = {10, 10, 10, 10, 10, 10, 10, 10, 50, 50, 90, 90};
    std::vector<std::uint8_t> b = a;
    b[6]  = 13; // Y (x 2, y 1): +3
    b[9]  = 48; // U (x 1, y 0): -2
    b[10] = 91; // V (x 0, y 0): +1
    const auto d = compareFrames(a.data(), b.data(), layout420(8));
    checkEqual(d.planes[0].sse, std::uint64_t(9), "Y SSE");
    checkEqual(d.planes[1].sse, std::uint64_t(4), "U SSE - the plane offset lands on U, not V");
    checkEqual(d.planes[2].sse, std::uint64_t(1), "V SSE");
    checkEqual(d.planes[0].samples, std::uint64_t(8), "Y sample count");
    checkEqual(d.planes[1].samples, std::uint64_t(2), "U sample count");
    checkEqual(*d.planes[0].firstX, 2, "first Y difference x");
    checkEqual(*d.planes[0].firstY, 1, "first Y difference y");
    checkEqual(*d.planes[1].firstX, 1, "first U difference x, in chroma samples");
    // MSE 9/8 at 8 bit: 10 log10(255^2 / 1.125)
    const auto p = psnr(d.planes[0], 8);
    check(p.has_value() && std::fabs(*p - 47.6193) < 1e-3, "Y PSNR " + std::to_string(*p));
  }

  std::cout << "== 10-bit little endian ==\n";
  {
    // Every sample 512 (0x0200), stored low byte first.
    std::vector<std::uint8_t> a;
    for (int i = 0; i < 12; ++i)
    {
      a.push_back(0x00);
      a.push_back(0x02);
    }
    auto b = a;
    b[2] = 0x01; // Y sample 1 becomes 0x0201: a difference of 1, not of 256
    const auto d = compareFrames(a.data(), b.data(), layout420(10));
    checkEqual(d.planes[0].sse, std::uint64_t(1), "a low byte change is a difference of 1");
    checkEqual(*d.planes[0].firstX, 1, "at sample 1");
    check(d.planes[1].identical() && d.planes[2].identical(), "chroma untouched");

    auto big        = layout420(10);
    big.bigEndian   = true;
    const auto dBig = compareFrames(a.data(), b.data(), big);
    checkEqual(dBig.planes[0].sse, std::uint64_t(256 * 256),
               "read big endian, the same byte is the high one");
  }

  std::cout << (failures == 0 ? "PASS" : "FAIL") << "\n";
  return failures == 0 ? 0 : 1;
}
