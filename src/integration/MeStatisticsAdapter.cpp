#include "MeStatisticsAdapter.h"

#include <algorithm>

#include "statistics/BlockInfo.h"
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

/* One row per kind, not one row per algorithm.
 *
 * The panel gives every uiGroup a single checkbox that sets `render` on every type in it. Putting
 * the vectors and the two cost overlays in one group meant ticking "show the vectors" also
 * switched on two block-value overlays, which paint a filled rectangle over every block - the
 * arrows were still drawn, underneath a colour-mapped blanket. Splitting by kind keeps the block
 * sizes collapsed into one row each, which is what the grouping is for, and leaves each row doing
 * one thing.
 */
std::string meGroupName(Algorithm algorithm, MeStatKind kind)
{
  const std::string base = std::string("ME ") + algorithmShortName(algorithm) + " - ";
  switch (kind)
  {
  case MeStatKind::NativeCost:
    return base + nativeCostName(algorithm);
  case MeStatKind::CommonSad:
    return base + "common SAD";
  case MeStatKind::Vector:
    break;
  }
  return base + "MV";
}

std::string meGroupName(Algorithm algorithm)
{
  return meGroupName(algorithm, MeStatKind::Vector);
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

bool syncMeStatTypes(stats::StatisticsData  &data,
                     Algorithm               algorithm,
                     const me::BlockSizeSet &sizes)
{
  bool changed = false;
  for (int s = 0; s < me::kBlockSizeCount; ++s)
  {
    const auto size      = static_cast<BlockSize>(s);
    const int  vectorId  = meVectorTypeId(algorithm, size);
    const int  costId    = meCostTypeId(algorithm, size);
    const int  commonId  = meCommonSadTypeId(algorithm, size);
    const bool wanted    = sizes.contains(size);

    if (!wanted)
    {
      changed = changed || hasType(data, vectorId);
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
      /* Width 2 and NOT scaled to zoom, which is how the decoder registers the bitstream's own
       * motion vectors (decoderDav1d.cpp:803). The painter reads scaleVectorToZoom as
       * `width * zoomFactor / 8`, so at the 1:1 zoom this is normally viewed at it turned a 2 pixel
       * line into a quarter of a pixel - drawn, antialiased away to nothing, and reported as "the
       * MV lines are not visible".
       */
      type.vectorStyle = stats::LineDrawStyle({algorithmColor(algorithm), 2.0, stats::Pattern::Solid});
      /* Drawn as soon as it exists. The base StatisticsType leaves `render` false, which is right
       * for a decoder that registers thirty types at once - but here the user asked for exactly
       * this overlay by ticking the box, and making them find a second checkbox to actually see it
       * is a step with no decision in it. The cost types stay off: they paint over the picture.
       */
      type.render = true;
      type.setInitialState();
      type.uiGroup = QString::fromStdString(meGroupName(algorithm, MeStatKind::Vector));
      data.addStatType(type);
      changed = true;
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
      type.uiGroup = QString::fromStdString(meGroupName(algorithm, MeStatKind::NativeCost));
      data.addStatType(type);
      changed = true;
    }

    if (!hasType(data, commonId))
    {
      stats::StatisticsType type(commonId,
                                 QString::fromStdString(meCommonSadTypeName(algorithm, size)));
      type.description = "Plain SAD of the chosen prediction against the original pictures - no "
                         "rate, no subsampling. Recomputed identically for every algorithm, so "
                         "this is the column to compare across them.";
      type.setInitialState();
      type.uiGroup = QString::fromStdString(meGroupName(algorithm, MeStatKind::CommonSad));
      data.addStatType(type);
      changed = true;
    }
  }
  return changed;
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

std::string meBlockRowName(Algorithm algorithm, BlockSize size)
{
  const int px = me::blockSizeInPixels(size);
  return std::string("ME ") + algorithmShortName(algorithm) + " " + std::to_string(px) + "x" +
         std::to_string(px);
}

void mergeMeBlockInfoEntries(stats::BlockInfo &info)
{
  const auto find = [&info](int typeId) {
    return std::find_if(info.entries.begin(),
                        info.entries.end(),
                        [typeId](const stats::BlockInfoEntry &e) { return e.typeID == typeId; });
  };

  for (int a = 0; a < 3; ++a)
  {
    const auto algorithm = static_cast<Algorithm>(a);
    for (int s = 0; s < me::kBlockSizeCount; ++s)
    {
      const auto size = static_cast<BlockSize>(s);

      /* The vector row is the one that survives: it is what the feature is for, and it is the only
       * one of the three that is drawn on the picture, so the pane and the overlay agree on which
       * row the arrow belongs to.
       */
      auto vector = find(meVectorTypeId(algorithm, size));
      if (vector == info.entries.end())
        continue;
      vector->typeName = QString::fromStdString(meBlockRowName(algorithm, size));

      /* Named in the text rather than left as a bare number, because the two are not on the same
       * scale - the native cost is whatever that estimator minimised, the common SAD is the
       * comparable one. A row reading "477  477" would invite exactly the wrong conclusion.
       */
      if (auto cost = find(meCostTypeId(algorithm, size)); cost != info.entries.end())
      {
        vector->valueText += "   " + QString::fromStdString(nativeCostName(algorithm)) + " " +
                             cost->valueText;
        info.entries.erase(cost);
      }
      if (auto common = find(meCommonSadTypeId(algorithm, size)); common != info.entries.end())
      {
        vector->valueText += "   common SAD " + common->valueText;
        info.entries.erase(common);
      }
    }
  }
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
