// Unit test: splitting a stream into OBUs, and comparing two streams payload by payload.
//
// The walker is the part that earns a test. It advances by a length it reads out of the stream, so
// a wrong size does not fail - it silently reframes everything after it, and the comparison then
// reports differences that are artefacts of the walk. The refusals matter for the same reason.
#include "diff/ObuPayloadDiff.h"

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

// obu_header: forbidden(1) type(4) extension(1) has_size(1) reserved(1)
std::uint8_t header(unsigned type, bool extension = false) 
{
  return std::uint8_t((type << 3) | (extension ? 0x04 : 0) | 0x02);
}

void appendObu(std::vector<std::uint8_t> &out, unsigned type, const std::vector<std::uint8_t> &payload)
{
  out.push_back(header(type));
  auto size = payload.size();      // leb128
  do
  {
    auto byte = std::uint8_t(size & 0x7f);
    size >>= 7;
    if (size)
      byte |= 0x80;
    out.push_back(byte);
  } while (size);
  out.insert(out.end(), payload.begin(), payload.end());
}
} // namespace

int main()
{
  {
    // Two temporal units: delimiter + sequence header + frame, then delimiter + frame.
    std::vector<std::uint8_t> s;
    appendObu(s, 2, {});                 // temporal delimiter
    appendObu(s, 1, {0xAA, 0xBB});       // sequence header
    appendObu(s, 6, {1, 2, 3, 4});       // frame
    appendObu(s, 2, {});
    appendObu(s, 6, {5, 6, 7, 8});

    const auto scan = scanObus(s.data(), s.size());
    check(scan.ok, "a well formed stream scans");
    checkEqual(scan.obus.size(), std::size_t(5), "five OBUs");
    check(scan.obus[0].type == ObuType::TemporalDelimiter, "first is the delimiter");
    check(scan.obus[2].type == ObuType::Frame, "third is a frame");
    checkEqual(scan.obus[0].temporalUnit, std::size_t(0), "first unit is 0");
    checkEqual(scan.obus[2].temporalUnit, std::size_t(0), "the frame is in unit 0");
    checkEqual(scan.obus[3].temporalUnit, std::size_t(1), "the second delimiter opens unit 1");
    checkEqual(scan.obus[4].temporalUnit, std::size_t(1), "and its frame follows it");
    checkEqual(scan.obus[2].indexInUnit, std::size_t(2), "index within the unit counts from 0");
    checkEqual(scan.obus[2].payloadSize, std::size_t(4), "payload size is read");
    checkEqual(std::string(scan.obus[2].label()), std::string("TU 0 / OBU 2: Frame"), "label");
  }

  {
    // A size that needs two leb128 bytes; getting the continuation bit wrong reframes the stream.
    std::vector<std::uint8_t> payload(200, 0x5A);
    std::vector<std::uint8_t> s;
    appendObu(s, 6, payload);
    const auto scan = scanObus(s.data(), s.size());
    check(scan.ok, "a multi-byte leb128 size scans");
    checkEqual(scan.obus.size(), std::size_t(1), "one OBU");
    checkEqual(scan.obus[0].payloadSize, std::size_t(200), "and its size is right");
  }

  {
    const std::uint8_t forbidden[] = {0x92, 0x00};
    const auto         scan        = scanObus(forbidden, sizeof(forbidden));
    check(!scan.ok, "obu_forbidden_bit is refused");
    check(!scan.error.empty(), "and says why");
  }
  {
    const std::uint8_t noSize[] = {0x30, 0x00}; // type 6, has_size_field clear
    const auto         scan     = scanObus(noSize, sizeof(noSize));
    check(!scan.ok, "a stream without obu_size is refused rather than guessed at");
  }
  {
    const std::uint8_t truncated[] = {0x32, 0x40}; // claims 64 payload bytes, has none
    const auto         scan        = scanObus(truncated, sizeof(truncated));
    check(!scan.ok, "a payload past the end of the stream is refused");
  }

  // ------------------------------------------------------------------- payload comparison
  {
    std::vector<std::uint8_t> s;
    appendObu(s, 2, {});
    appendObu(s, 6, {1, 2, 3, 4, 5});
    const auto scan = scanObus(s.data(), s.size());
    const auto r    = comparePayloads(scan.obus, s.data(), scan.obus, s.data());
    check(r.identical(), "a stream against itself has no payload difference");
    checkEqual(r.comparedObus, std::size_t(2), "both OBUs were compared");
  }
  {
    std::vector<std::uint8_t> a, b;
    appendObu(a, 2, {});  appendObu(b, 2, {});
    appendObu(a, 6, {1, 2, 3, 4, 5});
    appendObu(b, 6, {1, 2, 9, 4, 5});     // same length, third byte differs
    const auto sa = scanObus(a.data(), a.size());
    const auto sb = scanObus(b.data(), b.size());
    const auto r  = comparePayloads(sa.obus, a.data(), sb.obus, b.data());
    checkEqual(r.diffs.size(), std::size_t(1), "one OBU differs");
    check(r.first()->kind == PayloadDiffKind::ContentDiffers, "same size, different content");
    check(r.first()->hasFirstDifferingByte, "the first differing byte is reported");
    checkEqual(r.first()->firstDifferingByte, std::size_t(2), "and it is byte 2 of the payload");
  }
  {
    std::vector<std::uint8_t> a, b;
    appendObu(a, 6, {1, 2, 3, 4, 5});
    appendObu(b, 6, {1, 2, 3});
    const auto sa = scanObus(a.data(), a.size());
    const auto sb = scanObus(b.data(), b.size());
    const auto r  = comparePayloads(sa.obus, a.data(), sb.obus, b.data());
    check(r.first()->kind == PayloadDiffKind::SizeDiffers, "a length difference is its own kind");
    checkEqual(r.first()->firstDifferingByte, std::size_t(3),
               "and reports how far the two agreed");
  }
  {
    std::vector<std::uint8_t> a, b;
    appendObu(a, 6, {1}); appendObu(a, 6, {2});
    appendObu(b, 6, {1});
    const auto sa = scanObus(a.data(), a.size());
    const auto sb = scanObus(b.data(), b.size());
    const auto r  = comparePayloads(sa.obus, a.data(), sb.obus, b.data());
    checkEqual(r.diffs.size(), std::size_t(1), "the unmatched OBU is reported");
    check(r.diffs[0].kind == PayloadDiffKind::OnlyInA, "as present only in A");
  }

  std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
  return failures == 0 ? 0 : 1;
}
