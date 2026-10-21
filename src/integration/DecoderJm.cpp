#include "DecoderJm.h"

#include <statistics/StatisticsData.h>
#include <statistics/StatisticsType.h>

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <algorithm>
#include <cstring>

namespace bda::integration
{

namespace
{

constexpr std::uint8_t kStartCode[4] = {0, 0, 0, 1};

// A prediction block of a macroblock, in luma samples relative to the macroblock.
struct PredBlock
{
  int x, y, w, h, pred; //!< pred: BDA_JM_PRED_*, -1 intra
};

std::vector<PredBlock> predictionBlocks(const bda_jm_mb &mb)
{
  if (mb.is_intra || mb.mb_part_w == 0)
    return {{0, 0, 16, 16, BDA_JM_PRED_INTRA}};
  if (mb.mb_part_w == 16 && mb.mb_part_h == 16)
    return {{0, 0, 16, 16, mb.part_pred[0]}};
  if (mb.mb_part_w == 16 && mb.mb_part_h == 8)
    return {{0, 0, 16, 8, mb.part_pred[0]}, {0, 8, 16, 8, mb.part_pred[2]}};
  if (mb.mb_part_w == 8 && mb.mb_part_h == 16)
    return {{0, 0, 8, 16, mb.part_pred[0]}, {8, 0, 8, 16, mb.part_pred[1]}};
  // 8x8 partitions, each split into its sub-macroblock partitions.
  std::vector<PredBlock> blocks;
  for (int b8 = 0; b8 < 4; ++b8)
  {
    const int w = std::clamp(int(mb.part_w[b8]), 4, 8), h = std::clamp(int(mb.part_h[b8]), 4, 8);
    for (int y = 0; y < 8; y += h)
      for (int x = 0; x < 8; x += w)
        blocks.push_back({(b8 % 2) * 8 + x, (b8 / 2) * 8 + y, w, h, mb.part_pred[b8]});
  }
  return blocks;
}

// Direction the intra prediction copies from, for the overlay arrow (luma 4x4 / 8x8 modes 0..8).
std::optional<std::pair<int, int>> intraDirection(int mode)
{
  switch (mode)
  {
  case 0: // vertical
    return std::pair{0, -4};
  case 1: // horizontal
    return std::pair{-4, 0};
  case 3: // diagonal down left
    return std::pair{4, -4};
  case 4: // diagonal down right
    return std::pair{-4, -4};
  case 5: // vertical right
    return std::pair{-2, -4};
  case 6: // horizontal down
    return std::pair{-4, -2};
  case 7: // vertical left
    return std::pair{2, -4};
  case 8: // horizontal up
    return std::pair{-4, 2};
  default: // DC
    return std::nullopt;
  }
}

} // namespace

DecoderJm::DecoderJm(bool cachingDecoder) : decoderBase(cachingDecoder)
{
  // The base decoders' search order (decoderBaseSingleLib::loadDecoderLibrary).
  QSettings settings;
  settings.beginGroup("Decoders");
  auto searchPath = settings.value("SearchPath", "").toString();
  settings.endGroup();
  if (!searchPath.endsWith("/"))
    searchPath.append("/");
  bool loaded = false;
  for (const auto &dir : {searchPath, QDir::currentPath() + "/", QDir::currentPath() + "/decoder/",
                          QCoreApplication::applicationDirPath() + "/",
                          QCoreApplication::applicationDirPath() + "/decoder/", QString()})
  {
    this->library.setFileName(dir + "libbda-jm-analyzer.so");
    if ((loaded = this->library.load()))
      break;
  }
  if (!loaded)
  {
    this->setError("Could not load libbda-jm-analyzer.so (scripts/setup-jm-analyzer.sh): " +
                   this->library.errorString());
    return;
  }
  auto resolve = [this](auto &fn, const char *symbol) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(this->library.resolve(symbol));
    if (!fn && !this->errorInDecoder())
      this->setError(QString("%1 has no %2").arg(this->library.fileName(), symbol));
  };
  resolve(this->apiVersion, "bda_jm_api_version");
  resolve(this->versionString, "bda_jm_version_string");
  resolve(this->createFn, "bda_jm_create");
  resolve(this->destroyFn, "bda_jm_destroy");
  resolve(this->pushFn, "bda_jm_push");
  resolve(this->pullFn, "bda_jm_pull");
  resolve(this->lastError, "bda_jm_last_error");
  if (this->errorInDecoder())
    return;
  // The structs are shared with the library: a different layout would be read silently wrong.
  if (this->apiVersion() != BDA_JM_API_VERSION)
  {
    this->setError(QString("%1 has API version %2, bd-analyzer needs %3")
                       .arg(this->library.fileName())
                       .arg(this->apiVersion())
                       .arg(BDA_JM_API_VERSION));
    return;
  }
  this->internalsSupported = true;
  this->maxBlockSize       = 16;
  this->rawFormat          = video::RawFormat::YUV;
  this->createHandle();
}

