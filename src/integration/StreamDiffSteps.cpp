#include "StreamDiffSteps.h"

#include <QAbstractItemModel>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <thread>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"
#include "playlistitem/playlistItemCompressedVideo.h"
#include "video/yuv/videoHandlerYUV.h"

namespace bda::integration
{

namespace
{

constexpr int MiSize = 4; // The smallest AV1 block, and the unit MI positions are counted in.

/* Values dav1d reports on every block of a superblock although they belong to the superblock.
 *
 * Left in the per-block comparison they drown the report: one differing block changes its
 * superblock's bit count, and then every block of that superblock is flagged as differing too.
 * Measured on WorldCup 512x288 frame 1, that turned 1 real finding in SB(0, 1) into 4.
 */
const std::vector<std::string> SuperblockElements = {"sb_bitcount", "sb_qindex"};

bool cancelled(const std::atomic<bool> *flag) { return flag != nullptr && flag->load(); }

void report(const StepProgress &progress, const std::string &text)
{
  if (progress)
    progress(text);
}

/* loadFrame() asserts it is not running on the GUI thread - it is written for the application's
 * loader threads. A release build drops the assert but not the contract. The CLI calls this from
 * its main thread, so the hop is needed there; from the window's worker it costs a thread start.
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
 * uncompressed_header), and the exact nesting is the parser's business, not this one's.
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

/* The planar layout the decoder hands out, or an error when it is something this cannot read.
 *
 * Chroma plane sizes truncate (w / 2, not (w + 1) / 2) because that is how upstream sizes the raw
 * buffer (PixelFormatYUV::bytesPerFrame); rounding up here would read past the end of it.
 */
bool layoutFromFormat(const video::yuv::PixelFormatYUV &format,
                      const Size &                      size,
                      bda::diff::FrameLayout &          layout,
                      std::string &                     error)
{
  if (!format.isPlanar() || format.isUVInterleaved() || format.isBytePacking())
  {
    error = "the decoder output is not planar YUV (" + format.getName() + ")";
    return false;
  }
  if (format.getPlaneOrder() != video::yuv::PlaneOrder::YUV)
  {
    error = "the decoder output plane order is not Y, U, V (" + format.getName() + ")";
    return false;
  }
  const auto w = int(size.width);
  const auto h = int(size.height);
  layout.planes   = {{"Y", w, h}};
  layout.bitDepth = format.getBitsPerSample();
  layout.bigEndian = format.isBigEndian();
  if (format.getSubsampling() != video::yuv::Subsampling::YUV_400)
  {
    const auto cw = w / format.getSubsamplingHor();
    const auto ch = h / format.getSubsamplingVer();
    layout.planes.push_back({"U", cw, ch});
    layout.planes.push_back({"V", cw, ch});
  }
  return true;
}

/* One decoded frame's raw samples, read through the item's own handler. */
QByteArray decodedFrame(playlistItemCompressedVideo &item, const int frameIdx, std::string &error)
{
  loadFrameOffMainThread(item, frameIdx);
  auto *yuv = dynamic_cast<video::yuv::videoHandlerYUV *>(item.getFrameHandler());
  if (yuv == nullptr)
  {
    error = "the decoder does not output YUV";
    return {};
  }
  int  loaded = -1;
  auto raw    = yuv->getCurrentRawYUVData(loaded);
  // A buffer for another frame would compare cleanly and say nothing true. Refuse it.
  if (loaded != frameIdx)
  {
    error = "the decoder did not deliver frame " + std::to_string(frameIdx) + " (it holds " +
            std::to_string(loaded) + ")";
    return {};
  }
  return raw;
}

} // namespace

DisplayMap buildDisplayMap(const QAbstractItemModel &model)
{
  DisplayMap   displayOf;
  CodedFrameId refSlots[8];
  // (-1, -1) marks a slot nothing has been written into yet.
  for (int i = 0; i < 8; ++i)
    refSlots[i] = {-1, -1};

  int display = 0;
  for (int row = 0; row < model.rowCount(); ++row)
  {
    const auto packetIdx = model.index(row, 0);

    /* Key on the packet index the parser logged, not on the row number. They differ by one on this
     * model, and the number step 3 reports is the packet, so keying on the row would look up the
     * temporal unit next door - which holds a different set of frames.
     */
    int packet = -1;
    for (int child = 0; child < model.rowCount(packetIdx); ++child)
      if (model.data(model.index(child, 0, packetIdx)).toString() == "Global AVPacket Count")
      {
        packet = model.data(model.index(child, 1, packetIdx)).toString().toInt();
        break;
      }
    if (packet < 0)
      continue;

    int obuIndex = -1;
    for (int child = 0; child < model.rowCount(packetIdx); ++child)
    {
      const auto childIdx = model.index(child, 0, packetIdx);
      if (!model.data(childIdx).toString().startsWith("OBU"))
        continue;
      ++obuIndex; // Counts from the temporal delimiter, as step 3 does.

      const auto showExisting = findValue(model, childIdx, "show_existing_frame");
      if (!showExisting)
        continue; // Not a frame OBU.

      const CodedFrameId id{packet, obuIndex};
      if (*showExisting == 1)
      {
        const auto mapIdx = findValue(model, childIdx, "frame_to_show_map_idx").value_or(-1);
        if (mapIdx >= 0 && mapIdx < 8 && refSlots[mapIdx].first >= 0)
          displayOf[refSlots[mapIdx]] = display;
        ++display;
        continue;
      }

      if (findValue(model, childIdx, "show_frame").value_or(0) == 1)
        displayOf[id] = display++;

      const auto refresh = findValue(model, childIdx, "refresh_frame_flags").value_or(0);
      for (int slot = 0; slot < 8; ++slot)
        if ((refresh >> slot) & 1)
          refSlots[slot] = id;
    }
  }
  return displayOf;
}

int resolveDisplayIndex(const QString &path, const int tu, const int obu, std::string &error)
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
  if (!parser.runParsingOfFile(std::filesystem::path(path.toStdString())))
  {
    error = "could not parse " + path.toStdString();
    return -1;
  }
  parser.updateNumberModelItems();
  auto *model = parser.getPacketItemModel();
  if (model == nullptr)
  {
    error = "the parser produced no packet model";
    return -1;
  }

