// Unit test: LCEVC enhancement data extraction and syntax (src/lcevc), against the MPEG reference.
//
// The reference is the LCEVC Test Model (LTM, pin 452a9869) built with BITSTREAM_DEBUG, which logs
// every field it reads as u(bits, "context.label") => value. tests/data/lcevc holds two streams and
// those logs, cut down to the fields this parser reads (the entropy decoder's own reads are left out):
//   * old_town_cross_ltm_hevc.lvc - LTM-encoded, HEVC base 256x144 -> 512x288, 30 pictures, mostly
//     no-enhancement pictures and a temporal layer;
//   * hw_encoder_h264.lvc - the in-house hardware encoder's output, H.264 base, 4 pictures with real
//     residual surfaces (IDR first).
// LTM parses the enhancement in output (POC) order, the file holds it in decode order, so each NAL
// unit's lines are matched as a block, in whatever order LTM read them.
//
// Also: the T.35 and AV1 metadata OBU paths give the same payload as the native NAL unit, and
// emulation prevention is removed.
#include <algorithm>
#include <fstream>
#include <tuple>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "lcevc/LcevcSyntax.h"

using namespace bda::lcevc;

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

std::string dataDir()
{
  std::string here = __FILE__; // .../tests/unit/lcevc-syntax.cpp
  return here.substr(0, here.rfind("/unit/")) + "/data/lcevc/";
}

std::vector<std::uint8_t> readFile(const std::string &path)
{
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

using Line = std::tuple<std::string, unsigned, std::uint64_t>;

std::vector<Line> readTrace(const std::string &path)
{
  std::vector<Line> out;
  std::ifstream     f(path);
  std::string       label;
  unsigned          bits;
  std::uint64_t     value;
  while (f >> label >> bits >> value)
    out.emplace_back(label, bits, value);
  return out;
}

// Parse every NAL unit and match each one's trace lines as a block against the reference.
bool matchesReference(const std::string &stream, const std::string &trace, int &nals, SyntaxParser &parser)
{
  const auto data = readFile(dataDir() + stream);
  const auto ref  = readTrace(dataDir() + trace);
  const auto found = findLcevcNals(data.data(), data.size());
  nals             = int(found.size());
  std::vector<std::vector<Line>> blocks;
  for (const auto &nal : found)
  {
    const auto before = parser.trace().size();
    parser.parse(nal);
    std::vector<Line> b;
    for (auto i = before; i < parser.trace().size(); ++i)
      b.emplace_back(parser.trace()[i].label, parser.trace()[i].bits, parser.trace()[i].value);
    blocks.push_back(std::move(b));
  }
  std::vector<bool> used(blocks.size(), false);
  std::size_t       pos = 0;
  while (pos < ref.size())
  {
    bool hit = false;
    for (std::size_t i = 0; i < blocks.size() && !hit; ++i)
      if (!used[i] && pos + blocks[i].size() <= ref.size() &&
          std::equal(blocks[i].begin(), blocks[i].end(), ref.begin() + std::ptrdiff_t(pos)))
      {
        used[i] = hit = true;
        pos += blocks[i].size();
      }
    if (!hit)
      return false;
  }
  return std::all_of(used.begin(), used.end(), [](bool u) { return u; });
}
} // namespace

