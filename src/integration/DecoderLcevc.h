// LCEVC decoding for a compressed video item: a base decoder (FFmpeg, dav1d, ...) wrapped so that
// every picture it outputs is enhanced with the LCEVC NAL unit that belongs to it.
//
// The pairing is fixed before decoding starts. The item lists, per picture in output order, the
// LCEVC NAL unit as Annex-B bytes (LcevcStream): from an Annex-B stream by the base picture each unit
// precedes, from IVF by the temporal unit that carries it. The wrapper then only has to know which
// output picture comes next - setNextFrameIndex() after every seek - because the base decoder
// already delivers pictures in output order, which is the order both enhancement decoders want.
//
// The enhancement keeps state from picture to picture (the temporal buffer, the last
// global_config), so decoding has to start at a picture LCEVC can start from: entryFrames.
#pragma once

#include <decoder/decoderBase.h>
#include <ffmpeg/AVPacketWrapper.h>

#include <QByteArray>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

#include "LcevcBackend.h"
#include "lcevc/LcevcSyntax.h"

namespace bda::integration
{

struct LcevcStream
{
  std::vector<std::vector<std::uint8_t>> perFrame; //!< Output order; empty where a picture has none
  lcevc::StreamSummary                   summary;

  int outputWidth() const { return int(this->summary.global.outputWidth()); }
  int outputHeight() const { return int(this->summary.global.outputHeight()); }
  int chromaFormat() const; //!< 0 4:0:0, 1 4:2:0, 2 4:2:2, 3 4:4:4
  int bitDepth() const { return int(this->summary.global.enhancementDepth); }
  bool valid() const { return this->summary.global.valid && !this->perFrame.empty(); }

  /* The output picture decoding must start from to show `frame` correctly: the last one at or before
   * it that is an LCEVC IDR, or 0. `baseSeekPoint` is where the base decoder could start; the result
   * is never after it.
   */
  int entryFrameFor(int baseSeekPoint) const;
};

std::shared_ptr<LcevcStream> makeLcevcStream(std::vector<std::vector<std::uint8_t>> perFrame);

//!< YUView's statistic type IDs of the LCEVC overlays (above the base decoders' own).
enum LcevcStatType
{
  kStatLoq0Residual = 400,
  kStatLoq1Residual = 401,
  kStatTemporalRefresh = 402,
  kStatTile = 403
};

class DecoderLcevc : public decoder::decoderBase
{
public:
  DecoderLcevc(std::unique_ptr<decoder::decoderBase> base,
               LcevcBackend::Kind                    backend,
               std::shared_ptr<const LcevcStream>    stream,
               int                                   signal,
               bool                                  cachingDecoder = false);

  void resetDecoder() override;
  //!< The output picture the next decoded frame is (call after every seek).
  void setNextFrameIndex(int frameIdx) { this->nextFrame = frameIdx; }

  int         nrSignalsSupported() const override;
  QStringList getSignalNames() const override;
  bool        isSignalDifference(int signalID) const override;
  void        setDecodeSignal(int signalID, bool &decoderResetNeeded) override;

  bool       decodeNextFrame() override;
  QByteArray getRawFrameData() override { return this->output; }
  bool       pushData(QByteArray &data) override;
  //!< Container input decoded by FFmpeg: the packets go to the FFmpeg base decoder as they are.
  bool       pushAVPacket(FFmpeg::AVPacketWrapper &packet);
  void       setNextDataOffsetInPacket(int offset) override;

  void fillStatisticList(stats::StatisticsData &statisticsData) const override;

  QStringList getLibraryPaths() const override;
  QString     getDecoderName() const override;
  QString     getCodecName() const override;

  //!< The signalled configuration of the last enhanced picture (LTM only).
  const std::optional<bda_ltm_picture_info> &lastPictureInfo() const { return this->pictureInfo; }
  int                                        lastFrameIndex() const { return this->lastFrame; }

private:
  void syncState();
  void composeOutput(const QByteArray &baseRaw, const LcevcDecodedPicture &picture);
  void composeBase(const QByteArray &baseRaw);
  void composeSurface(const LcevcDecodedPicture &picture, int surfaceId);
  void addStatistics(const LcevcDecodedPicture &picture);

  std::unique_ptr<decoder::decoderBase> base;
  std::unique_ptr<LcevcBackend>         backend;
  std::shared_ptr<const LcevcStream>    stream;

  int        nextFrame{0};
  int        lastFrame{-1};
  QByteArray output;
  std::optional<bda_ltm_picture_info> pictureInfo;
};

} // namespace bda::integration
