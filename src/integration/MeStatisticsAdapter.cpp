#include "MeStatisticsAdapter.h"

#include <algorithm>

#include "statistics/StatisticsData.h"
#include "statistics/StatisticsType.h"

namespace bda::integration
{
namespace
{

using me::Algorithm;
using me::BlockSize;

int algorithmSlot(Algorithm algorithm)
{
  switch (algorithm)
  {
  case Algorithm::SvtIntegerMe:
    return 0;
  case Algorithm::OdysseyOpenLoop:
    return 1;
  case Algorithm::OdysseyClosedLoop:
    return 2;
  }
  return 0;
}

const char *algorithmShortName(Algorithm algorithm)
{
  switch (algorithm)
  {
  case Algorithm::SvtIntegerMe:
    return "SVT integer";
  case Algorithm::OdysseyClosedLoop:
    return "odyssey closed-loop";
  case Algorithm::OdysseyOpenLoop:
    break;
  }
  return "odyssey open-loop";
}

const char *nativeCostName(Algorithm algorithm)
{
  // Kept next to the type names because a cost column without its metric is misleading: these
  // three numbers are not on the same scale.
  switch (algorithm)
  {
  case Algorithm::SvtIntegerMe:
    return "SAD";
  case Algorithm::OdysseyOpenLoop:
    return "SSE + rate^2";
  case Algorithm::OdysseyClosedLoop:
    return "SAD + rate";
  }
  return "cost";
}

/* One colour per algorithm, so an overlay of two of them is readable at a glance. The bitstream's
 * own motion vectors are already red (type 24) and blue (type 25), so both of these avoid those.
 */
Color algorithmColor(Algorithm algorithm)
{
  switch (algorithm)
  {
  case Algorithm::SvtIntegerMe:
    return Color(0, 200, 0); // green
  case Algorithm::OdysseyOpenLoop:
    return Color(255, 160, 0); // amber
  case Algorithm::OdysseyClosedLoop:
    return Color(180, 0, 220); // violet
  }
  return Color(255, 255, 255);
}

int sizeSlot(BlockSize size)
{
  return static_cast<int>(size);
}

bool isMeTypeId(int typeId)
{
  return typeId >= kMeStatTypeBase && typeId < kMeStatTypeBase + 3 * 16;
}

void eraseType(stats::StatisticsData &data, int typeId)
{
  auto &types = data.getStatisticsTypes();
  types.erase(std::remove_if(types.begin(),
                             types.end(),
                             [typeId](const stats::StatisticsType &t) { return t.typeID == typeId; }),
              types.end());
  data.eraseDataForTypeID(typeId);
}

bool hasType(stats::StatisticsData &data, int typeId)
{
  const auto &types = data.getStatisticsTypes();
  return std::any_of(types.begin(), types.end(), [typeId](const stats::StatisticsType &t) {
    return t.typeID == typeId;
  });
}

} // namespace

int meVectorTypeId(Algorithm algorithm, BlockSize size)
{
  return kMeStatTypeBase + algorithmSlot(algorithm) * 16 + sizeSlot(size);
}

int meCostTypeId(Algorithm algorithm, BlockSize size)
{
  return kMeStatTypeBase + algorithmSlot(algorithm) * 16 + 4 + sizeSlot(size);
}

int meCommonSadTypeId(Algorithm algorithm, BlockSize size)
{
  return kMeStatTypeBase + algorithmSlot(algorithm) * 16 + 8 + sizeSlot(size);
}

std::string meGroupName(Algorithm algorithm)
{
  return std::string("ME (") + algorithmShortName(algorithm) + ")";
}

std::string meVectorTypeName(Algorithm algorithm, BlockSize size)
{
  const int px = me::blockSizeInPixels(size);
  return std::string("ME ") + algorithmShortName(algorithm) + " MV " + std::to_string(px) + "x" +
         std::to_string(px);
}

std::string meCostTypeName(Algorithm algorithm, BlockSize size)
{
  const int px = me::blockSizeInPixels(size);
  return std::string("ME ") + algorithmShortName(algorithm) + " " + nativeCostName(algorithm) + " " +
         std::to_string(px) + "x" + std::to_string(px);
}

std::string meCommonSadTypeName(Algorithm algorithm, BlockSize size)
{
  const int px = me::blockSizeInPixels(size);
  return std::string("ME ") + algorithmShortName(algorithm) + " common SAD " + std::to_string(px) +
         "x" + std::to_string(px);
}

void syncMeStatTypes(stats::StatisticsData  &data,
                     Algorithm               algorithm,
                     const me::BlockSizeSet &sizes)
{
  for (int s = 0; s < me::kBlockSizeCount; ++s)
  {
    const auto size      = static_cast<BlockSize>(s);
    const int  vectorId  = meVectorTypeId(algorithm, size);
    const int  costId    = meCostTypeId(algorithm, size);
    const int  commonId  = meCommonSadTypeId(algorithm, size);
    const bool wanted    = sizes.contains(size);

    if (!wanted)
    {
      eraseType(data, vectorId);
      eraseType(data, costId);
      eraseType(data, commonId);
      continue;
    }

    if (!hasType(data, vectorId))
    {
      /* vectorScale 8 because the ME core reports eighth-pel throughout - including for the
       * estimators that only ever produce whole pixels. The overlay divides by this before
       * drawing, so getting it wrong scales every arrow.
       */
      stats::StatisticsType type(vectorId, QString::fromStdString(meVectorTypeName(algorithm, size)), 8);
      type.description = QString::fromStdString(
          std::string("Reproduced ") + algorithmShortName(algorithm) + " motion vector");
      type.vectorStyle       = stats::LineDrawStyle({algorithmColor(algorithm), 2.0, stats::Pattern::Solid});
      type.scaleVectorToZoom = true;
      type.setInitialState();
      type.uiGroup = QString::fromStdString(meGroupName(algorithm));
      data.addStatType(type);
    }

    if (!hasType(data, costId))
    {
      /* The cost type carries the algorithm's own metric as the block value. The common metric
       * travels with it in the same record so the block info pane can put the two side by side -
       * see fillMeStatistics().
       */
      stats::StatisticsType type(costId, QString::fromStdString(meCostTypeName(algorithm, size)));
      type.description = QString::fromStdString(
          std::string("Native cost (") + nativeCostName(algorithm) +
          ") of the reproduced vector. Not comparable across algorithms - the common SAD is.");
      type.setInitialState();
      type.uiGroup = QString::fromStdString(meGroupName(algorithm));
      data.addStatType(type);
    }

    if (!hasType(data, commonId))
    {
      stats::StatisticsType type(commonId,
                                 QString::fromStdString(meCommonSadTypeName(algorithm, size)));
      type.description = "Plain SAD of the chosen prediction against the original pictures - no "
                         "rate, no subsampling. Recomputed identically for every algorithm, so "
                         "this is the column to compare across them.";
      type.setInitialState();
      type.uiGroup = QString::fromStdString(meGroupName(algorithm));
      data.addStatType(type);
    }
  }
}

void clearMeStatTypes(stats::StatisticsData &data)
{
  auto &types = data.getStatisticsTypes();

  std::vector<int> doomed;
  for (const auto &t : types)
    if (isMeTypeId(t.typeID))
      doomed.push_back(t.typeID);

  for (const auto id : doomed)
    eraseType(data, id);
}

void fillMeStatistics(stats::StatisticsData &data, const me::MeFrameResult &result)
{
  if (!result.ok())
    return;

  for (const auto &block : result.blocks)
  {
    const int vectorId = meVectorTypeId(result.algorithm, block.block.size);
    const int costId   = meCostTypeId(result.algorithm, block.block.size);

    // A size the user switched off has no type; dropping the record is better than conjuring one.
    if (!hasType(data, vectorId))
      continue;

    const auto x = static_cast<unsigned short>(block.block.x);
    const auto y = static_cast<unsigned short>(block.block.y);
    const auto w = static_cast<unsigned short>(me::blockSizeInPixels(block.block.size));

    data.at(vectorId).addBlockVector(x, y, w, w, block.mv.x, block.mv.y);

    /* Both costs are reported. The native one is the block value - that is what the algorithm
     * actually minimised - and the common SAD goes in as well so the two can be read together.
     * Showing only one of them was considered and rejected: without the native number a
     * disagreement between algorithms cannot be explained, and without the common one the
     * algorithms cannot be compared at all.
     */
    if (hasType(data, costId))
      data.at(costId).addBlockValue(x, y, w, w, static_cast<int>(block.nativeCost));

    const int commonId = meCommonSadTypeId(result.algorithm, block.block.size);
    if (hasType(data, commonId))
      data.at(commonId).addBlockValue(x, y, w, w, static_cast<int>(block.commonSad));
  }
}

} // namespace bda::integration
