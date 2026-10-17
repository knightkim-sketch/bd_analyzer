#include "GmStatisticsAdapter.h"

#include <algorithm>

#include "statistics/StatisticsType.h"

namespace bda::integration
{

namespace
{

bool hasType(stats::StatisticsData &data, int typeId)
{
  const auto &types = data.getStatisticsTypes();
  return std::any_of(types.begin(), types.end(),
                     [typeId](const stats::StatisticsType &t) { return t.typeID == typeId; });
}

bool isGmType(int typeId) { return typeId >= kGmAcceptedTypeId && typeId <= kGmLastTypeId; }

stats::StatisticsType vectorType(int id, const QString &name, const Color &colour, int width)
{
  // vectorScale 4: the estimator reports quarter-pel at source resolution.
  stats::StatisticsType type(id, name, 4);
  // Not scaled to zoom, for the reason MeStatisticsAdapter gives: at 1:1 a scaled 2 px line is
  // antialiased away.
  type.vectorStyle = stats::LineDrawStyle({colour, double(width), stats::Pattern::Solid});
  type.render      = true;
  type.setInitialState();
  type.uiGroup = "Global motion";
  return type;
}

} // namespace

bool syncGmStatTypes(stats::StatisticsData &data)
{
  bool changed = false;
  if (!hasType(data, kGmAcceptedTypeId))
  {
    auto type = vectorType(kGmAcceptedTypeId, "GM accepted", Color(0, 220, 255), 3);
    type.description = "Global motion vector per window, as odyssey would receive it: a rejected "
                       "axis is 0. Drawn from the window centre.";
    data.addStatType(type);
    changed = true;
  }
  if (!hasType(data, kGmRawTypeId))
  {
    auto type = vectorType(kGmRawTypeId, "GM rejected (raw)", Color(150, 150, 150), 2);
    type.description = "The search's answer for windows where an axis was rejected - what the "
                       "gates turned down.";
    data.addStatType(type);
    changed = true;
  }
  if (!hasType(data, kGmWindowTypeId))
  {
    stats::StatisticsType type(kGmWindowTypeId, "GM window (accepted axes)");
    type.description = "The 4x4 window grid. Value: number of accepted axes, 0..2.";
    type.render      = true;
    type.setInitialState();
    // The grid only: a colour fill over the whole picture would hide what the arrows are about.
    type.renderValueData = false;
    type.renderGrid      = true;
    type.gridStyle       = stats::LineDrawStyle({Color(0, 220, 255), 1.0, stats::Pattern::Dash});
    type.uiGroup         = "Global motion";
    data.addStatType(type);
    changed = true;
  }
  if (!hasType(data, kGmInlierTypeId))
  {
    stats::StatisticsType type(kGmInlierTypeId, "GM model inlier",
                               stats::color::ColorMapper({0, 1}, Color(230, 40, 40), Color(40, 200, 80)));
    type.description = "The RANSAC global model's verdict per block of the ME result it was fitted to: "
                       "1 (green) follows the model, 0 (red) does not - local motion, or a mismatch.";
    type.render          = true;
    type.renderValueData = true;
    type.alphaFactor     = 35;
    type.setInitialState();
    type.uiGroup = "Global motion";
    data.addStatType(type);
    changed = true;
  }
  if (!hasType(data, kGmModelTypeId))
  {
    auto type = vectorType(kGmModelTypeId, "GM model vector", Color(255, 200, 0), 2);
    type.vectorScale = 8; // eighth-pel, as the model is evaluated
    type.description = "The RANSAC global model's vector at the centre of each 64x64 block.";
    data.addStatType(type);
    changed = true;
  }
  return changed;
}

void clearGmStatTypes(stats::StatisticsData &data)
{
  auto &types = data.getStatisticsTypes();
  types.erase(std::remove_if(types.begin(), types.end(),
                             [](const stats::StatisticsType &t) { return isGmType(t.typeID); }),
              types.end());
  for (int id = kGmAcceptedTypeId; id <= kGmLastTypeId; ++id)
    data.eraseDataForTypeID(id);
}

void fillGmStatistics(stats::StatisticsData &  data,
                      const me::GmFrameResult &frame,
                      const int                width,
                      const int                height)
{
  for (int id = kGmAcceptedTypeId; id <= kGmWindowTypeId; ++id)
    data.eraseDataForTypeID(id);
  if (!frame.ok())
    return;

  for (const auto &w : frame.windows)
  {
    int x0{}, y0{}, ww{}, wh{};
    me::gmWindowSourceRect(frame, w, width, height, x0, y0, ww, wh);
    const auto px = static_cast<unsigned short>(x0), py = static_cast<unsigned short>(y0);
    const auto pw = static_cast<unsigned short>(ww), ph = static_cast<unsigned short>(wh);

    data.at(kGmWindowTypeId).addBlockValue(px, py, pw, ph, int(w.acceptX) + int(w.acceptY));
    if (w.acceptX || w.acceptY)
      data.at(kGmAcceptedTypeId)
          .addBlockVector(px, py, pw, ph, w.acceptX ? w.dxQpel : 0, w.acceptY ? w.dyQpel : 0);
    if (!w.acceptX || !w.acceptY)
      data.at(kGmRawTypeId).addBlockVector(px, py, pw, ph, w.dxQpel, w.dyQpel);
  }
}

void fillGmModelStatistics(stats::StatisticsData &  data,
                           const me::GmModelResult *model,
                           const int                width,
                           const int                height)
{
  data.eraseDataForTypeID(kGmInlierTypeId);
  data.eraseDataForTypeID(kGmModelTypeId);
  if (model == nullptr || !model->ok())
    return;
  for (std::size_t i = 0; i < model->blocks.size(); ++i)
  {
    const auto &b = model->blocks[i];
    const auto  s = static_cast<unsigned short>(me::blockSizeInPixels(b.size));
    data.at(kGmInlierTypeId)
        .addBlockValue(static_cast<unsigned short>(b.x), static_cast<unsigned short>(b.y), s, s, model->inlier[i]);
  }
  for (int y = 0; y < height; y += 64)
    for (int x = 0; x < width; x += 64)
    {
      const int  w = std::min(64, width - x), h = std::min(64, height - y);
      const auto mv = me::gmModelMvAt(*model, x + w / 2.0, y + h / 2.0);
      data.at(kGmModelTypeId)
          .addBlockVector(static_cast<unsigned short>(x), static_cast<unsigned short>(y), static_cast<unsigned short>(w),
                          static_cast<unsigned short>(h), mv.x, mv.y);
    }
}

} // namespace bda::integration
