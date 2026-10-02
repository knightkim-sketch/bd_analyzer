// Compare one decoded frame of two AV1 streams, block by block.
//
//   stream-diff-blocks <a> <b> --frame N [--sb 64|128] [--max-sb N] [--all]
//
// Step 4 of Find diff. Step 3 (stream-diff-payload) says which OBU payload first differs and at
// which byte; that byte is a position in the arithmetic-coded data and says nothing about where in
// the picture the divergence is. This decodes that one frame on both sides and reports the answer
// as coordinates: SB(row, col) and MI(row, col).
//
// Only the named frame is decoded. Decoding a 4K frame and querying every MI position costs
// seconds per stream, so sweeping a whole sequence here would be the wrong tool; step 3 has already
// narrowed it to one frame.
//
// Coordinate convention: SB and MI are (row, col). Pixel positions are (x, y).
#include <QApplication>
#include <QSettings>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <QAbstractItemModel>

#include <filesystem>
#include <map>
#include <optional>

#include "diff/BlockSyntaxDiff.h"
#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"
#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{

constexpr int MiSize = 4; // The smallest AV1 block, and the unit MI positions are counted in.

/* loadFrame() asserts it is not running on the GUI thread - it is written for the application's
 * loader threads. A release build drops the assert but not the contract.
 */
void loadFrameOffMainThread(playlistItemCompressedVideo &item, const int frameIdx)
{
  std::thread loader([&item, frameIdx] { item.loadFrame(frameIdx, false, true, false); });
  loader.join();
}

std::vector<bda::diff::SyntaxElement> toElements(const stats::BlockInfo &info)
{
  std::vector<bda::diff::SyntaxElement> out;
  out.reserve(info.entries.size());
  // Entries arrive deduplicated and ascending by typeID, so the order is stable between runs and
  // between streams - which is what lets the name alignment stay exact.
  for (const auto &e : info.entries)
    out.push_back({e.typeName.toStdString(), e.valueText.toStdString()});
  return out;
}

/* First value of a named syntax element anywhere under this model row.
 *
 * The frame header sits a few levels down inside the OBU (frame_obu > frame_header_obu >
 * uncompressed_header), and the exact nesting is the parser's business, not this tool's.
 */
std::optional<int> findValue(const QAbstractItemModel &model,
                             const QModelIndex &       index,
                             const QString &           name)
{
  for (int row = 0; row < model.rowCount(index); ++row)
  {
    const auto child = model.index(row, 0, index);
    if (model.data(child).toString() == name)
    {
      bool       ok    = false;
      const auto value = model.data(model.index(row, 1, index)).toString().toInt(&ok);
      if (ok)
        return value;
    }
    if (const auto found = findValue(model, child, name))
      return found;
  }
  return std::nullopt;
}

/* Turn the (temporal unit, OBU) that step 3 reports into the display frame index step 4 decodes.
 *
 * These are not the same number and must not be assumed to be. Step 3 names a *coded* frame; a
 * coded frame with show_frame = 0 is a hidden alternate reference that some later temporal unit
 * puts on screen with show_existing_frame. Measured on the stream this was written for: step 3
 * named TU 90 / OBU 2, order_hint 26, hidden - and display index 90 shows order_hint 25, a
 * different frame entirely. Comparing it answers with a superblock that has nothing to do with the
 * divergence.
 *
 * So the reference slots are simulated the way the decoder keeps them: a decoded frame is written
 * into every slot its refresh_frame_flags names, and show_existing_frame displays whatever is in
 * the slot it points at.
 */