DecoderJm::~DecoderJm()
{
  if (this->handle)
    this->destroyFn(this->handle);
}

bool DecoderJm::createHandle()
{
  if (this->handle)
    this->destroyFn(this->handle);
  this->handle = this->createFn();
  if (!this->handle)
    this->setError("bda_jm_create failed");
  return this->handle != nullptr;
}

void DecoderJm::resetDecoder()
{
  if (!this->createFn)
    return;
  decoderBase::resetDecoder();
  this->pushedBytes = 0;
  this->segments.clear();
  this->nextPosition.reset();
  this->pending.reset();
  this->statisticsAdded = false;
  this->createHandle();
}

void DecoderJm::setNextDataOffsetInPacket(int offset)
{
  this->nextPosition = std::pair<std::int64_t, bool>{offset, false};
}

void DecoderJm::setNextDataFilePosition(std::int64_t position)
{
  this->nextPosition = std::pair<std::int64_t, bool>{position, true};
}

bool DecoderJm::pushData(QByteArray &data)
{
  if (this->decoderState != decoder::DecoderState::NeedsMoreData || !this->handle)
    return false;
  if (data.isEmpty())
  {
    this->pushFn(this->handle, nullptr, 0); // end of stream: the remaining pictures flush out
  }
  else
  {
    // Units come with or without their start code (and from a container without any); JM reads
    // Annex-B. The position the item named is the unit's first byte, start code included.
    const auto *bytes   = reinterpret_cast<const std::uint8_t *>(data.constData());
    std::size_t prefix  = 0;
    if (data.size() >= 4 && std::memcmp(bytes, kStartCode, 4) == 0)
      prefix = 4;
    else if (data.size() >= 3 && std::memcmp(bytes, kStartCode + 1, 3) == 0)
      prefix = 3;
    else
      this->pushFn(this->handle, kStartCode, 4);
    const auto headerPushed = this->pushedBytes + (prefix ? std::int64_t(prefix) : 4);
    if (this->nextPosition)
      this->segments.push_back({headerPushed, this->nextPosition->first + std::int64_t(prefix),
                                std::int64_t(data.size()) - std::int64_t(prefix), this->nextPosition->second});
    this->nextPosition.reset();
    this->pushFn(this->handle, bytes, std::size_t(data.size()));
    this->pushedBytes += (prefix ? 0 : 4) + data.size();
  }
  this->pullAhead();
  return true;
}

void DecoderJm::pullAhead()
{
  if (this->pending || this->errorInDecoder())
    return;
  bda_jm_picture picture{};
  const auto     rc = this->pullFn(this->handle, &picture);
  if (rc == BDA_JM_OK)
  {
    this->takePicture(picture);
    this->decoderState = decoder::DecoderState::RetrieveFrames;
  }
  else if (rc == BDA_JM_NEED_DATA)
    this->decoderState = decoder::DecoderState::NeedsMoreData;
  else if (rc == BDA_JM_EOS)
    this->decoderState = decoder::DecoderState::EndOfBitstream;
  else
    this->setError(QString("JM decoding failed (%1): %2").arg(rc).arg(QString::fromUtf8(this->lastError(this->handle))));
}