  const auto displayOf = buildDisplayMap(*model);
  const auto it        = displayOf.find({tu, obu});
  if (it == displayOf.end())
  {
    error = "TU " + std::to_string(tu) + " / OBU " + std::to_string(obu) +
            " is never displayed - it is not a frame, or nothing shows it";
    return -1;
  }
  return it->second;
}

const bda::diff::SbDiff *BlockStepResult::firstWithBlockDiff() const
{
  for (const auto &sb : this->differing)
    if (sb.hasBlockDiff())
      return &sb;
  return nullptr;
}

const bda::diff::SbDiff *BlockStepResult::firstTotalsOnlyBefore() const
{
  for (const auto &sb : this->differing)
  {
    if (sb.hasBlockDiff())
      return nullptr;
    return &sb;
  }
  return nullptr;
}

BlockStepResult runBlockStep(const QString &          pathA,
                             const QString &          pathB,
                             const int                frameIdx,
                             const BlockStepOptions &options)
{
  BlockStepResult out;
  out.frameIdx = frameIdx;

  playlistItemCompressedVideo itemA(pathA, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
  playlistItemCompressedVideo itemB(pathB, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
  itemA.setBlockInfoRequested(true);
  itemB.setBlockInfoRequested(true);

  const auto sizeA = itemA.getSize();
  const auto sizeB = itemB.getSize();
  if (sizeA.width() <= 0 || sizeB.width() <= 0)
  {
    out.error = "could not open both streams";
    return out;
  }
  if (sizeA != sizeB)
  {
    out.error = "the two streams have different picture sizes: " + std::to_string(sizeA.width()) +
                "x" + std::to_string(sizeA.height()) + " and " + std::to_string(sizeB.width()) +
                "x" + std::to_string(sizeB.height());
    return out;
  }
  out.width  = sizeA.width();
  out.height = sizeA.height();

  report(options.progress, "picture " + std::to_string(out.width) + "x" +
                               std::to_string(out.height) + ", frame " + std::to_string(frameIdx));
  report(options.progress, "decoding A ...");
  Loader a(itemA, frameIdx, sizeA);
  if (cancelled(options.cancel))
  {
    out.cancelled = true;
    return out;
  }
  report(options.progress, "decoding B ...");
  Loader b(itemB, frameIdx, sizeA);

  /* Superblock size is not exported, so it is probed from the largest block in the frame and can
   * be overridden. Getting it wrong only regroups the report - the comparison is per MI position
   * either way - but the SB coordinates would not match what the GUI shows, and the walk below
   * fills superblock by superblock, so the grid has to be settled before it starts.
   */
  out.sbSizeGiven = options.sbSize == 64 || options.sbSize == 128;
  if (out.sbSizeGiven)
    out.sbSize = options.sbSize;
  else
  {
    report(options.progress, "probing superblock size ...");
    out.sbSize = std::max(a.probeSuperblockSize(), b.probeSuperblockSize());
  }
  a.grid().sbSizeMi = b.grid().sbSizeMi = out.sbSize / MiSize;

  const auto sbRows = a.grid().sbRows();
  const auto sbCols = a.grid().sbCols();
  out.sbTotal       = static_cast<std::size_t>(sbRows) * static_cast<std::size_t>(sbCols);

  bda::diff::BlockDiffOptions diffOptions;
  diffOptions.superblockElements = SuperblockElements;

  for (int sbRow = 0; sbRow < sbRows && !out.stoppedEarly; ++sbRow)
    for (int sbCol = 0; sbCol < sbCols && !out.stoppedEarly; ++sbCol)
    {
      if (cancelled(options.cancel))
      {
        out.cancelled = true;
        return out;
      }
      a.fillSb(sbRow, sbCol);
      b.fillSb(sbRow, sbCol);
      ++out.scannedSbs;

      std::size_t compared = 0;
      auto sb = bda::diff::compareSuperblock(a.grid(), b.grid(), sbRow, sbCol, diffOptions,
                                             &compared);
      out.comparedPositions += compared;
      out.differingPositions += sb.differingPositions;
      if (!sb.differs())
        continue;

      const bool hasBlockDiff = sb.hasBlockDiff();
      out.differing.push_back(std::move(sb));
      // Keep going with options.all; otherwise the first real block difference is the answer.
      if (hasBlockDiff && !options.all)
        out.stoppedEarly = true;
    }

  const auto totalPositions = double(out.sbTotal) * a.grid().sbSizeMi * a.grid().sbSizeMi;
  out.queriesA  = a.queries();
  out.queriesB  = b.queries();
  out.coverageA = 100.0 * double(a.covered()) / totalPositions;
  out.coverageB = 100.0 * double(b.covered()) / totalPositions;
  out.staleA    = a.stale();
  out.staleB    = b.stale();
  return out;
}

int ReconStepResult::firstDiffering() const
{
  for (std::size_t i = 0; i < this->frames.size(); ++i)
    if (!this->frames[i].diff.identical())
      return int(i);
  return -1;
}

ReconStepResult runReconStep(const QString &          pathA,
                             const QString &          pathB,
                             const ReconStepOptions &options)
{
  ReconStepResult out;

  playlistItemCompressedVideo itemA(pathA, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
  playlistItemCompressedVideo itemB(pathB, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);

  const auto sizeA = itemA.getSize();
  const auto sizeB = itemB.getSize();
  if (sizeA.width() <= 0 || sizeB.width() <= 0)
  {
    out.error = "could not open both streams";
    return out;
  }
  if (sizeA != sizeB)
  {
    out.error = "the two streams have different picture sizes";
    return out;
  }

  const auto lastA = itemA.properties().startEndRange.second;
  const auto lastB = itemB.properties().startEndRange.second;
  out.frameCount   = std::min(lastA, lastB) + 1;
  if (out.frameCount <= 0)
  {
    out.error = "no decodable frames";
    return out;
  }

  for (int frame = 0; frame < out.frameCount; ++frame)
  {
    if (cancelled(options.cancel))
    {
      out.cancelled = true;
      return out;
    }
    report(options.progress,
           "frame " + std::to_string(frame + 1) + " of " + std::to_string(out.frameCount));

    std::string error;
    const auto  rawA = decodedFrame(itemA, frame, error);
    if (!error.empty())
    {
      out.error = "A: " + error;
      return out;
    }
    const auto rawB = decodedFrame(itemB, frame, error);
    if (!error.empty())
    {
      out.error = "B: " + error;
      return out;
    }

    // The format is only known once a frame has been decoded, so the layout is settled here.
    if (frame == 0)
    {
      auto *yuvA = dynamic_cast<video::yuv::videoHandlerYUV *>(itemA.getFrameHandler());
      auto *yuvB = dynamic_cast<video::yuv::videoHandlerYUV *>(itemB.getFrameHandler());
      if (yuvA->getPixelFormatYUV() != yuvB->getPixelFormatYUV())
      {
        out.error = "the two streams decode to different formats: " +
                    yuvA->getPixelFormatYUV().getName() + " and " +
                    yuvB->getPixelFormatYUV().getName();
        return out;
      }
      if (!layoutFromFormat(yuvA->getPixelFormatYUV(), yuvA->getFrameSize(), out.layout,
                            out.error))
        return out;
    }

    const auto need = out.layout.bytesPerFrame();
    if (std::size_t(rawA.size()) < need || std::size_t(rawB.size()) < need)
    {
      out.error = "frame " + std::to_string(frame) + " is shorter than its format says (" +
                  std::to_string(rawA.size()) + " / " + std::to_string(rawB.size()) + " of " +
                  std::to_string(need) + " bytes)";
      return out;
    }

    ReconFrame f;
    f.frameIdx = frame;
    f.diff     = bda::diff::compareFrames(reinterpret_cast<const std::uint8_t *>(rawA.constData()),
                                      reinterpret_cast<const std::uint8_t *>(rawB.constData()),
                                      out.layout);
    const bool differs = !f.diff.identical();
    out.frames.push_back(std::move(f));
    if (differs && options.stopAtFirst)
      break;
  }
  return out;
}

std::string formatPlanePsnr(const bda::diff::PlaneDiff &plane, const unsigned bitDepth)
{
  const auto p = bda::diff::psnr(plane, bitDepth);
  if (!p)
    return "identical";
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f dB", *p);
  return text;
}

} // namespace bda::integration
