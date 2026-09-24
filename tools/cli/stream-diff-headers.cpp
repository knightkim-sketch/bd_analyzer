// Compare the header syntax of two AV1 streams and report where they first diverge.
//
//   stream-diff-headers <a.ivf> <b.ivf> [--max N] [--all]
//
// This is layer A of the Find diff feature, driven from the command line so the comparison can be
// checked on real streams before any of it is wired to a window. It walks the same parser model
// the Bitstream Analysis panel shows, flattens it to name/value pairs, and hands both sides to
// bda::diff::compareSyntax.
//
// Only syntax is compared. The packet rows libavformat contributes - pts, dts, stream_index and
// the like - are not in the bitstream and are skipped by descending into OBU nodes only.
#include <QAbstractItemModel>
#include <QApplication>

#include <iostream>
#include <string>
#include <vector>

#include "diff/SyntaxDiff.h"
#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"

namespace
{

/* Flatten every syntax element under the OBU nodes, in bitstream order.
 *
 * Derived values the parser logs for convenience (FrameWidth, TileColsLog2 ...) are kept: they are
 * computed from the syntax, so if one differs the syntax that produced it differs too, and naming
 * the derived value is often the clearer report.
 */
void collect(const QAbstractItemModel &         model,
             const QModelIndex &                index,
             std::vector<bda::diff::SyntaxElement> &out)
{
  const auto name  = model.data(model.index(index.row(), 0, index.parent())).toString();
  const auto value = model.data(model.index(index.row(), 1, index.parent())).toString();
  if (!value.isEmpty() && !name.startsWith("raw_byte"))
    out.push_back({name.toStdString(), value.toStdString()});

  for (int row = 0; row < model.rowCount(index); ++row)
    collect(model, model.index(row, 0, index), out);
}

/* One section per OBU, labelled so a difference can be named rather than numbered.
 *
 * Splitting matters: flattened whole, a 24 frame clip is ~13000 elements and the exact alignment
 * is out of reach. Per OBU it is a few hundred, so the alignment stays exact and the report says
 * which OBU diverged.
 */
bool readSections(const std::string &path, std::vector<bda::diff::SyntaxSection> &out)
{
  parser::ParserAVFormat parser;
  parser.enableModel();
  if (!parser.runParsingOfFile(std::filesystem::path(path)))
    return false;
  parser.updateNumberModelItems();

  auto *model = parser.getPacketItemModel();
  if (model == nullptr)
    return false;

  for (int packet = 0; packet < model->rowCount(); ++packet)
  {
    const auto packetIdx = model->index(packet, 0);

    /* Label the packet with the index the parser logged, not with the row number.
     *
     * The two are not the same, and a label that silently disagrees with what the Bitstream
     * Analysis panel shows sends the reader to the wrong packet.
     */
    auto packetLabel = "row " + std::to_string(packet);
    for (int child = 0; child < model->rowCount(packetIdx); ++child)
    {
      const auto childIdx = model->index(child, 0, packetIdx);
      if (model->data(childIdx).toString() == "Global AVPacket Count")
      {
        packetLabel =
            "packet " +
            model->data(model->index(child, 1, packetIdx)).toString().toStdString();
        break;
      }
    }

    for (int child = 0; child < model->rowCount(packetIdx); ++child)
    {
      const auto childIdx = model->index(child, 0, packetIdx);
      const auto name     = model->data(childIdx).toString();
      if (!name.startsWith("OBU"))
        continue;
      bda::diff::SyntaxSection section;
      section.label = packetLabel + " / " + name.toStdString();
      collect(*model, childIdx, section.elements);
      // An OBU with no syntax of its own - a temporal delimiter - would pair with anything and
      // says nothing. Keeping it would only shift the positional alignment.
      if (!section.elements.empty())
        out.push_back(std::move(section));
    }
  }
  return true;
}

const char *kindName(bda::diff::DiffKind kind)
{
  switch (kind)
  {
  case bda::diff::DiffKind::ValueMismatch: return "value";
  case bda::diff::DiffKind::OnlyInA:       return "only in A";
  case bda::diff::DiffKind::OnlyInB:       return "only in B";
  }
  return "?";
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  std::vector<std::string> files;
  std::size_t              maxReported = 20;
  bool                     reportAll   = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--max" && i + 1 < argc)
      maxReported = std::stoul(argv[++i]);
    else if (a == "--all")
      reportAll = true;
    else
      files.push_back(a);
  }
  if (files.size() != 2)
  {
    std::cerr << "usage: stream-diff-headers <a> <b> [--max N] [--all]\n";
    return 2;
  }