void DecoderJm::takePicture(const bda_jm_picture &picture)
{
  Picture p;
  const auto &planes = picture.planes;
  p.width            = planes.width;
  p.height           = planes.height;
  p.cropLeft         = picture.crop_left;
  p.cropTop          = picture.crop_top;
  const int nPlanes  = planes.chroma_format == BDA_JM_CHROMA_400 ? 1 : 3;
  qsizetype total    = 0;
  for (int i = 0; i < nPlanes; ++i)
    total += qsizetype(planes.plane_width[i]) * planes.plane_height[i] * planes.bytes_per_sample;
  p.yuv.resize(total);
  auto *out = reinterpret_cast<std::uint8_t *>(p.yuv.data());
  for (int i = 0; i < nPlanes; ++i)
  {
    const auto rowBytes = std::size_t(planes.plane_width[i]) * std::size_t(planes.bytes_per_sample);
    for (int y = 0; y < planes.plane_height[i]; ++y, out += rowBytes)
      std::memcpy(out, static_cast<const std::uint8_t *>(planes.data[i]) + std::ptrdiff_t(y) * planes.stride[i], rowBytes);
  }
  if (!this->frameSize.isValid())
  {
    using video::yuv::Subsampling;
    static const Subsampling kSubsampling[] = {Subsampling::YUV_400, Subsampling::YUV_420,
                                               Subsampling::YUV_422, Subsampling::YUV_444};
    this->frameSize = Size(unsigned(planes.width), unsigned(planes.height));
    this->formatYUV = video::yuv::PixelFormatYUV(kSubsampling[std::clamp(planes.chroma_format, 0, 3)],
                                                 unsigned(planes.bit_depth_luma));
  }
  p.hasMbData = picture.has_mb_data != 0;
  p.mbWidth   = picture.mb_width;
  p.mbHeight  = picture.mb_height;
  if (p.hasMbData)
  {
    p.mbs.assign(picture.mbs, picture.mbs + std::size_t(picture.mb_width) * std::size_t(picture.mb_height));
    p.blocks.assign(picture.blocks, picture.blocks + std::size_t(picture.mb_width) * 16 * std::size_t(picture.mb_height));
  }
  p.slices.assign(picture.slices, picture.slices + std::max(0, picture.num_slices));
  p.outputFrame = this->nextOutputFrame++;
  p.decodeIndex = picture.decode_index;
  if (picture.decode_index >= 0)
    this->codingIndexByFrame[p.outputFrame] = int(this->codingBase + picture.decode_index);
  this->pending = std::move(p);
}

bool DecoderJm::decodeNextFrame()
{
  if (!this->pending)
    return false;
  this->current = std::move(*this->pending);
  this->pending.reset();
  this->statisticsAdded = false;
  this->decoderState    = decoder::DecoderState::NeedsMoreData;
  this->pullAhead();
  return true;
}

QByteArray DecoderJm::getRawFrameData()
{
  if (this->statisticsEnabled() && !this->statisticsAdded)
    this->addStatistics();
  return this->current.yuv;
}

std::optional<int> DecoderJm::codingIndexOfFrame(int frameIdx) const
{
  const auto it = this->codingIndexByFrame.find(frameIdx);
  if (it == this->codingIndexByFrame.end())
    return std::nullopt;
  return it->second;
}

std::optional<std::pair<std::int64_t, bool>> DecoderJm::nalPosition(std::int64_t pushedOffset) const
{
  // The last segment that starts at or before the NAL unit header.
  const auto it = std::upper_bound(this->segments.begin(), this->segments.end(), pushedOffset,
                                   [](std::int64_t v, const Segment &s) { return v < s.pushed; });
  if (it == this->segments.begin())
    return std::nullopt;
  const auto &segment = *std::prev(it);
  // A unit pushed without a position (parameter sets after a seek) has no segment of its own.
  if (pushedOffset - segment.pushed >= segment.length)
    return std::nullopt;
  return std::pair{segment.position + (pushedOffset - segment.pushed), segment.inFile};
}

