// Put LCEVC enhancement into an AV1 stream: one metadata OBU (ITU-T T.35) per temporal unit.
//
//   lcevc-av1-mux <base.ivf> <enhancement> <out.ivf>
//
// <enhancement> holds the LCEVC NAL units, one per picture in output order:
//   * enh.nal (+ enh.nal.sizes next to it), as the LCEVC encoders write them, or
//   * any Annex-B stream carrying them, e.g. the .lvc LTM's ModelEncoder writes around a carrier base
//     encoded without B-frames (so file order is output order).
// Picture k goes into temporal unit k of the IVF, which assumes one shown frame per temporal unit.
// bd-analyzer opens the result (AV1 over dav1d + LCEVC); docs/ai/30-designs/lcevc-decoding-design.md
// has the whole recipe from a source YUV.
//
// Build:
//   g++ -std=c++20 -O2 -Isrc tools/cli/lcevc-av1-mux.cpp src/lcevc/LcevcBitstream.cpp -o lcevc-av1-mux
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "lcevc/LcevcBitstream.h"

namespace
{
std::optional<std::vector<std::uint8_t>> readFile(const std::string &path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return std::nullopt;
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}
} // namespace

int main(int argc, char **argv)
{
  using namespace bda::lcevc;
  if (argc != 4)
  {
    std::cerr << "usage: lcevc-av1-mux <base.ivf> <enhancement .nal|.lvc> <out.ivf>\n";
    return 2;
  }
  const auto base        = readFile(argv[1]);
  const auto enhancement = readFile(argv[2]);
  if (!base || !enhancement)
  {
    std::cerr << "cannot read " << (!base ? argv[1] : argv[2]) << "\n";
    return 1;
  }

  std::optional<std::vector<std::size_t>> sizes;
  if (const auto text = readFile(std::string(argv[2]) + ".sizes"))
  {
    sizes = parseSizesFile(std::string(text->begin(), text->end()));
    if (!sizes)
    {
      std::cerr << argv[2] << ".sizes is not a list of byte counts\n";
      return 1;
    }
  }
  const auto perPicture =
      lcevcPerPictureFromNalFile(enhancement->data(), enhancement->size(), sizes ? &*sizes : nullptr);
  const auto frames = lcevcPerIvfFrame(base->data(), base->size()).size();
  if (perPicture.empty() || frames == 0)
  {
    std::cerr << (perPicture.empty() ? "no LCEVC NAL units in " + std::string(argv[2])
                                     : std::string(argv[1]) + " is not an IVF file")
              << "\n";
    return 1;
  }

  const auto out = muxLcevcIntoIvf(base->data(), base->size(), perPicture);
  if (out.empty())
  {
    std::cerr << "cannot rewrite " << argv[1] << " (an OBU before the first frame has no size field?)\n";
    return 1;
  }
  std::ofstream file(argv[3], std::ios::binary);
  file.write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
  if (!file)
  {
    std::cerr << "cannot write " << argv[3] << "\n";
    return 1;
  }
  std::cout << argv[3] << ": " << frames << " temporal units, " << std::min(frames, perPicture.size())
            << " with LCEVC" << (perPicture.size() != frames ? " (picture count differs from the base)" : "")
            << "\n";
  return 0;
}
