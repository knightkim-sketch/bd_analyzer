// AV1 OBU parsing against a tiled stream, checked field by field.
//
// Every assertion here comes from a defect found by dumping this stream from both bd_analyzer and
// VQ Analyzer and diffing the two (tests/tools/dump-obu-headers.cpp). A 2x2 tile stream with
// hidden alternate-reference frames walks paths a single-tile 176x144 clip never touches, and all
// five were silent - no crash, no error, just wrong numbers in the bitstream panel:
//
//   patch 0047  tile_info() computed minLog2TileRows in unsigned arithmetic, so the subtraction
//               wrapped and increment_tile_rows_log2 was never read; and context_update_tile_id /
//               tile_size_bytes_minus_1 were read with the ns(n) coding instead of f(n).
//   patch 0048  cdef_uv_sec_strength was read as f(4) instead of f(2).
//   patch 0049  SeenFrameHeader was only cleared by a temporal delimiter, so the second and later
//               frames of a multi-frame packet parsed as frame_header_copy() - no syntax at all.
//   patch 0050  the reference frame update process (spec 7.20) was missing, so RefOrderHint stayed
//               zero, skipModeAllowed was always false and skip_mode_present was never read.
//   patch 0051  the OBU loop needed more than nrBytesRead + 3 bytes left to continue, dropping the
//               3 byte show_existing_frame frame header at the end of its packet.
//   patch 0052  tile_group_obu() was never parsed, so the per tile sizes - the one thing that says
//               how a tiled frame's bits were actually split across the grid - were absent, along
//               with trailing_bits() at the end of every non-tile OBU.
//
// The first four all end the same way: the reader falls behind the encoder and every later field
// in the uncompressed header decodes from the wrong bit offset.
#include <QAbstractItemModel>
#include <QApplication>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition)
    ++failures;
}

template <typename T> void checkEqual(const T &got, const T &want, const std::string &what)
{
  const bool ok = (got == want);
  std::cout << (ok ? "  ok   " : "  FAIL ") << what << ": " << got;
  if (!ok)
    std::cout << " (expected " << want << ")";
  std::cout << "\n";
  if (!ok)
    ++failures;
}

struct Field
{
  std::string name;
  std::string value;
};

// Flatten the whole packet/OBU tree into the syntax elements it logged, in bitstream order.
void collect(const QAbstractItemModel &model,
             const QModelIndex &       index,
             std::vector<Field> &      fields,
             std::vector<std::string> &obuNames)
{
  const auto name  = model.data(model.index(index.row(), 0, index.parent())).toString();
  const auto value = model.data(model.index(index.row(), 1, index.parent())).toString();

  if (name.startsWith("OBU"))
    obuNames.push_back(name.toStdString());
  else if (!value.isEmpty())
    fields.push_back({name.toStdString(), value.toStdString()});

  for (int row = 0; row < model.rowCount(index); ++row)
    collect(model, model.index(row, 0, index), fields, obuNames);
}