void DecoderJm::addStatistics()
{
  this->statisticsAdded = true;
  const auto &p         = this->current;
  if (!p.hasMbData)
    return;
  auto     *stats = this->statisticsData;
  const int W = p.width, H = p.height;
  const int bw = p.mbWidth * 4;
  auto add = [&](int typeId, int x, int y, int w, int h, int value) {
    x -= p.cropLeft;
    y -= p.cropTop;
    if (x >= W || y >= H || x + w <= 0 || y + h <= 0)
      return;
    (*stats)[typeId].addBlockValue(std::uint16_t(std::max(x, 0)), std::uint16_t(std::max(y, 0)),
                                   std::uint16_t(w), std::uint16_t(h), value);
  };
  auto addVector = [&](int typeId, int x, int y, int w, int h, int vx, int vy) {
    x -= p.cropLeft;
    y -= p.cropTop;
    if (x >= W || y >= H || x + w <= 0 || y + h <= 0)
      return;
    (*stats)[typeId].addBlockVector(std::uint16_t(std::max(x, 0)), std::uint16_t(std::max(y, 0)),
                                    std::uint16_t(w), std::uint16_t(h), vx, vy);
  };

  for (const auto &mb : p.mbs)
  {
    if (mb.kind == BDA_JM_MB_UNKNOWN)
      continue;
    const int mx = mb.mb_x * 16, my = mb.mb_y * 16;
    add(kJmMbType, mx, my, 16, 16, mb.kind);
    add(kJmSkip, mx, my, 16, 16, mb.skip);
    add(kJmTransform8x8, mx, my, 16, 16, mb.transform_8x8);
    add(kJmQp, mx, my, 16, 16, mb.qp);
    add(kJmCbp, mx, my, 16, 16, mb.cbp);
    add(kJmSlice, mx, my, 16, 16, mb.slice_index);
    add(kJmMbBits, mx, my, 16, 16, int(std::max<std::int64_t>(0, mb.bit_end - mb.bit_start)));

    if (mb.slice_index >= 0 && std::size_t(mb.slice_index) < p.slices.size() && mb.bit_end > mb.bit_start)
      if (const auto position = nalPosition(p.slices[std::size_t(mb.slice_index)].nal_offset))
      {
        stats::BitstreamRange range;
        range.startBit = std::uint64_t(position->first * 8 + mb.bit_start);
        range.endBit   = std::uint64_t(position->first * 8 + mb.bit_end);
        range.origin   = position->second ? "source file" : "packet payload";
        stats->addBlockBitstreamRange(std::uint16_t(std::max(0, mx - p.cropLeft)),
                                      std::uint16_t(std::max(0, my - p.cropTop)), 16, 16, range);
      }

    if (mb.is_intra)
    {
      add(kJmPredMode, mx, my, 16, 16, 0);
      if (mb.intra_chroma_mode >= 0)
        add(kJmIntraChroma, mx, my, 16, 16, mb.intra_chroma_mode);
      if (mb.kind == BDA_JM_MB_I16X16)
        add(kJmIntraLuma, mx, my, 16, 16, 20 + mb.intra16x16_mode);
      else if (mb.kind == BDA_JM_MB_I8X8)
        for (int b = 0; b < 4; ++b)
        {
          const int x = mx + (b % 2) * 8, y = my + (b / 2) * 8;
          add(kJmIntraLuma, x, y, 8, 8, 10 + mb.intra8x8_mode[b]);
          if (const auto d = intraDirection(mb.intra8x8_mode[b]))
            addVector(kJmIntraLuma, x, y, 8, 8, d->first, d->second);
        }
      else if (mb.kind == BDA_JM_MB_I4X4)
        for (int b = 0; b < 16; ++b)
        {
          const int x = mx + (b % 4) * 4, y = my + (b / 4) * 4;
          add(kJmIntraLuma, x, y, 4, 4, mb.intra4x4_mode[b]);
          if (const auto d = intraDirection(mb.intra4x4_mode[b]))
            addVector(kJmIntraLuma, x, y, 4, 4, d->first, d->second);
        }
      continue;
    }

    for (const auto &blk : predictionBlocks(mb))
    {
      const int x = mx + blk.x, y = my + blk.y;
      // Pred mode: 1 L0, 2 L1, 3 Bi, 4 direct, 5 skip.
      const int pred = mb.skip ? 5 : blk.pred == BDA_JM_PRED_DIRECT ? 4 : blk.pred + 1;
      add(kJmPredMode, x, y, blk.w, blk.h, pred);
      // Motion of the block's top-left 4x4 (direct partitions may vary inside; 4x4 then).
      const auto addMotion = [&](int px, int py, int w, int h) {
        const auto &b4 = p.blocks[std::size_t((py / 4) * bw + px / 4)];
        for (int list = 0; list < 2; ++list)
          if (b4.ref_idx[list] >= 0)
          {
            add(list ? kJmRefIdxL1 : kJmRefIdxL0, px, py, w, h, b4.ref_idx[list]);
            addVector(list ? kJmMvL1 : kJmMvL0, px, py, w, h, b4.mv[list][0], b4.mv[list][1]);
          }
      };
      if (blk.pred == BDA_JM_PRED_DIRECT)
        for (int yy = y; yy < y + blk.h; yy += 4)
          for (int xx = x; xx < x + blk.w; xx += 4)
            addMotion(xx, yy, 4, 4);
      else
        addMotion(x, y, blk.w, blk.h);
    }
  }
}

