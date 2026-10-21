// Per display frame summary of a parsed H.264 stream, for the Info pane's Frame tab and for
// double-click seeking in the bitstream tree - the H.264 counterpart of FrameObuSummary.
//
// Reads the packet model the bitstream parser fills. Two layouts:
//   * a raw Annex-B file: one top-level row per NAL unit ("NAL n: 1 Slice POC 8");
//   * a container (MP4, MKV, TS): one top-level row per AVPacket with its NAL units below.
// A picture starts at a slice with first_mb_in_slice 0 (or a new POC); its slices follow it. Display
// order is POC order - the parser's POC keeps counting across IDRs - which is also the order the
// decoders output and YUView counts frames in. The container's "YUView frame index" row is the
// packet's decode-order index and must not be used for H.264: it differs as soon as there are
// B-frames.
#pragma once

#include <QModelIndex>

#include <optional>
#include <string>
#include <vector>

class QAbstractItemModel;

namespace bda::integration
{

struct AvcPictureSummary
{
  int                      codingOrder{-1};
  int                      poc{};
  int                      frameNum{-1};
  bool                     idr{};
  int                      nalRefIdc{-1};
  std::vector<std::string> sliceTypes; //!< Per slice: "I", "P", "B", "SP", "SI"
  int                      packetRow{-1}; //!< Container: top-level row of the AVPacket, else -1
  std::vector<int>         nalRows;       //!< Annex-B: top-level rows; container: rows under the packet
  std::string              packetName;    //!< Container only
};

class FrameAvcIndex
{
public:
  void reset();
  //!< Re-reads the model when it grew since the last call.
  void update(const QAbstractItemModel &model);

  bool isAvc() const { return !this->pictures.empty(); }
  int  parsedFrames() const { return int(this->pictures.size()); }

  //!< By YUView (display) frame index; nullptr when not parsed (yet).
  const AvcPictureSummary *frame(int displayIndex) const;
  /* The display frame index of the picture a model row belongs to: `topRow` is the top-level row,
   * `childRow` the NAL row under a packet (container) or -1. Nothing for a row that is no slice.
   */
  std::optional<int> displayIndexOf(int topRow, int childRow) const;

private:
  void addSlice(const QAbstractItemModel &model, const QModelIndex &nal, int packetRow, int nalRow,
                const std::string &packetName);

  std::vector<AvcPictureSummary> pictures;     //!< Coding order
  std::vector<int>               displayOrder; //!< Display index -> coding order index
  int                            seenRows{-1};
  int                            seenLastRowChildren{-1};
};

} // namespace bda::integration
