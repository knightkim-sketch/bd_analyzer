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

bool isGmType(int typeId) { return typeId >= kGmAcceptedTypeId && typeId <= kGmWindowTypeId; }

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
  return changed;
}

void clearGmStatTypes(stats::StatisticsData &data)
{
  auto &types = data.getStatisticsTypes();
  types.erase(std::remove_if(types.begin(), types.end(),
                             [](const stats::StatisticsType &t) { return isGmType(t.typeID); }),
              types.end());
  for (int id = kGmAcceptedTypeId; id <= kGmWindowTypeId; ++id)
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
    const int x0 = w.x0 << frame.n, y0 = w.y0 << frame.n;
    const int x1 = w.winI == me::kGmGrid - 1 ? width : (w.x0 + w.w) << frame.n;
    const int y1 = w.winJ == me::kGmGrid - 1 ? height : (w.y0 + w.h) << frame.n;
    const auto px = static_cast<unsigned short>(x0), py = static_cast<unsigned short>(y0);
    const auto pw = static_cast<unsigned short>(x1 - x0), ph = static_cast<unsigned short>(y1 - y0);

    data.at(kGmWindowTypeId).addBlockValue(px, py, pw, ph, int(w.acceptX) + int(w.acceptY));
    if (w.acceptX || w.acceptY)
      data.at(kGmAcceptedTypeId)
          .addBlockVector(px, py, pw, ph, w.acceptX ? w.dxQpel : 0, w.acceptY ? w.dyQpel : 0);
    if (!w.acceptX || !w.acceptY)
      data.at(kGmRawTypeId).addBlockVector(px, py, pw, ph, w.dxQpel, w.dyQpel);
  }
}

} // namespace bda::integration