void DecoderJm::fillStatisticList(stats::StatisticsData &statisticsData) const
{
  using namespace stats::color;
  using stats::StatisticsType;

  StatisticsType mbType(kJmMbType, "MB type", ColorMapper({0, 43}, PredefinedType::Jet));
  mbType.description = "Macroblock type (normalised over the slice types; see the value text).";
  std::vector<QString> kinds(44);
  const std::pair<int, const char *> kindNames[] = {
      {0, "I_4x4"}, {1, "I_8x8"}, {2, "I_16x16"}, {3, "I_PCM"}, {4, "SI"}, {10, "P_L0_16x16"},
      {11, "P_L0_L0_16x8"}, {12, "P_L0_L0_8x16"}, {13, "P_8x8"}, {14, "P_8x8ref0"}, {15, "P_Skip"},
      {20, "B_Direct_16x16"}, {21, "B_L0_16x16"}, {22, "B_L1_16x16"}, {23, "B_Bi_16x16"},
      {24, "B_L0_L0_16x8"}, {25, "B_L0_L0_8x16"}, {26, "B_L1_L1_16x8"}, {27, "B_L1_L1_8x16"},
      {28, "B_L0_L1_16x8"}, {29, "B_L0_L1_8x16"}, {30, "B_L1_L0_16x8"}, {31, "B_L1_L0_8x16"},
      {32, "B_L0_Bi_16x8"}, {33, "B_L0_Bi_8x16"}, {34, "B_L1_Bi_16x8"}, {35, "B_L1_Bi_8x16"},
      {36, "B_Bi_L0_16x8"}, {37, "B_Bi_L0_8x16"}, {38, "B_Bi_L1_16x8"}, {39, "B_Bi_L1_8x16"},
      {40, "B_Bi_Bi_16x8"}, {41, "B_Bi_Bi_8x16"}, {42, "B_8x8"}, {43, "B_Skip"}};
  for (const auto &[value, name] : kindNames)
    kinds[std::size_t(value)] = name;
  mbType.setMappingValues(kinds);
  statisticsData.addStatType(mbType);

  const std::map<int, Color> predColors = {{0, Color(255, 0, 0)},   {1, Color(0, 160, 255)},
                                           {2, Color(0, 0, 255)},   {3, Color(160, 0, 200)},
                                           {4, Color(0, 200, 120)}, {5, Color(255, 255, 0)}};
  StatisticsType predMode(kJmPredMode, "Pred mode", ColorMapper(predColors, Color(128, 128, 128)));
  predMode.description = "Prediction of each partition: intra, list 0, list 1, bi-predicted, direct or skip.";
  predMode.setMappingValues({"INTRA", "L0", "L1", "BI", "DIRECT", "SKIP"});
  predMode.uiGroup = "Pred mode";
  statisticsData.addStatType(predMode);

  StatisticsType intraLuma(kJmIntraLuma, "Intra luma mode", ColorMapper({0, 23}, PredefinedType::Jet));
  intraLuma.description = "Intra4x4PredMode (0..8), Intra8x8PredMode (10..18) or Intra16x16PredMode "
                          "(20..23), with the prediction direction.";
  std::vector<QString> lumaNames(24);
  const char *dirNames[] = {"V", "H", "DC", "DDL", "DDR", "VR", "HD", "VL", "HU"};
  for (int i = 0; i < 9; ++i)
  {
    lumaNames[std::size_t(i)]      = QString("4x4 %1").arg(dirNames[i]);
    lumaNames[std::size_t(10 + i)] = QString("8x8 %1").arg(dirNames[i]);
  }
  const char *i16Names[] = {"16x16 V", "16x16 H", "16x16 DC", "16x16 Plane"};
  for (int i = 0; i < 4; ++i)
    lumaNames[std::size_t(20 + i)] = i16Names[i];
  intraLuma.setMappingValues(lumaNames);
  intraLuma.uiGroup = "Pred mode";
  statisticsData.addStatType(intraLuma);

  StatisticsType intraChroma(kJmIntraChroma, "Intra chroma mode", ColorMapper({0, 3}, PredefinedType::Jet));
  intraChroma.setMappingValues({"DC", "H", "V", "Plane"});
  intraChroma.uiGroup = "Pred mode";
  statisticsData.addStatType(intraChroma);

  for (int list = 0; list < 2; ++list)
  {
    StatisticsType ref(list ? kJmRefIdxL1 : kJmRefIdxL0, QString("Ref idx L%1").arg(list),
                       ColorMapper({0, 15}, PredefinedType::Jet));
    ref.uiGroup = "Ref idx";
    statisticsData.addStatType(ref);
  }
  for (int list = 0; list < 2; ++list)
  {
    StatisticsType mv(list ? kJmMvL1 : kJmMvL0, QString("Motion vector L%1").arg(list), 4);
    mv.description = "Motion vector in quarter samples (reference position minus block position).";
    mv.uiGroup     = "Motion vector";
    statisticsData.addStatType(mv);
  }

  StatisticsType t8(kJmTransform8x8, "Transform 8x8", ColorMapper({0, 1}, Color(0, 0, 0), Color(255, 160, 0)));
  t8.description = "transform_size_8x8_flag";
  statisticsData.addStatType(t8);
  StatisticsType qp(kJmQp, "QP", ColorMapper({0, 51}, PredefinedType::Jet));
  qp.description = "QPY of the macroblock";
  statisticsData.addStatType(qp);
  StatisticsType cbp(kJmCbp, "CBP", ColorMapper({0, 47}, PredefinedType::Jet));
  cbp.description = "coded_block_pattern: bits 0..3 luma 8x8 blocks, (cbp >> 4) chroma";
  statisticsData.addStatType(cbp);
  StatisticsType slice(kJmSlice, "Slice", ColorMapper({0, 16}, PredefinedType::Jet));
  statisticsData.addStatType(slice);
  StatisticsType skip(kJmSkip, "Skip", ColorMapper({0, 1}, Color(0, 0, 0), Color(255, 0, 0)));
  skip.description = "P_Skip / B_Skip";
  statisticsData.addStatType(skip);
  StatisticsType bits(kJmMbBits, "MB bits", ColorMapper({0, 2048}, Color(0, 200, 255, 0), Color(0, 200, 255, 255)));
  bits.description = "Bits of the macroblock in the stream (CABAC: arithmetic decoder position, the "
                     "split between neighbouring macroblocks is approximate by a few bits).";
  statisticsData.addStatType(bits);

  statisticsData.setBlockRectTypeIDs(kJmMbType, std::nullopt);
}

QStringList DecoderJm::getLibraryPaths() const
{
  return QStringList() << "JM analyzer" << this->library.fileName() << this->library.fileName();
}

QString DecoderJm::getDecoderName() const
{
  return this->versionString ? QString::fromUtf8(this->versionString()) : QString("JM");
}

} // namespace bda::integration
