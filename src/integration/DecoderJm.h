// H.264 analysis decoder: the JM 19.0 reference decoder (libbda-jm-analyzer.so, built by
// scripts/setup-jm-analyzer.sh) behind YUView's decoder interface - for H.264 what the dav1d
// analyzer is for AV1. Next to the picture it reports every macroblock: type and partitions,
// prediction direction, intra modes, reference indices and motion vectors, transform size, QP,
// coded block pattern, slice and the macroblock's bits in the stream (with their bit range, for the
// block hexdump).
//
// Bit ranges. The library counts them in the bytes pushed into it; the item says, before each NAL
// unit it pushes, where that unit sits: in the file (Annex-B input, setNextDataFilePosition) or in
// its demuxed packet (container input, setNextDataOffsetInPacket). Ranges come out in those
// coordinates, file bits or packet bits, and getItemDataDump() rebases the file ones on the frame
// it shows.
#pragma once

#include <decoder/decoderBase.h>

#include <QByteArray>
#include <QLibrary>
#include <QStringList>

#include <map>
#include <optional>
#include <vector>

#include "../../third_party/jm-analyzer/bda_jm.h"

namespace bda::integration
{

//!< Statistic type IDs of the JM decoder.
enum JmStatType
{
  kJmMbType       = 0,
  kJmPredMode     = 1,
  kJmIntraLuma    = 2,
  kJmIntraChroma  = 3,
  kJmRefIdxL0     = 4,
  kJmRefIdxL1     = 5,
  kJmMvL0         = 6,
  kJmMvL1         = 7,
  kJmTransform8x8 = 8,
  kJmQp           = 9,
  kJmCbp          = 10,
  kJmSlice        = 11,
  kJmSkip         = 12,
  kJmMbBits       = 13
};

class DecoderJm : public decoder::decoderBase
{
public:
  explicit DecoderJm(bool cachingDecoder = false);
  ~DecoderJm() override;

  void resetDecoder() override;

  bool       decodeNextFrame() override;
  QByteArray getRawFrameData() override;
  bool       pushData(QByteArray &data) override;

  void setNextDataOffsetInPacket(int offset) override;
  void setNextDataFilePosition(std::int64_t position) override;
  /* After a seek to frame `frameIdx` (an IDR: no picture before it in decoding order is output after
   * it, so its display and coding index agree), the pictures that follow count from there.
   */
  void setNextOutputFrameIndex(int frameIdx) override
  {
    this->nextOutputFrame = frameIdx;
    this->codingBase      = frameIdx;
  }
  std::optional<int> codingIndexOfFrame(int frameIdx) const override;

  void               fillStatisticList(stats::StatisticsData &statisticsData) const override;
  std::optional<int> blockBitsTypeId() const override { return kJmMbBits; }

  QStringList getLibraryPaths() const override;
  QString     getDecoderName() const override;
  QString     getCodecName() const override { return "H.264/AVC"; }

private:
  struct Segment
  {
    std::int64_t pushed{};   //!< Offset of the NAL unit header in the pushed bytes
    std::int64_t position{}; //!< ... and in the file or the packet
    std::int64_t length{};   //!< Bytes from the header on
    bool         inFile{};
  };
  struct Picture
  {
    QByteArray                yuv;
    int                       width{}, height{}, cropLeft{}, cropTop{};
    int                       mbWidth{}, mbHeight{};
    bool                      hasMbData{};
    std::vector<bda_jm_mb>    mbs;
    std::vector<bda_jm_block> blocks;
    std::vector<bda_jm_slice> slices;
    int                       outputFrame{-1};
    std::int64_t              decodeIndex{-1};
  };

  bool createHandle();
  void pullAhead();
  void takePicture(const bda_jm_picture &picture);
  void addStatistics();
  //!< The slice's NAL unit position (bytes) in file or packet coordinates, and which of the two.
  std::optional<std::pair<std::int64_t, bool>> nalPosition(std::int64_t pushedOffset) const;

  QLibrary library;
  bda_jm  *handle{};

  decltype(&bda_jm_api_version)    apiVersion{};
  decltype(&bda_jm_version_string) versionString{};
  decltype(&bda_jm_create)         createFn{};
  decltype(&bda_jm_destroy)        destroyFn{};
  decltype(&bda_jm_push)           pushFn{};
  decltype(&bda_jm_pull)           pullFn{};
  decltype(&bda_jm_last_error)     lastError{};

  std::int64_t         pushedBytes{};
  std::vector<Segment> segments;
  std::optional<std::pair<std::int64_t, bool>> nextPosition; //!< (position, inFile) of the next push

  std::optional<Picture> pending;  //!< Pulled, not yet handed out
  Picture                current;  //!< Handed out by the last decodeNextFrame()
  bool                   statisticsAdded{};
  int                    nextOutputFrame{0};
  std::int64_t           codingBase{0}; //!< Coding index of the first picture since the last reset
  std::map<int, int>     codingIndexByFrame;
};

} // namespace bda::integration
