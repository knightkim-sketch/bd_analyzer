// Per display frame summary of a parsed AV1 stream, for the Info pane's Frame tab.
//
// Reads the packet model the bitstream parser fills (one top-level row per AVPacket, the OBUs of
// that temporal unit below it). Display frame N is the video packet whose "YUView frame index" is
// N - the same one-packet-per-frame rule the OBU tab's double-click seek and the frame dump use.
//
// coding_order counts coded frames in decode order over the whole file, from 0: every frame header
// with show_existing_frame = 0, hidden alternate references included, so the coding_order of the
// shown frames can skip numbers. A show_existing_frame temporal unit codes no frame, so it has no
// coding_order of its own; it reports the frame it puts on screen instead, whose order_hint is found
// by keeping the eight reference slots the way the decoder does.
#pragma once

#include <QModelIndex>

#include <map>
#include <string>
#include <vector>

class QAbstractItemModel;

namespace bda::integration
{

struct CodedFrameSummary
{
  int  codingOrder{-1};
  int  orderHint{-1};
  int  frameType{-1}; //!< 0 KEY, 1 INTER, 2 INTRA_ONLY, 3 SWITCH
  bool showFrame{};
};

struct DisplayFrameSummary
{
  int         yuviewFrameIndex{-1};
  int         packetRow{-1}; //!< Top-level row of the AVPacket in the model
  std::string packetName;

  bool showExisting{};
  int  frameToShowMapIdx{-1}; //!< Only for show_existing_frame
  int  codingOrder{-1};       //!< -1 for show_existing_frame: no frame is coded in this unit
  int  orderHint{-1};         //!< Of the frame on screen; -1 when it cannot be told
  int  frameType{-1};         //!< Of the frame on screen
  int  shownCodingOrder{-1};  //!< show_existing_frame: coding_order of the frame it shows

  std::vector<CodedFrameSummary> hidden; //!< Frames coded in this unit with show_frame = 0
};

//!< "KEY_FRAME", "INTER_FRAME", ... or "-".
std::string frameTypeName(int frameType);

/* Walks the model once and keeps what it found, so a model that grows while the parser runs is
 * only read for its new rows. The last row is re-read on every update because the parser may still
 * be appending OBUs to it.
 */
class FrameObuIndex
{
public:
  void reset();
  void update(const QAbstractItemModel &model);

  //!< nullptr when that frame's packet has not been parsed (yet).
  const DisplayFrameSummary *frame(int yuviewFrameIndex) const;
  int                        parsedFrames() const { return int(this->frames.size()); }

private:
  struct Slot
  {
    int codingOrder{-1};
    int orderHint{-1};
    int frameType{-1};
  };
  struct State
  {
    Slot refSlots[8];
    int  nextCodingOrder{};
  };

  void readRow(const QAbstractItemModel &model, int row, State &state);

  State                              done;        //!< After every completed row
  int                                doneRows{};  //!< Rows folded into `done`
  std::map<int, DisplayFrameSummary> frames;      //!< By YUView frame index
  int                                tailFrame{-1}; //!< Frame read from the unfinished last row
};

} // namespace bda::integration
