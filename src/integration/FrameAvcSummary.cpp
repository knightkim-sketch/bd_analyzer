#include "FrameAvcSummary.h"

#include <QAbstractItemModel>
#include <QRegularExpression>

#include <algorithm>
#include <numeric>

namespace bda::integration
{

namespace
{

QString nameAt(const QAbstractItemModel &model, int row, const QModelIndex &parent)
{
  return model.data(model.index(row, 0, parent)).toString();
}

std::optional<int> intValue(const QAbstractItemModel &model, const QModelIndex &parent, const char *name)
{
  for (int row = 0; row < model.rowCount(parent); ++row)
    if (nameAt(model, row, parent) == name)
    {
      bool       ok    = false;
      const auto value = model.data(model.index(row, 1, parent)).toString().toInt(&ok);
      return ok ? std::optional<int>(value) : std::nullopt;
    }
  return std::nullopt;
}

QModelIndex childNamed(const QAbstractItemModel &model, const QModelIndex &parent, const QString &prefix)
{
  for (int row = 0; row < model.rowCount(parent); ++row)
    if (nameAt(model, row, parent).startsWith(prefix))
      return model.index(row, 0, parent);
  return {};
}

// "NAL 4: 1 Slice POC 8" - a coded slice (IDR or not); nothing for other NAL units.
std::optional<int> slicePoc(const QString &name)
{
  static const QRegularExpression re(R"(^NAL \d+: (1|5) Slice.* POC (-?\d+))");
  const auto                      m = re.match(name);
  if (!m.hasMatch())
    return std::nullopt;
  return m.captured(2).toInt();
}

const char *sliceTypeName(int sliceTypeId)
{
  static const char *names[] = {"P", "B", "I", "SP", "SI"};
  return sliceTypeId >= 0 ? names[sliceTypeId % 5] : "?";
}

} // namespace

void FrameAvcIndex::reset()
{
  this->pictures.clear();
  this->displayOrder.clear();
  this->seenRows            = -1;
  this->seenLastRowChildren = -1;
}

void FrameAvcIndex::addSlice(const QAbstractItemModel &model,
                             const QModelIndex        &nal,
                             int                       packetRow,
                             int                       nalRow,
                             const std::string        &packetName)
{
  const auto poc = slicePoc(model.data(nal).toString());
  if (!poc)
    return;
  const auto header      = childNamed(model, nal, "nal_unit_header");
  const auto rbsp        = childNamed(model, nal, "slice_layer");
  const auto sliceHeader = childNamed(model, rbsp, "slice_header");
  const auto firstMb     = intValue(model, sliceHeader, "first_mb_in_slice").value_or(0);
  const auto nalType     = intValue(model, header, "nal_unit_type").value_or(1);

  if (this->pictures.empty() || firstMb == 0 || this->pictures.back().poc != *poc)
  {
    AvcPictureSummary picture;
    picture.codingOrder = int(this->pictures.size());
    picture.poc         = *poc;
    picture.frameNum    = intValue(model, sliceHeader, "frame_num").value_or(-1);
    picture.idr         = nalType == 5;
    picture.nalRefIdc   = intValue(model, header, "nal_ref_idc").value_or(-1);
    picture.packetRow   = packetRow;
    picture.packetName  = packetName;
    this->pictures.push_back(std::move(picture));
  }
  auto &picture = this->pictures.back();
  picture.sliceTypes.push_back(sliceTypeName(intValue(model, sliceHeader, "slice_type_id").value_or(-1)));
  picture.nalRows.push_back(nalRow);
}

void FrameAvcIndex::update(const QAbstractItemModel &model)
{
  const int rows      = model.rowCount();
  const int lastChild = rows > 0 ? model.rowCount(model.index(rows - 1, 0)) : 0;
  if (rows == this->seenRows && lastChild == this->seenLastRowChildren)
    return;
  this->reset();
  this->seenRows            = rows;
  this->seenLastRowChildren = lastChild;

  for (int row = 0; row < rows; ++row)
  {
    const auto top  = model.index(row, 0);
    const auto name = model.data(top).toString();
    if (name.startsWith("NAL "))
      this->addSlice(model, top, -1, row, {});
    else if (name.contains("AVPacket"))
      for (int child = 0; child < model.rowCount(top); ++child)
        if (nameAt(model, child, top).startsWith("NAL "))
          this->addSlice(model, model.index(child, 0, top), row, child, name.toStdString());
  }

  this->displayOrder.resize(this->pictures.size());
  std::iota(this->displayOrder.begin(), this->displayOrder.end(), 0);
  std::stable_sort(this->displayOrder.begin(), this->displayOrder.end(),
                   [this](int a, int b) { return this->pictures[std::size_t(a)].poc < this->pictures[std::size_t(b)].poc; });
}

const AvcPictureSummary *FrameAvcIndex::frame(int displayIndex) const
{
  if (displayIndex < 0 || std::size_t(displayIndex) >= this->displayOrder.size())
    return nullptr;
  return &this->pictures[std::size_t(this->displayOrder[std::size_t(displayIndex)])];
}

std::optional<int> FrameAvcIndex::displayIndexOf(int topRow, int childRow) const
{
  for (std::size_t display = 0; display < this->displayOrder.size(); ++display)
  {
    const auto &picture = this->pictures[std::size_t(this->displayOrder[display])];
    const bool  container = picture.packetRow >= 0;
    if (container && picture.packetRow != topRow)
      continue;
    const int row = container ? childRow : topRow;
    // A packet row itself (childRow -1) seeks to its picture too.
    if ((container && childRow < 0) ||
        std::find(picture.nalRows.begin(), picture.nalRows.end(), row) != picture.nalRows.end())
      return int(display);
  }
  return std::nullopt;
}

} // namespace bda::integration
