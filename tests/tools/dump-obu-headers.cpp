// Dump an AV1 stream's parsed OBU syntax in VQ Analyzer's plain-text header format.
//
// Not a test - a comparison tool. bd_analyzer and VQ Analyzer read the same bitstream, so their
// parses should agree field for field, and the cheapest way to see that is to put them in the
// same shape and diff. VQ Analyzer writes:
//
//   -------OBU_SEQUENCE_HEADER-------
//   seq_profile                                        = 0
//
// - a section rule per OBU, then `name`, left justified in a 51 character field, `= value`.
//
// Usage:  dump-obu-headers <stream> [out.txt]
//
// Build (from the repository root, after ./scripts/build.sh):
//   scl enable gcc-toolset-13 -- g++ -std=gnu++2a -fPIC -O1 \
//       -Ithird_party/yuview/upstream/YUViewLib/src -Ibuild/YUViewLib -Isrc \
//       -I$QT/include -I$QT/include/QtCore ... tests/tools/dump-obu-headers.cpp \
//       -o dump-obu-headers -Lbuild/YUViewLib -lYUViewLib <the Qt6 .so list> -lpthread -lGL
#include <QAbstractItemModel>
#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"

namespace
{

//!< VQ Analyzer's column: the name padded out to 51 characters, then "= value".
constexpr int kNameWidth = 51;

/* Walk the parser's item model and write every leaf that carries a value.
 *
 * An OBU node becomes a section rule; everything under it is a field. The model nests further than
 * VQ Analyzer's flat dump (a frame header has sub-structures), so nesting is flattened - the field
 * names are unique enough to compare on, and a hierarchy VQ Analyzer does not print would only
 * make the diff noisy.
 */
void writeNode(const QAbstractItemModel &model,
               const QModelIndex &     index,
               QTextStream &           out,
               bool                    showBits)
{
  const auto name  = model.data(model.index(index.row(), 0, index.parent())).toString();
  const auto value = model.data(model.index(index.row(), 1, index.parent())).toString();
  // Columns 2 and 3 are the reader's own record of how it read the field: the coding, and the
  // exact bits it consumed. VQ Analyzer prints neither, but when two parsers disagree on a value
  // this is what says which of them is at the wrong bit offset.
  const auto coding = model.data(model.index(index.row(), 2, index.parent())).toString();
  const auto code   = model.data(model.index(index.row(), 3, index.parent())).toString();

  const bool isObu = name.startsWith("OBU");
  if (isObu)
  {
    out << "-------" << name << "-------\n";
  }
  // The packet's raw_byte[] rows are a hexdump, not syntax - VQ Analyzer does not print them, and
  // their values embed control characters that make the whole dump read as a binary file.
  else if (!value.isEmpty() && !name.startsWith("raw_byte"))
  {
  {
    out << name.leftJustified(kNameWidth, ' ') << "= " << value;
    if (showBits && !code.isEmpty())
      out << "   [" << coding << " " << code << "]";
    out << "\n";
  }
  }

  for (int row = 0; row < model.rowCount(index); ++row)
    writeNode(model, model.index(row, 0, index), out, showBits);
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  std::vector<std::string> args;
  bool                     showBits = false;
  for (int i = 1; i < argc; ++i)
  {
    if (std::string(argv[i]) == "--bits")
      showBits = true;
    else
      args.push_back(argv[i]);
  }
  if (args.empty())
  {
    std::cerr << "usage: " << argv[0] << " [--bits] <stream> [out.txt]" << std::endl;
    return 2;
  }

  FFmpeg::FFmpegVersionHandler ff;
  ff.loadFFmpegLibraries();
  if (!ff.loadingSuccessfull())
  {
    std::cerr << "ffmpeg libraries did not load" << std::endl;
    return 1;
  }

  parser::ParserAVFormat parser;
  parser.enableModel();
  if (!parser.runParsingOfFile(std::filesystem::path(args[0])))
  {
    std::cerr << "could not parse " << args[0] << std::endl;
    return 1;
  }
  parser.updateNumberModelItems();

  auto *model = parser.getPacketItemModel();
  if (model == nullptr)
  {
    std::cerr << "the parser produced no model" << std::endl;
    return 1;
  }

  QFile file(args.size() >= 2 ? QString::fromStdString(args[1]) : QString("bda-headers.txt"));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
  {
    std::cerr << "could not write " << file.fileName().toStdString() << std::endl;
    return 1;
  }
  QTextStream out(&file);

  for (int row = 0; row < model->rowCount(); ++row)
    writeNode(*model, model->index(row, 0), out, showBits);

  out.flush();
  std::cout << "wrote " << file.fileName().toStdString() << std::endl;
  return 0;
}