int main()
{
  // ---- against the reference ---------------------------------------------------------------------
  {
    SyntaxParser parser;
    int          nals = 0;
    const bool same = matchesReference("old_town_cross_ltm_hevc.lvc", "old_town_cross_ltm_hevc.ltm-trace.txt", nals, parser);
    check(same && nals == 30, "HEVC-base stream: 30 NAL units, every field equal to LTM's (" + std::to_string(nals) + ")");
    const auto &g = parser.global();
    check(g.resolutionWidth == 512 && g.resolutionHeight == 288 && g.transformBlockSize == 4 && g.temporalEnabled &&
              g.scalingModeLevel2 == 2 && g.scalingModeLevel1 == 0,
          "global_config: 512x288, DDS, temporal on, 2D scaling to level 2");
    check(parser.lastPicture().baseWidth == 256 && parser.lastPicture().baseHeight == 144, "base 256x144");
  }
  {
    SyntaxParser parser;
    int          nals = 0;
    const bool same = matchesReference("hw_encoder_h264.lvc", "hw_encoder_h264.ltm-trace.txt", nals, parser);
    check(same && nals == 4, "H.264-base stream: 4 NAL units, every field equal to LTM's (" + std::to_string(nals) + ")");
    const auto data  = readFile(dataDir() + "hw_encoder_h264.lvc");
    const auto found = findLcevcNals(data.data(), data.size());
    check(found.size() == 4 && found[0].isIdr() && !found[1].isIdr(), "the first is an IDR (type 29), then type 28");
    SyntaxParser p2;
    p2.parse(found[0]);
    const auto &s = p2.lastPicture();
    check(s.enhancementEnabled && s.stepWidthLevel1 == 512 && s.stepWidthLevel2 == 1024 && s.surfaceBytes[0][1] > 0,
          "IDR picture: step widths 512 / 1024, level-2 luma surfaces carry data (" +
              std::to_string(s.surfaceBytes[0][1]) + " bytes)");
  }

  // ---- other carriages give the same payload --------------------------------------------------
  {
    const auto data  = readFile(dataDir() + "hw_encoder_h264.lvc");
    const auto found = findLcevcNals(data.data(), data.size());
    const auto &first = found.front();
    std::vector<std::uint8_t> nalBytes(data.begin() + std::ptrdiff_t(first.offset),
                                       data.begin() + std::ptrdiff_t(first.offset + first.size));

    std::vector<std::uint8_t> t35 = {0xb4, 0x00, 0x50, 0x00};
    t35.insert(t35.end(), nalBytes.begin(), nalBytes.end());
    const auto viaT35 = findLcevcInT35(t35.data(), t35.size());
    check(viaT35.size() == 1 && viaT35[0].payload == first.payload && viaT35[0].isIdr(),
          "ITU-T T.35 (B4 00 50 00 + NAL unit) gives the same payload");
    std::vector<std::uint8_t> other = {0xb5, 0x00, 0x31, 0x00};
    other.insert(other.end(), nalBytes.begin(), nalBytes.end());
    check(findLcevcInT35(other.data(), other.size()).empty(), "another T.35 provider is not LCEVC");

    // AV1: a temporal delimiter, then a metadata OBU with metadata_type 4 (ITU-T T.35) and trailing bits.
    std::vector<std::uint8_t> body = {0x04};
    body.insert(body.end(), t35.begin(), t35.end());
    body.push_back(0x80);
    std::vector<std::uint8_t> tu = {0x12, 0x00, 0x2a};
    for (std::size_t v = body.size();;)
    {
      tu.push_back(std::uint8_t((v & 0x7f) | (v > 0x7f ? 0x80 : 0)));
      if ((v >>= 7) == 0)
        break;
    }
    tu.insert(tu.end(), body.begin(), body.end());
    const auto viaAv1 = findLcevcInAv1TemporalUnit(tu.data(), tu.size());
    check(viaAv1.size() == 1 && viaAv1[0].payload == first.payload,
          "an AV1 metadata OBU (ITU-T T.35) gives the same payload");
  }

  // ---- what the decoders are handed ----------------------------------------------------------------
  for (const char *stream : {"old_town_cross_ltm_hevc.lvc", "hw_encoder_h264.lvc"})
  {
    const auto data  = readFile(dataDir() + stream);
    const auto found = findLcevcNals(data.data(), data.size());
    bool       same  = !found.empty();
    for (const auto &nal : found)
    {
      const std::vector<std::uint8_t> original(data.begin() + std::ptrdiff_t(nal.offset),
                                               data.begin() + std::ptrdiff_t(nal.offset + nal.size));
      same = same && toAnnexB(nal) == original;
      // A parser unit: with a 4-byte start code and a trailing zero, as Annex-B readers hand it over.
      std::vector<std::uint8_t> unit = {0x00};
      unit.insert(unit.end(), original.begin(), original.end());
      unit.push_back(0x00);
      const auto fromUnit = lcevcNalFromUnit(unit.data(), unit.size());
      same = same && fromUnit && fromUnit->payload == nal.payload && fromUnit->nalUnitType == nal.nalUnitType;
    }
    check(same, std::string(stream) + ": toAnnexB() gives the stored bytes back, lcevcNalFromUnit() the payload");
  }
  {
    const auto hevc = readFile(dataDir() + "old_town_cross_ltm_hevc.lvc");
    const auto avc  = readFile(dataDir() + "hw_encoder_h264.lvc");
    check(detectAnnexBBaseCodec(hevc.data(), hevc.size()) == AnnexBCodec::HEVC &&
              detectAnnexBBaseCodec(avc.data(), avc.size()) == AnnexBCodec::AVC,
          "base codec of a .lvc: HEVC and H.264");
    const std::uint8_t sps[] = {0x00, 0x00, 0x01, 0x67, 0x42};
    check(lcevcNalFromUnit(sps, sizeof(sps)) == std::nullopt, "an H.264 SPS is not an LCEVC NAL unit");
  }
  {
    // IVF: header, then frames of a temporal delimiter + metadata OBU (T.35 LCEVC), and one without.
    const auto data  = readFile(dataDir() + "hw_encoder_h264.lvc");
    const auto found = findLcevcNals(data.data(), data.size());
    std::vector<std::uint8_t> ivf(32, 0);
    std::copy_n("DKIF", 4, ivf.begin());
    ivf[6] = 32;
    auto addFrame = [&ivf](const std::vector<std::uint8_t> &tu) {
      const auto n = tu.size();
      ivf.insert(ivf.end(), {std::uint8_t(n), std::uint8_t(n >> 8), std::uint8_t(n >> 16), std::uint8_t(n >> 24)});
      ivf.insert(ivf.end(), 8, 0);
      ivf.insert(ivf.end(), tu.begin(), tu.end());
    };
    for (std::size_t k = 0; k < 3; ++k)
    {
      std::vector<std::uint8_t> tu = {0x12, 0x00};
      if (k != 1)
      {
        std::vector<std::uint8_t> body = {0x04, 0xb4, 0x00, 0x50, 0x00};
        const auto                nal  = toAnnexB(found[k]);
        body.insert(body.end(), nal.begin(), nal.end());
        body.push_back(0x80);
        tu.push_back(0x2a);
        for (std::size_t v = body.size();;)
        {
          tu.push_back(std::uint8_t((v & 0x7f) | (v > 0x7f ? 0x80 : 0)));
          if ((v >>= 7) == 0)
            break;
        }
        tu.insert(tu.end(), body.begin(), body.end());
      }
      addFrame(tu);
    }
    const auto perFrame = lcevcPerIvfFrame(ivf.data(), ivf.size());
    check(perFrame.size() == 3 && perFrame[0] == toAnnexB(found[0]) && perFrame[1].empty() &&
              perFrame[2] == toAnnexB(found[2]),
          "IVF: the LCEVC NAL unit of each frame, none for a frame without metadata");
  }

  // ---- emulation prevention ------------------------------------------------------------------------
  {
    const std::uint8_t in[] = {0x12, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x00, 0x05};
    const auto         out  = unescapeRbsp(in, sizeof(in));
    check(out == std::vector<std::uint8_t>({0x12, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05}),
          "00 00 03 xx loses the 03");
  }

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
