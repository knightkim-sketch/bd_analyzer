#include "FrameObuSummary.h"

#include <QAbstractItemModel>

#include <optional>

namespace bda::integration
{

namespace
{

constexpr int kKeyFrame    = 0;
constexpr int kSwitchFrame = 3;

struct FrameHeaderValues
{
  std::optional<int> showExisting, frameToShowMapIdx, frameType, showFrame, orderHint, refresh;
};

std::optional<int> intAt(const QAbstractItemModel &model, int row, const QModelIndex &parent)
{
  bool       ok    = false;
  const auto value = model.data(model.index(row, 1, parent)).toString().toInt(&ok);
  return ok ? std::optional<int>(value) : std::nullopt;
}

/* First value of each frame header element under one OBU row.
 *
 * One walk for all of them instead of a search per name, and the tile group is not entered: it is
 * by far the largest part of a frame OBU and holds none of these.
 */
void collect(const QAbstractItemModel &model, const QModelIndex &parent, FrameHeaderValues &v)
{
  for (int row = 0; row < model.rowCount(parent); ++row)
  {
    const auto name = model.data(model.index(row, 0, parent)).toString();
    if (name.startsWith("tile_group"))
      continue;
    const auto take = [&](std::optional<int> &field) {
      if (!field)
        field = intAt(model, row, parent);
    };
    if (name == "show_existing_frame")
      take(v.showExisting);
    else if (name == "frame_to_show_map_idx")
      take(v.frameToShowMapIdx);
    else if (name == "frame_type")
      take(v.frameType);
    else if (name == "show_frame")
      take(v.showFrame);
    else if (name == "order_hint")
      take(v.orderHint);
    else if (name == "refresh_frame_flags")
      take(v.refresh);
    collect(model, model.index(row, 0, parent), v);
  }
}

} // namespace

std::string frameTypeName(const int frameType)
{
  switch (frameType)
  {
  case 0:
    return "KEY_FRAME";
  case 1:
    return "INTER_FRAME";
  case 2:
    return "INTRA_ONLY_FRAME";
  case 3:
    return "SWITCH_FRAME";
  default:
    return "-";
  }
}

void FrameObuIndex::reset()
{
  *this = FrameObuIndex();
}

void FrameObuIndex::update(const QAbstractItemModel &model)
{
  const int rows = model.rowCount();
  if (rows < this->doneRows)
  {
    // The model was reset under us (another item, or the parse restarted).
    this->reset();
  }
  if (this->tailFrame >= 0)
  {
    this->frames.erase(this->tailFrame);
    this->tailFrame = -1;
  }
  for (; this->doneRows < rows - 1; ++this->doneRows)
    this->readRow(model, this->doneRows, this->done);

  if (rows > 0)
  {
    // The last row may still be filling: read it into a copy and keep it apart.
    auto       tail   = this->done;
    const auto before = this->frames.size();
    this->readRow(model, rows - 1, tail);
    if (this->frames.size() > before)
      this->tailFrame = this->frames.rbegin()->first;
  }
}

const DisplayFrameSummary *FrameObuIndex::frame(const int yuviewFrameIndex) const
{
  const auto it = this->frames.find(yuviewFrameIndex);
  return it == this->frames.end() ? nullptr : &it->second;
}

void FrameObuIndex::readRow(const QAbstractItemModel &model, const int row, State &state)
{
  const auto packetIdx = model.index(row, 0);

  DisplayFrameSummary out;
  out.packetRow  = row;
  out.packetName = model.data(packetIdx).toString().toStdString();
  for (int child = 0; child < model.rowCount(packetIdx); ++child)
    if (model.data(model.index(child, 0, packetIdx)).toString() == "YUView frame index")
      out.yuviewFrameIndex = intAt(model, child, packetIdx).value_or(-1);
  if (out.yuviewFrameIndex < 0)
    return; // Metadata, extradata, or a packet of another stream.

  for (int child = 0; child < model.rowCount(packetIdx); ++child)
  {
    const auto obuIdx = model.index(child, 0, packetIdx);
    if (!model.data(obuIdx).toString().startsWith("OBU"))
      continue;

    FrameHeaderValues v;
    collect(model, obuIdx, v);
    if (!v.showExisting)
      continue; // Not a frame header, or a frame_header_copy() that repeats one.

    if (*v.showExisting == 1)
    {
      const int mapIdx      = v.frameToShowMapIdx.value_or(-1);
      out.showExisting      = true;
      out.frameToShowMapIdx = mapIdx;
      if (mapIdx >= 0 && mapIdx < 8)
      {
        const auto shown     = state.refSlots[mapIdx];
        out.orderHint        = shown.orderHint;
        out.frameType        = shown.frameType;
        out.shownCodingOrder = shown.codingOrder;
        // Showing a key frame again reloads it into every slot (spec 7.21).
        if (shown.frameType == kKeyFrame)
          for (auto &slot : state.refSlots)
            slot = shown;
      }
      continue;
    }

    CodedFrameSummary coded;
    coded.codingOrder = state.nextCodingOrder++;
    coded.orderHint   = v.orderHint.value_or(-1);
    coded.frameType   = v.frameType.value_or(-1);
    coded.showFrame   = v.showFrame.value_or(0) == 1;

    // Not written to the stream for these two; the decoder takes every slot.
    const bool refreshAll =
        coded.frameType == kSwitchFrame || (coded.frameType == kKeyFrame && coded.showFrame);
    const int refresh = refreshAll ? 0xff : v.refresh.value_or(0);
    for (int slot = 0; slot < 8; ++slot)
      if ((refresh >> slot) & 1)
        state.refSlots[slot] = {coded.codingOrder, coded.orderHint, coded.frameType};

    if (coded.showFrame)
    {
      out.codingOrder = coded.codingOrder;
      out.orderHint   = coded.orderHint;
      out.frameType   = coded.frameType;
    }
    else
      out.hidden.push_back(coded);
  }
  this->frames[out.yuviewFrameIndex] = out;
}

} // namespace bda::integration
