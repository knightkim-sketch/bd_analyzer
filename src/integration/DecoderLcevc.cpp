#include "DecoderLcevc.h"

#include <decoder/decoderFFmpeg.h>
#include <statistics/StatisticsData.h>
#include <statistics/StatisticsType.h>

#include <algorithm>
#include <cstring>

namespace bda::integration
{

namespace
{

enum Signal
{
  kEnhanced = 0,
  kBase,
  kLoq1Prediction,
  kLoq1Residual,
  kLoq0Prediction,
  kLoq0Residual,
  kTemporalBuffer,
  kTemporalMask,
  kSignalCount
};

// The LTM surface each analysis signal shows (-1: not a surface).
int surfaceOfSignal(int signal)
{
  switch (signal)
  {
  case kLoq1Prediction:
    return BDA_LTM_SURF_BASE_PRED;
  case kLoq1Residual:
    return BDA_LTM_SURF_LOQ1_RESIDUAL;
  case kLoq0Prediction:
    return BDA_LTM_SURF_LOQ0_PRED;
  case kLoq0Residual:
    return BDA_LTM_SURF_LOQ0_RESIDUAL;
  case kTemporalBuffer:
    return BDA_LTM_SURF_TEMPORAL_BUFFER;
  case kTemporalMask:
    return BDA_LTM_SURF_TEMPORAL_MASK;
  default:
    return -1;
  }
}

bool isResidualSurface(int surfaceId)
{
  return surfaceId == BDA_LTM_SURF_LOQ1_RESIDUAL || surfaceId == BDA_LTM_SURF_LOQ0_RESIDUAL ||
         surfaceId == BDA_LTM_SURF_TEMPORAL_BUFFER;
}

int chromaFormatOf(video::yuv::Subsampling s)
{
  using video::yuv::Subsampling;
  return s == Subsampling::YUV_400 ? 0 : s == Subsampling::YUV_422 ? 2 : s == Subsampling::YUV_444 ? 3 : 1;
}

int readSample(const std::uint8_t *row, int x, int bytesPerSample)
{
  if (bytesPerSample == 1)
    return row[x];
  return row[2 * x] | (row[2 * x + 1] << 8);
}

void writeSample(std::uint8_t *&out, int value, int bytesPerSample)
{
  *out++ = std::uint8_t(value);
  if (bytesPerSample == 2)
    *out++ = std::uint8_t(value >> 8);
}

} // namespace

int LcevcStream::chromaFormat() const
{
  const auto &g = this->summary.global;
  if (g.numImagePlanes == 1)
    return 0;
  if (g.chromaScaleW == 2 && g.chromaScaleH == 2)
    return 1;
  return g.chromaScaleW == 2 ? 2 : 3;
}

int LcevcStream::entryFrameFor(int baseSeekPoint) const
{
  const auto &idr = this->summary.idr;
  for (int f = std::min(baseSeekPoint, int(idr.size()) - 1); f > 0; --f)
    if (idr[std::size_t(f)])
      return f;
  return 0;
}

std::shared_ptr<LcevcStream> makeLcevcStream(std::vector<std::vector<std::uint8_t>> perFrame)
{
  auto stream      = std::make_shared<LcevcStream>();
  stream->summary  = lcevc::summarizeStream(perFrame);
  stream->perFrame = std::move(perFrame);
  return stream;
}

DecoderLcevc::DecoderLcevc(std::unique_ptr<decoder::decoderBase> baseDecoder,
                           LcevcBackend::Kind                    backendKind,
                           std::shared_ptr<const LcevcStream>    lcevcStream,
                           int                                   signal,
                           bool                                  cachingDecoder)
    : decoderBase(cachingDecoder), base(std::move(baseDecoder)), stream(std::move(lcevcStream))
{
  this->backend = LcevcBackend::create(backendKind);
  if (!this->backend->loaded())
  {
    this->setError(this->backend->errorString());
    return;
  }
  if (!this->base || this->base->errorInDecoder())
  {
    this->setError("The base decoder could not be created: " +
                   (this->base ? this->base->decoderErrorString() : QString()));
    return;
  }
  this->backend->setOutputDepth(this->stream->bitDepth());
  this->backend->setForcedBitstreamVersion(this->stream->summary.bitstreamVersion ? -1 : 1);
  this->backend->reset();
  this->frameSize = Size(unsigned(this->stream->outputWidth()), unsigned(this->stream->outputHeight()));
  this->rawFormat = video::RawFormat::YUV;
  static const video::yuv::Subsampling kSubsampling[] = {
      video::yuv::Subsampling::YUV_400, video::yuv::Subsampling::YUV_420,
      video::yuv::Subsampling::YUV_422, video::yuv::Subsampling::YUV_444};
  this->formatYUV = video::yuv::PixelFormatYUV(kSubsampling[this->stream->chromaFormat()],
                                               unsigned(this->stream->bitDepth()));
  // The surfaces and the configuration come from LTM only.
  this->internalsSupported = backendKind == LcevcBackend::Kind::Ltm;
  if (signal >= 0 && signal < this->nrSignalsSupported())
    this->decodeSignal = signal;
  this->syncState();
}

void DecoderLcevc::syncState()
{
  if (this->decoderState == decoder::DecoderState::Error || !this->base)
    return;
  if (this->base->errorInDecoder())
    this->setError(this->base->decoderErrorString());
  else
    this->decoderState = this->base->state();
}

void DecoderLcevc::resetDecoder()
{
  if (!this->base || !this->backend->loaded())
    return;
  this->decoderState = decoder::DecoderState::NeedsMoreData;
  this->base->resetDecoder();
  if (!this->backend->reset())
    this->setError(this->backend->errorString());
  this->output.clear();
  this->pictureInfo.reset();
  this->lastFrame = -1;
  this->syncState();
}

int DecoderLcevc::nrSignalsSupported() const
{
  return this->backend && this->backend->kind() == LcevcBackend::Kind::Ltm ? kSignalCount : 2;
}

QStringList DecoderLcevc::getSignalNames() const
{
  const QStringList all = {"Enhanced",
                           "Base",
                           "LoQ-1 prediction (base upsampled)",
                           "LoQ-1 residual",
                           "LoQ-0 prediction",
                           "LoQ-0 residual",
                           "Temporal buffer",
                           "Temporal mask"};
  return all.mid(0, this->nrSignalsSupported());
}

bool DecoderLcevc::isSignalDifference(int signalID) const
{
  return isResidualSurface(surfaceOfSignal(signalID));
}

void DecoderLcevc::setDecodeSignal(int signalID, bool &decoderResetNeeded)
{
  decoderResetNeeded = false;
  if (signalID < 0 || signalID >= this->nrSignalsSupported() || signalID == this->decodeSignal)
    return;
  this->decodeSignal = signalID;
  // The current picture has to be composed again, and the enhancement cannot be re-run on its own.
  decoderResetNeeded = true;
}

bool DecoderLcevc::pushData(QByteArray &data)
{
  if (this->decoderState == decoder::DecoderState::Error)
    return false;
  const auto ok = this->base->pushData(data);
  this->syncState();
  return ok;
}

bool DecoderLcevc::pushAVPacket(FFmpeg::AVPacketWrapper &packet)
{
  auto *ffmpeg = dynamic_cast<decoder::decoderFFmpeg *>(this->base.get());
  if (this->decoderState == decoder::DecoderState::Error || !ffmpeg)
    return false;
  const auto ok = ffmpeg->pushAVPacket(packet);
  this->syncState();
  return ok;
}

void DecoderLcevc::setNextDataOffsetInPacket(int offset)
{
  this->base->setNextDataOffsetInPacket(offset);
}

bool DecoderLcevc::decodeNextFrame()
{
  if (this->decoderState == decoder::DecoderState::Error)
    return false;
  if (!this->base->decodeNextFrame())
  {
    this->syncState();
    return false;
  }
  this->syncState();
  const auto frameIdx = this->nextFrame++;
  this->lastFrame     = frameIdx;
  const auto baseRaw  = this->base->getRawFrameData();

  const auto &perFrame = this->stream->perFrame;
  if (frameIdx < 0 || std::size_t(frameIdx) >= perFrame.size() || perFrame[std::size_t(frameIdx)].empty())
  {
    // No enhancement for this picture: show the base, scaled to the enhanced size.
    this->pictureInfo.reset();
    this->composeBase(baseRaw);
    return true;
  }

  const auto baseSize   = this->base->getFrameSize();
  const auto baseFormat = this->base->getPixelFormatYUV();
  LcevcPlanes planes;
  planes.width          = int(baseSize.width);
  planes.height         = int(baseSize.height);
  planes.bitDepth       = int(baseFormat.getBitsPerSample());
  planes.chromaFormat   = chromaFormatOf(baseFormat.getSubsampling());
  const int bytesPerSample = planes.bitDepth > 8 ? 2 : 1;
  const auto *p            = reinterpret_cast<const std::uint8_t *>(baseRaw.constData());
  for (int plane = 0; plane < (planes.chromaFormat == 0 ? 1 : 3); ++plane)
  {
    const int w         = lcevcPlaneWidth(planes.width, planes.chromaFormat, plane);
    const int h         = lcevcPlaneHeight(planes.height, planes.chromaFormat, plane);
    planes.data[plane]  = p;
    planes.stride[plane] = std::ptrdiff_t(w) * bytesPerSample;
    p += std::ptrdiff_t(w) * h * bytesPerSample;
  }
  if (p > reinterpret_cast<const std::uint8_t *>(baseRaw.constData()) + baseRaw.size())
  {
    this->setError("The base picture is smaller than its format says.");
    return false;
  }

  unsigned dumpMask = 0;
  if (const auto surface = surfaceOfSignal(this->decodeSignal); surface >= 0)
    dumpMask |= 1u << surface;
  if (this->statisticsEnabled())
    dumpMask |= (1u << BDA_LTM_SURF_LOQ0_RESIDUAL) | (1u << BDA_LTM_SURF_LOQ1_RESIDUAL) |
                (1u << BDA_LTM_SURF_TEMPORAL_MASK);

  LcevcDecodedPicture picture;
  if (!this->backend->decode(perFrame[std::size_t(frameIdx)], planes, dumpMask, picture))
  {
    this->setError(QString("LCEVC picture %1: %2").arg(frameIdx).arg(this->backend->errorString()));
    return false;
  }
  // The canvas was sized from the first global_config. A later one may change the picture (size,
  // chroma, depth): the video handler would then read past the end of the frame.
  if (picture.width != int(this->frameSize.width) || picture.height != int(this->frameSize.height) ||
      picture.bitDepth != int(this->formatYUV.getBitsPerSample()) ||
      picture.chromaFormat != this->stream->chromaFormat())
  {
    this->setError(QString("LCEVC picture %1 is %2x%3, %4-bit, chroma format %5, but the stream started as "
                           "%6x%7, %8-bit: a change of the enhanced picture format is not supported.")
                       .arg(frameIdx)
                       .arg(picture.width)
                       .arg(picture.height)
                       .arg(picture.bitDepth)
                       .arg(picture.chromaFormat)
                       .arg(this->frameSize.width)
                       .arg(this->frameSize.height)
                       .arg(this->formatYUV.getBitsPerSample()));
    return false;
  }
  this->pictureInfo = picture.info;
  this->composeOutput(baseRaw, picture);
  if (this->statisticsEnabled())
    this->addStatistics(picture);
  return true;
}

void DecoderLcevc::composeOutput(const QByteArray &baseRaw, const LcevcDecodedPicture &picture)
{
  const auto surface = surfaceOfSignal(this->decodeSignal);
  if (this->decodeSignal == kBase)
    this->composeBase(baseRaw);
  else if (surface >= 0)
    this->composeSurface(picture, surface);
  else
    this->output = picture.yuv;
}

void DecoderLcevc::composeBase(const QByteArray &baseRaw)
{
  // Nearest-neighbour onto the output canvas, with the base's samples brought to the output depth.
  const auto baseSize   = this->base->getFrameSize();
  const auto baseFormat = this->base->getPixelFormatYUV();
  const int  inDepth    = int(baseFormat.getBitsPerSample());
  const int  inChroma   = chromaFormatOf(baseFormat.getSubsampling());
  const int  inBytes    = inDepth > 8 ? 2 : 1;
  const int  outDepth   = int(this->formatYUV.getBitsPerSample());
  const int  outChroma  = this->stream->chromaFormat();
  const int  outBytes   = outDepth > 8 ? 2 : 1;
  const int  W = int(this->frameSize.width), H = int(this->frameSize.height);

  qsizetype total = 0;
  for (int p = 0; p < (outChroma == 0 ? 1 : 3); ++p)
    total += qsizetype(lcevcPlaneWidth(W, outChroma, p)) * lcevcPlaneHeight(H, outChroma, p) * outBytes;
  this->output.resize(total);
  auto *out = reinterpret_cast<std::uint8_t *>(this->output.data());

  const auto *in = reinterpret_cast<const std::uint8_t *>(baseRaw.constData());
  for (int p = 0; p < (outChroma == 0 ? 1 : 3); ++p)
  {
    const int w  = lcevcPlaneWidth(int(baseSize.width), inChroma, p);
    const int h  = lcevcPlaneHeight(int(baseSize.height), inChroma, p);
    const int ow = lcevcPlaneWidth(W, outChroma, p), oh = lcevcPlaneHeight(H, outChroma, p);
    const bool havePlane = p == 0 || inChroma != 0;
    for (int y = 0; y < oh; ++y)
      for (int x = 0; x < ow; ++x)
      {
        int v = 1 << (outDepth - 1);
        if (havePlane && w > 0 && h > 0)
        {
          const auto *row = in + std::ptrdiff_t(y * h / oh) * w * inBytes;
          v               = readSample(row, x * w / ow, inBytes);
          v               = outDepth >= inDepth ? v << (outDepth - inDepth) : v >> (inDepth - outDepth);
        }
        writeSample(out, v, outBytes);
      }
    if (havePlane)
      in += std::ptrdiff_t(w) * h * inBytes;
  }
}

void DecoderLcevc::composeSurface(const LcevcDecodedPicture &picture, int surfaceId)
{
  const int  d        = picture.bitDepth;
  const int  bytes    = d > 8 ? 2 : 1;
  const int  maxValue = (1 << d) - 1;
  const int  neutral  = 1 << (d - 1);
  const int  shift    = 15 - d;
  const bool residual = isResidualSurface(surfaceId);
  const int  W = picture.width, H = picture.height, chroma = picture.chromaFormat;

  qsizetype total = 0;
  for (int p = 0; p < (chroma == 0 ? 1 : 3); ++p)
    total += qsizetype(lcevcPlaneWidth(W, chroma, p)) * lcevcPlaneHeight(H, chroma, p) * bytes;
  this->output.resize(total);
  auto *out = reinterpret_cast<std::uint8_t *>(this->output.data());

  for (int p = 0; p < (chroma == 0 ? 1 : 3); ++p)
  {
    const auto &s  = picture.surfaces[std::size_t(surfaceId)][std::size_t(p)];
    const int   ow = lcevcPlaneWidth(W, chroma, p), oh = lcevcPlaneHeight(H, chroma, p);
    for (int y = 0; y < oh; ++y)
      for (int x = 0; x < ow; ++x)
      {
        int v = neutral;
        if (s.valid())
        {
          const int sx = x * s.width / ow, sy = y * s.height / oh;
          if (s.bytesPerSample == 1)
            v = p == 0 ? int(s.u8(sx, sy)) << (d - 8) : neutral;
          else if (residual)
            v = neutral + int(s.s16(sx, sy)) / (1 << shift);
          else
            v = (int(s.s16(sx, sy)) + 0x4000 + (1 << (shift - 1))) >> shift;
        }
        writeSample(out, std::clamp(v, 0, maxValue), bytes);
      }
  }
}

void DecoderLcevc::addStatistics(const LcevcDecodedPicture &picture)
{
  auto *stats = this->statisticsData;
  stats->setFrameIndex(this->lastFrame);
  if (!picture.info)
    return;
  const auto &info  = *picture.info;
  const int   tb    = info.transform_block_size > 0 ? info.transform_block_size : 2;
  const int   shift = 15 - picture.bitDepth;
  const int   W = picture.width, H = picture.height;

  // Sum of |residual| in sample units per transform block, on the output canvas.
  auto addResidual = [&](int surfaceId, int typeId) {
    const auto &s = picture.surfaces[std::size_t(surfaceId)][0];
    if (!s.valid())
      return;
    // Edge blocks may be cut by the surface border.
    for (int by = 0; by < s.height; by += tb)
      for (int bx = 0; bx < s.width; bx += tb)
      {
        int sum = 0;
        for (int y = by; y < std::min(by + tb, s.height); ++y)
          for (int x = bx; x < std::min(bx + tb, s.width); ++x)
            sum += std::abs(int(s.s16(x, y))) >> shift;
        if (sum == 0)
          continue;
        const int x0 = bx * W / s.width, y0 = by * H / s.height;
        const int x1 = (bx + tb) * W / s.width, y1 = (by + tb) * H / s.height;
        if (x0 < W && y0 < H)
          (*stats)[typeId].addBlockValue(std::uint16_t(x0), std::uint16_t(y0),
                                         std::uint16_t(std::min(x1, W) - x0), std::uint16_t(std::min(y1, H) - y0),
                                         sum);
      }
  };
  addResidual(BDA_LTM_SURF_LOQ0_RESIDUAL, kStatLoq0Residual);
  addResidual(BDA_LTM_SURF_LOQ1_RESIDUAL, kStatLoq1Residual);

  // Transform blocks that were refreshed (intra) rather than predicted from the temporal buffer. The
  // mask holds one sample per transform block.
  if (const auto &mask = picture.surfaces[BDA_LTM_SURF_TEMPORAL_MASK][0]; mask.valid())
    for (int by = 0; by < mask.height; ++by)
      for (int bx = 0; bx < mask.width; ++bx)
        if (mask.u8(bx, by) == 192)
        {
          const int x0 = bx * W / mask.width, y0 = by * H / mask.height;
          const int x1 = std::min(W, (bx + 1) * W / mask.width), y1 = std::min(H, (by + 1) * H / mask.height);
          if (x0 < W && y0 < H)
            (*stats)[kStatTemporalRefresh].addBlockValue(std::uint16_t(x0), std::uint16_t(y0),
                                                         std::uint16_t(x1 - x0), std::uint16_t(y1 - y0), 1);
        }

  if (info.tile_width > 0 && info.tile_height > 0)
  {
    int index = 0;
    for (int y = 0; y < H; y += info.tile_height)
      for (int x = 0; x < W; x += info.tile_width)
        (*stats)[kStatTile].addBlockValue(std::uint16_t(x), std::uint16_t(y),
                                          std::uint16_t(std::min(info.tile_width, W - x)),
                                          std::uint16_t(std::min(info.tile_height, H - y)), index++);
  }
}

void DecoderLcevc::fillStatisticList(stats::StatisticsData &statisticsData) const
{
  using namespace stats::color;
  stats::StatisticsType loq0(kStatLoq0Residual, "LCEVC LoQ-0 residual", ColorMapper({0, 64}, PredefinedType::Jet));
  loq0.description = "Sum of |residual| (sample units) per transform block of enhancement sub-layer 2, "
                     "at full resolution. Blocks without residual are not drawn.";
  statisticsData.addStatType(loq0);
  stats::StatisticsType loq1(kStatLoq1Residual, "LCEVC LoQ-1 residual", ColorMapper({0, 64}, PredefinedType::Jet));
  loq1.description = "Sum of |residual| (sample units) per transform block of enhancement sub-layer 1, "
                     "drawn on the output picture.";
  statisticsData.addStatType(loq1);
  stats::StatisticsType refresh(kStatTemporalRefresh, "LCEVC temporal refresh",
                                ColorMapper({0, 1}, Color(0, 0, 0), Color(255, 0, 0)));
  refresh.description = "Transform blocks coded intra (temporal buffer refreshed) instead of predicted.";
  statisticsData.addStatType(refresh);
  stats::StatisticsType tile(kStatTile, "LCEVC tile", ColorMapper({0, 16}, PredefinedType::Jet));
  tile.description = "The enhancement tiles (tile_dimensions_type), by index.";
  statisticsData.addStatType(tile);
}

QStringList DecoderLcevc::getLibraryPaths() const
{
  auto paths = this->base ? this->base->getLibraryPaths() : QStringList();
  if (this->backend)
    paths << this->backend->name() << this->backend->libraryPath() << this->backend->libraryPath();
  return paths;
}

QString DecoderLcevc::getDecoderName() const
{
  const auto enhancement =
      this->backend && this->backend->kind() == LcevcBackend::Kind::Ltm ? QString("LCEVC (LTM)") : QString("LCEVC (LCEVCdec)");
  return enhancement + " over " + (this->base ? this->base->getDecoderName() : QString("?"));
}

QString DecoderLcevc::getCodecName() const
{
  return "LCEVC + " + (this->base ? this->base->getCodecName() : QString("?"));
}

} // namespace bda::integration