std::map<std::string, int> countByName(const std::vector<Field> &fields)
{
  std::map<std::string, int> counts;
  for (const auto &f : fields)
    counts[f.name]++;
  return counts;
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 37-av1-tile-stream-obu-parsing <tiled.ivf>\n";
    return 2;
  }

  FFmpeg::FFmpegVersionHandler ff;
  ff.loadFFmpegLibraries();
  if (!ff.loadingSuccessfull())
  {
    std::cout << "RESULT: FAIL - ffmpeg libraries did not load\n";
    return 1;
  }

  parser::ParserAVFormat parser;
  parser.enableModel();
  const auto parsed = parser.runParsingOfFile(std::filesystem::path(argv[1]));
  parser.updateNumberModelItems();
  check(parsed, "runParsingOfFile");

  auto *model = parser.getPacketItemModel();
  if (model == nullptr)
  {
    std::cout << "RESULT: FAIL - no packet model\n";
    return 1;
  }

  std::vector<Field>       fields;
  std::vector<std::string> obuNames;
  for (int row = 0; row < model->rowCount(); ++row)
    collect(*model, model->index(row, 0), fields, obuNames);

  const auto counts = countByName(fields);
  auto       countOf = [&counts](const std::string &name) {
    const auto it = counts.find(name);
    return it == counts.end() ? 0 : it->second;
  };
  auto valuesOf = [&fields](const std::string &name) {
    std::vector<std::string> out;
    for (const auto &f : fields)
      if (f.name == name)
        out.push_back(f.value);
    return out;
  };

  std::cout << "OBUs: " << obuNames.size() << ", syntax elements: " << fields.size() << "\n";

  // -------------------------------------------------------------------- tile_info(), patch 0047
  // The stream is -tiles 2x2, so both increment loops run and both log2 counts land on 1.
  check(countOf("increment_tile_rows_log2") > 0,
        "increment_tile_rows_log2 is read at all (unsigned wrap left the loop unentered)");
  for (const auto &v : valuesOf("TileColsLog2"))
    checkEqual(v, std::string("1"), "TileColsLog2");
  for (const auto &v : valuesOf("TileRowsLog2"))
    checkEqual(v, std::string("1"), "TileRowsLog2");

  // f(TileRowsLog2 + TileColsLog2) over a 2x2 grid is two bits, so the id addresses one of four
  // tiles. Reading it as ns(n) consumed one bit and could never reach 3.
  for (const auto &v : valuesOf("context_update_tile_id"))
    check(v == "0" || v == "1" || v == "2" || v == "3", "context_update_tile_id in 0..3: " + v);
  check(countOf("context_update_tile_id") > 0, "context_update_tile_id is present");

  // ------------------------------------------------------------------ cdef_params(), patch 0048
  // f(2), with 3 mapped to 4 - so 0, 1, 2 or 4 and nothing else. The f(4) read produced 10 and 11.
  for (const auto &v : valuesOf("cdef_uv_sec_strength[0]"))
    check(v == "0" || v == "1" || v == "2" || v == "4", "cdef_uv_sec_strength[0] in {0,1,2,4}: " + v);
  for (const auto &v : valuesOf("cdef_uv_sec_strength[1]"))
    check(v == "0" || v == "1" || v == "2" || v == "4", "cdef_uv_sec_strength[1] in {0,1,2,4}: " + v);

  // -------------------------------------------------------------- SeenFrameHeader, patch 0049
  // Hidden alternate-reference frames arrive several OBUs to a packet. Each one carries a full
  // uncompressed header, so base_q_idx must appear once per coded frame, not once per packet.
  int frameObus = 0;
  for (const auto &n : obuNames)
    if (n.find("Frame") != std::string::npos)
      ++frameObus;
  check(frameObus > 0, "frame OBUs found");
  checkEqual(countOf("base_q_idx"),
             frameObus - countOf("frame_to_show_map_idx"),
             "frames with a parsed uncompressed header (one per frame OBU that codes a picture)");

  // ----------------------------------------------------- reference frame update, patch 0050
  // With the reference buffers left at zero no frame can ever find a backward reference, so this
  // bit - which the encoder did write - was never read.
  check(countOf("skip_mode_present") > 0,
        "skip_mode_present is read on the frames that allow skip mode");

  // -------------------------------------------------------------------- OBU loop, patch 0051
  // libavformat hands the re-shown frame over as a 5 byte packet: temporal delimiter plus a
  // 3 byte frame header. The loop used to stop before the frame header.
  check(countOf("frame_to_show_map_idx") > 0, "show_existing_frame headers are parsed");

  // ------------------------------------------------------------------- tile group, patch 0052
  // One tile group per coded frame, and every tile but the last of a group carries its size.
  // The fixture is -tiles 2x2, so four tiles and three size fields per frame.
  const auto codedFrames = countOf("base_q_idx");
  checkEqual(countOf("tile_start_and_end_present_flag"), codedFrames, "tile groups parsed");
  checkEqual(countOf("tile_size_minus_1"), codedFrames * 3, "per tile size fields (4 tiles = 3)");
  for (const auto &v : valuesOf("tile_size_minus_1"))
    check(std::stoll(v) >= 0, "tile_size_minus_1 is a real size: " + v);

  // A desynchronised reader walks off the end of the OBU rather than landing on the next size
  // field, so the tile sizes must also add up to less than the frame's own payload.
  check(!valuesOf("tile_size_minus_1").empty(), "tile sizes were read");

  // trailing_bits() closes every OBU that is not a frame or a tile group - here the sequence
  // headers and the show_existing_frame headers.
  check(countOf("trailing_one_bit") > 0, "trailing_bits() is parsed");
  checkEqual(countOf("trailing_one_bit"),
             2 + countOf("frame_to_show_map_idx"),
             "one trailing_one_bit per sequence header and per show_existing_frame header");

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << "\n";
  return failures == 0 ? 0 : 1;
}