int resolveDisplayIndex(const std::string &path, const int tu, const int obu, std::string &error)
{
  FFmpeg::FFmpegVersionHandler ff;
  ff.loadFFmpegLibraries();
  if (!ff.loadingSuccessfull())
  {
    error = "the FFmpeg libraries could not be loaded";
    return -1;
  }

  parser::ParserAVFormat parser;
  parser.enableModel();
  if (!parser.runParsingOfFile(std::filesystem::path(path)))
  {
    error = "could not parse " + path;
    return -1;
  }
  parser.updateNumberModelItems();
  auto *model = parser.getPacketItemModel();
  if (model == nullptr)
  {
    error = "the parser produced no packet model";
    return -1;
  }

  using FrameId = std::pair<int, int>; // (temporal unit, OBU index within it)
  std::map<FrameId, int> displayOf;
  FrameId                refSlots[8];
  // (-1, -1) marks a slot nothing has been written into yet.
  for (int i = 0; i < 8; ++i)
    refSlots[i] = {-1, -1};

  int display = 0;
  for (int row = 0; row < model->rowCount(); ++row)
  {
    const auto packetIdx = model->index(row, 0);

    /* Key on the packet index the parser logged, not on the row number. They differ by one on this
     * model, and the number step 3 reports is the packet, so keying on the row would look up the
     * temporal unit next door - which holds a different set of frames.
     */
    int packet = -1;
    for (int child = 0; child < model->rowCount(packetIdx); ++child)
      if (model->data(model->index(child, 0, packetIdx)).toString() == "Global AVPacket Count")
      {
        packet = model->data(model->index(child, 1, packetIdx)).toString().toInt();
        break;
      }
    if (packet < 0)
      continue;

    int obuIndex = -1;
    for (int child = 0; child < model->rowCount(packetIdx); ++child)
    {
      const auto childIdx = model->index(child, 0, packetIdx);
      if (!model->data(childIdx).toString().startsWith("OBU"))
        continue;
      ++obuIndex; // Counts from the temporal delimiter, as step 3 does.

      const auto showExisting = findValue(*model, childIdx, "show_existing_frame");
      if (!showExisting)
        continue; // Not a frame OBU.

      const FrameId id{packet, obuIndex};
      if (*showExisting == 1)
      {
        const auto mapIdx = findValue(*model, childIdx, "frame_to_show_map_idx").value_or(-1);
        if (mapIdx >= 0 && mapIdx < 8 && refSlots[mapIdx].first >= 0)
          displayOf[refSlots[mapIdx]] = display;
        ++display;
        continue;
      }

      if (findValue(*model, childIdx, "show_frame").value_or(0) == 1)
        displayOf[id] = display++;

      const auto refresh = findValue(*model, childIdx, "refresh_frame_flags").value_or(0);
      for (int slot = 0; slot < 8; ++slot)
        if ((refresh >> slot) & 1)
          refSlots[slot] = id;
    }
  }

  const auto it = displayOf.find({tu, obu});
  if (it == displayOf.end())
  {
    error = "TU " + std::to_string(tu) + " / OBU " + std::to_string(obu) +
            " is never displayed - it is not a frame, or nothing shows it";
    return -1;
  }
  return it->second;
}

/* Fills one superblock's MI positions on demand, one query per coding block.
 *
 * Two costs are being avoided here, and they are different.
 *
 * One query per block, not per MI position: getBlockInfoAt() is a linear scan over every value of
 * every statistics type (StatisticsData.cpp). It is written for a mouse click, where one scan is
 * free. The query already answers with the covering block's full rect, so every MI position inside
 * that rect is known from the one answer. Measured on a 4K frame: 518,400 queries became 68,389,
 * and the run went from 21m37s to 3m55s with byte-identical output.
 *
 * Only the superblocks actually reached: finding the *first* differing superblock needs every
 * superblock before it compared - that is what makes it first - but nothing after it. So the
 * caller walks in raster order and stops, and this fills only what it asks for.
 */
class Loader
{
public:
  Loader(playlistItemCompressedVideo &item, const int frameIdx, const QSize size)
      : item(item), frameIdx(frameIdx)
  {
    this->miCols = (size.width() + MiSize - 1) / MiSize;
    this->miRows = (size.height() + MiSize - 1) / MiSize;
    this->map.resize(this->miRows, this->miCols);
    loadFrameOffMainThread(this->item, this->frameIdx);
  }

