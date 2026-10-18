// Regression: LCEVC enhancement data in the bitstream analysis tree (patch 0073).
//
//   * An Annex-B stream with LCEVC NAL units (HEVC base, H.264 base): the AVC/HEVC parsers name every
//     LCEVC NAL unit "NAL n: LCEVC IDR / non-IDR" and hang its process blocks under it, instead of
//     logging it as an unspecified NAL unit. The counts are those of the reference (LTM) parse.
//   * An AV1 metadata OBU carrying ITU-T T.35 LCEVC: the OBU is named "Metadata (LCEVC)" and gets an
//     "LCEVC (ITU-T T.35)" group with the same fields.
//
// args: <hevc-base .lvc> <h264-base .lvc>
#include <QAbstractItemModel>
#include <QApplication>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "parser/AV1/ParserAV1OBU.h"
#include "parser/AVC/ParserAnnexBAVC.h"
#include "parser/HEVC/ParserAnnexBHEVC.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition)
    ++failures;
}

struct Counts
{
  int idr{}, nonIdr{}, otherNal{}, globalConfig{}, errors{}, t35Groups{}, metadataLcevc{};
  std::vector<std::string> resolution;
};

void walk(const QAbstractItemModel &model, const QModelIndex &index, Counts &c)
{
  const auto name  = model.data(model.index(index.row(), 0, index.parent())).toString().toStdString();
  const auto value = model.data(model.index(index.row(), 1, index.parent())).toString().toStdString();
  if (name.find(": LCEVC IDR") != std::string::npos)
    ++c.idr;
  else if (name.find(": LCEVC non-IDR") != std::string::npos)
    ++c.nonIdr;
  else if (name.rfind("NAL ", 0) == 0)
    ++c.otherNal;
  if (name == "global_config")
    ++c.globalConfig;
  if (name == "error")
    ++c.errors;
  if (name.rfind("LCEVC (ITU-T T.35)", 0) == 0)
    ++c.t35Groups;
  if (name.find("Metadata (LCEVC)") != std::string::npos)
    ++c.metadataLcevc;
  if (name == "resolution_type" || name == "resolution_width")
    c.resolution.push_back(value);
  for (int row = 0; row < model.rowCount(index); ++row)
    walk(model, model.index(row, 0, index), c);
}

Counts countTree(parser::Parser &p)
{
  Counts c;
  auto  *model = p.getPacketItemModel();
  if (model)
    for (int row = 0; row < model->rowCount(); ++row)
      walk(*model, model->index(row, 0), c);
  return c;
}

std::vector<std::uint8_t> readFile(const std::string &path)
{
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3)
  {
    std::cerr << "usage: 54-lcevc-syntax-tree <hevc-base.lvc> <h264-base.lvc>\n";
    return 2;
  }

  // ---- HEVC base: 30 LCEVC NAL units (LTM golden) ----------------------------------------------
  {
    parser::ParserAnnexBHEVC p;
    p.enableModel();
    check(p.runParsingOfFile(argv[1]), "HEVC-base .lvc parsed");
    p.updateNumberModelItems();
    const auto c = countTree(p);
    std::cout << "  HEVC base: IDR " << c.idr << ", non-IDR " << c.nonIdr << ", other NAL " << c.otherNal
              << ", global_config " << c.globalConfig << ", errors " << c.errors << "\n";
    check(c.idr + c.nonIdr == 30, "30 LCEVC NAL units in the tree");
    // LTM writes every picture as nal_unit_type 28, the first included: the stream has no IDR.
    check(c.idr == 0 && c.nonIdr == 30 && c.globalConfig >= 1, "all non-IDR (as LTM writes it), with a global_config");
    check(c.otherNal > 0, "the HEVC base NAL units are still parsed as HEVC");
    check(c.errors == 0, "no parse errors");
  }

  // ---- H.264 base: 4 LCEVC NAL units (hardware encoder), IDR first --------------------------------
  {
    parser::ParserAnnexBAVC p;
    p.enableModel();
    check(p.runParsingOfFile(argv[2]), "H.264-base .lvc parsed");
    p.updateNumberModelItems();
    const auto c = countTree(p);
    std::cout << "  H.264 base: IDR " << c.idr << ", non-IDR " << c.nonIdr << ", other NAL " << c.otherNal
              << ", errors " << c.errors << "\n";
    check(c.idr == 1 && c.nonIdr == 3, "1 IDR + 3 non-IDR LCEVC NAL units");
    check(c.otherNal > 0, "the H.264 base NAL units are still parsed as H.264");
    check(c.errors == 0, "no parse errors");
  }

  // ---- AV1 metadata OBU (ITU-T T.35) carrying the first H.264-stream LCEVC NAL unit ---------------
  {
    const auto data = readFile(argv[2]);
    // The first LCEVC NAL unit: from its start code to the next start code.
    std::size_t start = std::string::npos, end = data.size();
    for (std::size_t i = 0; i + 4 < data.size(); ++i)
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
      {
        const bool lcevc = (data[i + 3] & 0xc0) == 0x40 && data[i + 4] == 0xff;
        if (start == std::string::npos && lcevc)
          start = i;
        else if (start != std::string::npos && i > start)
        {
          end = (data[i - 1] == 0) ? i - 1 : i;
          break;
        }
      }
    check(start != std::string::npos, "found an LCEVC NAL unit to wrap");

    ByteVector obu = {0x2a}; // OBU_METADATA, has_size_field
    std::vector<std::uint8_t> body = {0x04, 0xb4, 0x00, 0x50, 0x00}; // metadata_type T.35, LCEVC
    body.insert(body.end(), data.begin() + std::ptrdiff_t(start), data.begin() + std::ptrdiff_t(end));
    body.push_back(0x80); // trailing bits
    for (std::size_t v = body.size();;)
    {
      obu.push_back(std::uint8_t((v & 0x7f) | (v > 0x7f ? 0x80 : 0)));
      if ((v >>= 7) == 0)
        break;
    }
    obu.insert(obu.end(), body.begin(), body.end());

    parser::ParserAV1OBU p;
    p.enableModel();
    const auto [used, name] = p.parseAndAddOBU(0, obu, nullptr);
    p.updateNumberModelItems();
    const auto c = countTree(p);
    std::cout << "  AV1 metadata OBU: consumed " << used << " of " << obu.size() << " bytes, groups "
              << c.t35Groups << ", global_config " << c.globalConfig << ", errors " << c.errors << "\n";
    check(c.metadataLcevc == 1, "the OBU is named Metadata (LCEVC)");
    check(c.t35Groups == 1 && c.globalConfig == 1, "one LCEVC (ITU-T T.35) group with the IDR's global_config");
    check(c.errors == 0, "no parse errors");
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << "\n";
  return failures == 0 ? 0 : 1;
}