  FFmpeg::FFmpegVersionHandler ff;
  ff.loadFFmpegLibraries();
  if (!ff.loadingSuccessfull())
  {
    std::cerr << "ffmpeg libraries did not load\n";
    return 1;
  }

  std::vector<bda::diff::SyntaxSection> a, b;
  if (!readSections(files[0], a)) { std::cerr << "could not parse " << files[0] << "\n"; return 1; }
  if (!readSections(files[1], b)) { std::cerr << "could not parse " << files[1] << "\n"; return 1; }

  auto elementCount = [](const std::vector<bda::diff::SyntaxSection> &s) {
    std::size_t n = 0;
    for (const auto &section : s)
      n += section.elements.size();
    return n;
  };
  std::cout << "A " << files[0] << "  (" << a.size() << " OBUs, " << elementCount(a)
            << " syntax elements)\n";
  std::cout << "B " << files[1] << "  (" << b.size() << " OBUs, " << elementCount(b)
            << " syntax elements)\n";

  const auto result = bda::diff::compareSections(a, b);
  std::size_t degraded = 0;
  for (const auto &section : result.sections)
    if (section.result.degraded)
      ++degraded;
  if (degraded > 0)
    std::cout << "WARNING " << degraded
              << " sections were too large to align exactly and used the windowed scan\n";

  if (result.identical())
  {
    std::cout << "\nheaders are identical\n";
    return 0;
  }

  const auto *firstSection = result.firstDiffering();
  std::cout << "\nfirst divergence in: " << firstSection->label << "\n";
  if (firstSection->onlyInA)
    std::cout << "    this OBU has no counterpart in B\n";
  else if (firstSection->onlyInB)
    std::cout << "    this OBU has no counterpart in A\n";
  else if (const auto *d = firstSection->result.first())
  {
    std::cout << "    " << d->name << "  [" << kindName(d->kind) << "]\n";
    if (d->kind == bda::diff::DiffKind::ValueMismatch)
      std::cout << "    A = " << d->valueA << "\n    B = " << d->valueB << "\n";
  }

  std::size_t differingSections = 0;
  for (const auto &section : result.sections)
    if (section.differs())
      ++differingSections;
  std::cout << "\n" << result.totalDiffs << " differences across " << differingSections << " of "
            << result.sections.size() << " OBUs"
            << (reportAll ? "" : ", showing the first " + std::to_string(maxReported)) << ":\n";

  std::size_t shown = 0;
  for (const auto &section : result.sections)
  {
    if (!section.differs())
      continue;
    if (!reportAll && shown >= maxReported)
      break;
    std::cout << "  " << section.label;
    if (section.onlyInA)      std::cout << "  [only in A]\n";
    else if (section.onlyInB) std::cout << "  [only in B]\n";
    else
    {
      std::cout << "  (" << section.result.diffs.size() << " differences)\n";
      for (const auto &d : section.result.diffs)
      {
        if (!reportAll && shown++ >= maxReported)
          break;
        std::cout << "      " << d.name << "  [" << kindName(d.kind) << "]";
        if (d.kind == bda::diff::DiffKind::ValueMismatch)
          std::cout << "  A=" << d.valueA << "  B=" << d.valueB;
        else if (d.kind == bda::diff::DiffKind::OnlyInA)
          std::cout << "  A=" << d.valueA;
        else
          std::cout << "  B=" << d.valueB;
        std::cout << "\n";
      }
    }
  }
  return 0;
}