  void fillSb(const int sbRow, const int sbCol)
  {
    const auto sbSize   = this->map.sbSizeMi;
    const auto miRowEnd = std::min(this->miRows, (sbRow + 1) * sbSize);
    const auto miColEnd = std::min(this->miCols, (sbCol + 1) * sbSize);
    for (int miRow = sbRow * sbSize; miRow < miRowEnd; ++miRow)
      for (int miCol = sbCol * sbSize; miCol < miColEnd; ++miCol)
        this->fillPosition(miRow, miCol);
  }

  bda::diff::BlockMap &grid() { return this->map; }
  long queries() const { return this->queryCount; }
  long covered() const { return this->coveredCount; }
  long stale() const { return this->staleCount; }
  int  maxBlockSide() const { return this->widestBlock; }

  /* Probe one point per 64x64 cell to learn the largest block in the frame, and from it the
   * superblock size. Cheap next to the real walk (2040 queries at 4K against 68,389), and it has
   * to happen before the walk because the superblock size decides the grid the walk reports in.
   */
  int probeSuperblockSize()
  {
    for (int miRow = 0; miRow < this->miRows; miRow += 16)
      for (int miCol = 0; miCol < this->miCols; miCol += 16)
        this->fillPosition(miRow, miCol);
    return (this->widestBlock > 64) ? 128 : 64;
  }

private:
  void fillPosition(const int miRow, const int miCol)
  {
    if (this->map.at(miRow, miCol).valid)
      return; // A block found earlier already covers this position.

    ++this->queryCount;
    const auto info = this->item.getBlockInfoAt(QPoint(miCol * MiSize, miRow * MiSize),
                                                this->frameIdx);
    if (!info.isValid)
      return;
    // Statistics can lag the displayed frame. A block from another frame compared against this one
    // would report a difference that is an artefact of the query, not of the stream.
    if (info.frameIndex != this->frameIdx)
    {
      ++this->staleCount;
      return;
    }

    const auto r      = info.codingBlockRect;
    this->widestBlock = std::max({this->widestBlock, r.width(), r.height()});

    bda::diff::BlockSyntax entry;
    entry.valid    = true;
    entry.x        = r.x();
    entry.y        = r.y();
    entry.w        = r.width();
    entry.h        = r.height();
    entry.elements = toElements(info);

    // Paint the block over its own MI positions. A position already filled keeps what it has: the
    // first answer for a position is the one that query gave.
    const auto rowFrom = std::max(0, r.y() / MiSize);
    const auto colFrom = std::max(0, r.x() / MiSize);
    const auto rowTo   = std::min(this->miRows, (r.y() + r.height() + MiSize - 1) / MiSize);
    const auto colTo   = std::min(this->miCols, (r.x() + r.width() + MiSize - 1) / MiSize);
    for (int rr = rowFrom; rr < rowTo; ++rr)
      for (int cc = colFrom; cc < colTo; ++cc)
        if (!this->map.at(rr, cc).valid)
        {
          this->map.at(rr, cc) = entry;
          ++this->coveredCount;
        }
  }

  playlistItemCompressedVideo &item;
  int                          frameIdx{};
  int                          miRows{}, miCols{};
  bda::diff::BlockMap          map;
  long                         queryCount{}, coveredCount{}, staleCount{};
  int                          widestBlock{};
};

/* Values dav1d reports on every block of a superblock although they belong to the superblock.
 *
 * Left in the per-block comparison they drown the report: one differing block changes its
 * superblock's bit count, and then every block of that superblock is flagged as differing too.
 * Measured on WorldCup 512x288 frame 1, that turned 1 real finding in SB(0, 1) into 4.
 */
const std::vector<std::string> SuperblockElements = {"sb_bitcount", "sb_qindex"};

