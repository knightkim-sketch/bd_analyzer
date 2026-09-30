// Compare two AV1 streams OBU payload by OBU payload.
//
//   stream-diff-payload <a> <b> [--max N]
//
// Step 3 of the Find diff pipeline. Step 2 compares header syntax and, on a real mismatch, finds
// that the only differing element is obu_size: the two encoders made the same decisions and coded
// them differently. This says which OBU's payload differs and from which byte.
//
// Byte offsets, not bit positions. The decoder's per-block bit positions are relative to the packet
// libavformat produced, which stops being the packet of the displayed frame as soon as a frame is
// hidden; whole payloads have no such ambiguity.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "diff/ObuPayloadDiff.h"

namespace
{
std::vector<std::uint8_t> readFile(const std::string &path, bool &ok)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  ok = false;
  if (!file)
    return {};
  const auto size = file.tellg();
  file.seekg(0);
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size), 0);
  ok = bool(file.read(reinterpret_cast<char *>(data.data()), size));
  return data;
}

const char *kindName(bda::diff::PayloadDiffKind kind)
{
  switch (kind)
  {
  case bda::diff::PayloadDiffKind::Equal:          return "equal";
  case bda::diff::PayloadDiffKind::SizeDiffers:    return "size differs";
  case bda::diff::PayloadDiffKind::ContentDiffers: return "content differs";
  case bda::diff::PayloadDiffKind::OnlyInA:        return "only in A";
  case bda::diff::PayloadDiffKind::OnlyInB:        return "only in B";
  }
  return "?";
}
} // namespace

int main(int argc, char **argv)
{
  std::vector<std::string> files;
  std::size_t              maxReported = 20;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--max" && i + 1 < argc)
      maxReported = std::strtoul(argv[++i], nullptr, 10);
    else
      files.push_back(a);
  }
  if (files.size() != 2)
  {
    std::cerr << "usage: stream-diff-payload <a> <b> [--max N]\n";
    return 2;
  }

  bool okA = false, okB = false;
  const auto dataA = readFile(files[0], okA);
  const auto dataB = readFile(files[1], okB);
  if (!okA || !okB)
  {
    std::cerr << "could not read " << (okA ? files[1] : files[0]) << "\n";
    return 1;
  }

  const auto scanA = bda::diff::scanObus(dataA.data(), dataA.size());
  const auto scanB = bda::diff::scanObus(dataB.data(), dataB.size());
  if (!scanA.ok || !scanB.ok)
  {
    std::cerr << "could not split the stream into OBUs: "
              << (scanA.ok ? scanB.error : scanA.error) << "\n";
    return 1;
  }

  printf("A %s  (%zu bytes, %zu OBUs)\n", files[0].c_str(), dataA.size(), scanA.obus.size());
  printf("B %s  (%zu bytes, %zu OBUs)\n", files[1].c_str(), dataB.size(), scanB.obus.size());

  const auto result =
      bda::diff::comparePayloads(scanA.obus, dataA.data(), scanB.obus, dataB.data());
  if (result.identical())
  {
    printf("\nevery OBU payload is identical (%zu compared)\n", result.comparedObus);
    return 0;
  }

  const auto *first = result.first();
  printf("\nfirst differing payload: %s\n", first->a.label().c_str());
  printf("    %s · A %zu bytes, B %zu bytes\n", kindName(first->kind), first->a.payloadSize,
         first->b.payloadSize);
  if (first->hasFirstDifferingByte)
    printf("    identical for the first %zu payload bytes, then diverges\n"
           "    (file offset A 0x%zx, B 0x%zx)\n",
           first->firstDifferingByte, first->a.payloadOffset + first->firstDifferingByte,
           first->b.payloadOffset + first->firstDifferingByte);
  printf("    -> step 4 decodes this frame and compares block syntax to place the superblock\n");

  printf("\n%zu of %zu OBU payloads differ, showing the first %zu:\n", result.diffs.size(),
         result.comparedObus, maxReported);
  std::size_t shown = 0;
  for (const auto &d : result.diffs)
  {
    if (shown++ >= maxReported)
      break;
    printf("  %-34s %-16s A=%-7zu B=%-7zu first diff @ %zu\n", d.a.label().c_str(),
           kindName(d.kind), d.a.payloadSize, d.b.payloadSize, d.firstDifferingByte);
  }
  return 0;
}