void printBlock(const char *side, const bda::diff::BlockSyntax &b)
{
  if (!b.valid)
  {
    std::cout << "      " << side << ": no block" << std::endl;
    return;
  }
  std::cout << "      " << side << ": " << b.w << "x" << b.h << " at pixel (" << b.x << ", " << b.y
            << "), MI(" << (b.y / MiSize) << ", " << (b.x / MiSize) << ")" << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  std::string pathA, pathB;
  int         frameIdx = -1, sbSize = 0, maxSb = 5;
  int         tu = -1, obu = -1;
  bool        all = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--frame" && i + 1 < argc)
      frameIdx = std::stoi(argv[++i]);
    else if (a == "--tu" && i + 1 < argc)
      tu = std::stoi(argv[++i]);
    else if (a == "--obu" && i + 1 < argc)
      obu = std::stoi(argv[++i]);
    else if (a == "--sb" && i + 1 < argc)
      sbSize = std::stoi(argv[++i]);
    else if (a == "--max-sb" && i + 1 < argc)
      maxSb = std::stoi(argv[++i]);
    else if (a == "--all")
      all = true;
    else if (pathA.empty())
      pathA = a;
    else if (pathB.empty())
      pathB = a;
  }
  if (pathA.empty() || pathB.empty() || (frameIdx < 0 && (tu < 0 || obu < 0)))
  {
    std::cerr << "usage: stream-diff-blocks <a> <b> (--tu N --obu N | --frame N)\n"
                 "                          [--sb 64|128] [--max-sb N] [--all]\n"
                 "\n"
                 "  --tu N --obu N  the coded frame step 3 named; the display frame is worked out\n"
                 "  --frame N       a display frame index directly"
              << std::endl;
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerBlockDiff");
  QCoreApplication::setApplicationName("bdAnalyzerBlockDiff");
  QSettings().clear();

  /* --tu/--obu is the form that matches what step 3 reports. Resolving it here rather than asking
   * the reader to do it: the arithmetic looks easy (GOP start + order_hint) and is wrong whenever a
   * hidden frame is involved, which is exactly when this tool gets used.
   */
  if (frameIdx < 0)
  {
    std::string error;
    frameIdx = resolveDisplayIndex(pathA, tu, obu, error);
    if (frameIdx < 0)
    {
      std::cerr << "could not place TU " << tu << " / OBU " << obu << ": " << error << std::endl;
      return 2;
    }
    std::cout << "TU " << tu << " / OBU " << obu << " is displayed as frame " << frameIdx
              << std::endl;
  }

  playlistItemCompressedVideo itemA(QString::fromStdString(pathA), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
  playlistItemCompressedVideo itemB(QString::fromStdString(pathB), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
  itemA.setBlockInfoRequested(true);
  itemB.setBlockInfoRequested(true);

  const auto sizeA = itemA.getSize();
  const auto sizeB = itemB.getSize();
  if (sizeA.width() <= 0 || sizeB.width() <= 0)
  {
    std::cerr << "could not open both streams" << std::endl;
    return 2;
  }
  if (sizeA != sizeB)
  {
    std::cerr << "the two streams have different picture sizes: " << sizeA.width() << "x"
              << sizeA.height() << " and " << sizeB.width() << "x" << sizeB.height() << std::endl;
    return 2;
  }

  std::cerr << "picture " << sizeA.width() << "x" << sizeA.height() << ", frame " << frameIdx
            << std::endl;
  std::cerr << "decoding A ..." << std::endl;
  Loader a(itemA, frameIdx, sizeA);
  std::cerr << "decoding B ..." << std::endl;
  Loader b(itemB, frameIdx, sizeA);

  /* Superblock size is not exported, so it is probed from the largest block in the frame and can
   * be overridden. Getting it wrong only regroups the report - the comparison is per MI position
   * either way - but the SB coordinates would not match what the GUI shows, and the walk below
   * fills superblock by superblock, so the grid has to be settled before it starts.
   */
  int inferred = 64;
  if (sbSize == 64 || sbSize == 128)
    inferred = sbSize;
  else
  {
    std::cerr << "probing superblock size ..." << std::endl;
    inferred = std::max(a.probeSuperblockSize(), b.probeSuperblockSize());
  }
  a.grid().sbSizeMi = b.grid().sbSizeMi = inferred / MiSize;

  std::cout << "A  " << pathA << std::endl;
  std::cout << "B  " << pathB << std::endl;
  std::cout << "frame " << frameIdx << ", " << sizeA.width() << "x" << sizeA.height()
            << ", superblock " << inferred << "x" << inferred
            << (sbSize ? " (given)" : " (probed)") << std::endl;

  const auto sbRows = a.grid().sbRows();
  const auto sbCols = a.grid().sbCols();
  const auto sbTotal = static_cast<std::size_t>(sbRows) * static_cast<std::size_t>(sbCols);

  bda::diff::BlockDiffOptions diffOptions;
  diffOptions.superblockElements = SuperblockElements;

  /* Walk superblocks in raster order and stop at the first one whose blocks differ.
   *
   * Not a binary search over the superblock index. That needs "every superblock before the
   * boundary is identical and every one after it differs", and the second half is false: once the
   * arithmetic decoder has diverged most superblocks differ, but not all. Measured on this 4K pair
   * at frame 17, 86 of 2040 superblocks are identical while the first difference is at SB(0, 30) -
   * so 56 identical superblocks lie *after* the divergence. A probe landing on one of them reads
   * as "still identical here" and sends the search past the answer, silently.
   *
   * A left-to-right scan that stops on the first hit is also strictly less work than a binary
   * search would be: proving a superblock is the *first* to differ means comparing every one
   * before it, which is exactly what this does and no more.
   */
  std::vector<bda::diff::SbDiff> differing;
  std::size_t                    comparedPositions = 0, differingPositions = 0;
  std::size_t                    scannedSbs = 0;
  const bda::diff::SbDiff *      firstTotalsOnly = nullptr;
  bool                           stoppedEarly    = false;

  for (int sbRow = 0; sbRow < sbRows && !stoppedEarly; ++sbRow)
    for (int sbCol = 0; sbCol < sbCols && !stoppedEarly; ++sbCol)
    {
      a.fillSb(sbRow, sbCol);
      b.fillSb(sbRow, sbCol);
      ++scannedSbs;

      std::size_t compared = 0;
      auto sb = bda::diff::compareSuperblock(a.grid(), b.grid(), sbRow, sbCol, diffOptions,
                                             &compared);
      comparedPositions += compared;
      differingPositions += sb.differingPositions;
      if (!sb.differs())
        continue;

      const bool hasBlockDiff = sb.hasBlockDiff();
      differing.push_back(std::move(sb));
      if (firstTotalsOnly == nullptr && !hasBlockDiff)
        firstTotalsOnly = &differing.back();
      // Keep going in --all; otherwise the first real block difference is the answer.
      if (hasBlockDiff && !all)
        stoppedEarly = true;
    }

  const auto totalPositions = double(sbTotal) * a.grid().sbSizeMi * a.grid().sbSizeMi;
  std::cout << std::fixed << std::setprecision(1) << "superblocks scanned: " << scannedSbs << " of "
            << sbTotal << (stoppedEarly ? " (stopped at the first difference)" : "") << std::endl;
  std::cout << "decoder queries: A " << a.queries() << ", B " << b.queries()
            << " (one per coding block, not one per MI position)" << std::endl;
  const auto coverageA = 100.0 * double(a.covered()) / totalPositions;
  const auto coverageB = 100.0 * double(b.covered()) / totalPositions;
  std::cout << "MI positions covered, of the whole frame: A " << coverageA << "%, B " << coverageB
            << "%" << (stoppedEarly ? " (only the scanned part was loaded)" : "") << std::endl;
  if (a.stale() > 0 || b.stale() > 0)
    std::cout << "  " << a.stale() << " / " << b.stale()
              << " queries answered for another frame and were dropped" << std::endl;
  if (!stoppedEarly && (coverageA < 95.0 || coverageB < 95.0))
    std::cout << "  WARNING coverage is low - differences below may be missing data rather than a "
                 "real divergence."
              << std::endl;

  if (differing.empty())
  {
    std::cout << "\nEvery block in frame " << frameIdx << " is identical ("
              << comparedPositions << " MI positions compared)." << std::endl;
    std::cout << "If step 3 pointed at this frame, the divergence is in syntax the decoder does "
                 "not export per block - check the frame header comparison."
              << std::endl;
    _exit(0);
  }

  const bda::diff::SbDiff *firstBlock = nullptr;
  for (const auto &sb : differing)
    if (sb.hasBlockDiff())
    {
      firstBlock = &sb;
      break;
    }

  if (firstBlock != nullptr)
  {
    const auto &blk = firstBlock->blocks.front();
    std::cout << "\nfirst superblock with a differing block: SB(" << firstBlock->sbRow << ", "
              << firstBlock->sbCol << ")" << std::endl;
    std::cout << "  first differing block:                MI(" << blk.miRow << ", " << blk.miCol
              << ")  [" << bda::diff::blockDiffKindName(blk.kind) << "]" << std::endl;
  }
  else
    std::cout << "\nNo block syntax differs in the part of the frame that was scanned."
              << std::endl;

  /* The two answers are not always the same superblock, and the difference is the useful part: a
   * superblock whose bit count differs while every exported block value matches coded the same
   * decisions with a different residual - real, but not where the decision parted ways.
   */
  if (firstTotalsOnly != nullptr && firstTotalsOnly != firstBlock)
    std::cout << "  earliest superblock differing at all:  SB(" << firstTotalsOnly->sbRow << ", "
              << firstTotalsOnly->sbCol << ")  (superblock totals only, no block syntax differs "
                 "there)"
              << std::endl;

  std::cout << "\n" << differing.size() << " differing superblocks in the " << scannedSbs
            << " scanned, " << differingPositions << " of " << comparedPositions
            << " MI positions." << std::endl;
  if (stoppedEarly)
    std::cout << "Pass --all to scan the whole frame and count them all." << std::endl;

  const auto &result_sbs = differing;
  const auto limit = all ? result_sbs.size() : std::min<std::size_t>(result_sbs.size(), maxSb);
  for (std::size_t i = 0; i < limit; ++i)
  {
    const auto &sb = result_sbs[i];
    std::cout << "\nSB(" << sb.sbRow << ", " << sb.sbCol << ")  " << sb.blocks.size()
              << " differing blocks, " << sb.differingPositions << " MI positions" << std::endl;
    for (const auto &d : sb.superblock.diffs)
      std::cout << "  [superblock] " << d.name << ": A " << d.valueA << "  |  B " << d.valueB
                << std::endl;
    for (const auto &blk : sb.blocks)
    {
      std::cout << "  MI(" << blk.miRow << ", " << blk.miCol << ")  "
                << bda::diff::blockDiffKindName(blk.kind) << ", covers " << blk.positions
                << " MI positions" << std::endl;
      if (blk.kind != bda::diff::BlockDiffKind::SyntaxDiffers)
      {
        printBlock("A", blk.a);
        printBlock("B", blk.b);
      }
      for (const auto &d : blk.syntax.diffs)
      {
        std::cout << "      " << d.name << ": ";
        switch (d.kind)
        {
        case bda::diff::DiffKind::ValueMismatch:
          std::cout << "A " << d.valueA << "  |  B " << d.valueB;
          break;
        case bda::diff::DiffKind::OnlyInA: std::cout << "only in A (" << d.valueA << ")"; break;
        case bda::diff::DiffKind::OnlyInB: std::cout << "only in B (" << d.valueB << ")"; break;
        }
        std::cout << std::endl;
      }
    }
  }
  if (limit < result_sbs.size())
    std::cout << "\n... " << (result_sbs.size() - limit)
              << " more differing superblocks; pass --all to see them." << std::endl;

  std::cout.flush();
  // The decoder keeps threads and dlopened libraries alive; Qt teardown after that is not worth
  // the risk in a batch tool whose output is already written.
  _exit(0);
}
