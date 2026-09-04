// Regression: ME results reach the screen through the upstream statistics overlay, not through a
// drawer of our own.
//
// This is the integration point for the whole feature, so it is checked against the real
// stats::StatisticsData rather than a stand-in:
//
//   * the vector types are registered with a vector scale of 8, because the ME core reports
//     eighth-pel and the overlay divides by that before drawing an arrow;
//   * the block-size checkboxes are the only switch - syncMeStatTypes() adds and removes types to
//     match, and removing one drops its cached data with it;
//   * both costs survive the trip, on separate types, because a statistics block holds one value
//     and the native cost and the common SAD are not on the same scale;
//   * the ME types do not collide with the decoder's own (AV1 motion vectors are types 24 and 25).
#include <QApplication>
#include <QSettings>
#include <iostream>
#include <string>
#include <vector>

#include "integration/MeStatisticsAdapter.h"
#include "me/IMotionEstimator.h"
#include "me/MePlane.h"
#include "statistics/StatisticsData.h"
#include "statistics/StatisticsType.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
void checkEq(long long got, long long want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (got " << got << ", want " << want << ")";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

const stats::StatisticsType *findType(stats::StatisticsData &data, int id)
{
  for (const auto &t : data.getStatisticsTypes())
    if (t.typeID == id)
      return &t;
  return nullptr;
}

constexpr int kW = 128;
constexpr int kH = 128;

bda::me::MePlane makePlane(int shift)
{
  std::vector<std::uint8_t> d(static_cast<std::size_t>(kW) * kH);
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
    {
      const int sx = x - shift;
      d[static_cast<std::size_t>(y) * kW + x] =
          static_cast<std::uint8_t>(((sx * sx + y * y) / 32 + sx * 3 + y * 5) & 0xFF);
    }
  return bda::me::MePlane::fromLuma8(d.data(), kW, kH, kW, bda::me::kOdysseyPadFull);
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  using namespace bda;
  using me::Algorithm;
  using me::BlockSize;

  stats::StatisticsData data;
  data.setFrameSize(Size(kW, kH));

  // --- registration follows the block size selection ------------------------------------------
  {
    me::BlockSizeSet only64;
    only64.add(BlockSize::Blk64);
    integration::syncMeStatTypes(data, Algorithm::SvtIntegerMe, only64);

    const auto *vec = findType(data, integration::meVectorTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk64));
    check(vec != nullptr, "the 64x64 vector type is registered");
    if (vec)
    {
      checkEq(vec->vectorScale, 8, "with a vector scale of 8 for eighth-pel vectors");
      check(vec->hasVectorData, "and it is a vector type");
      check(!vec->uiGroup.isEmpty(), "grouped in the panel: " + vec->uiGroup.toStdString());
    }

    check(findType(data, integration::meCostTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk64)) != nullptr,
          "the native cost type is registered alongside");
    check(findType(data, integration::meCommonSadTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk64)) !=
              nullptr,
          "and the common SAD type, separately - one value per block means they cannot share");

    check(findType(data, integration::meVectorTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk8)) ==
              nullptr,
          "an unselected size is not registered");

    // Turning a size on adds it; turning it off takes it away again.
    me::BlockSizeSet both = only64;
    both.add(BlockSize::Blk16);
    integration::syncMeStatTypes(data, Algorithm::SvtIntegerMe, both);
    check(findType(data, integration::meVectorTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk16)) !=
              nullptr,
          "checking a size registers it");

    integration::syncMeStatTypes(data, Algorithm::SvtIntegerMe, only64);
    check(findType(data, integration::meVectorTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk16)) ==
              nullptr,
          "unchecking it removes it again");
  }

  // --- no collision with the decoder's own types ----------------------------------------------
  {
    // The dav1d path registers motion vectors as 24 and 25; ours must be nowhere near.
    bool clash = false;
    for (const auto &t : data.getStatisticsTypes())
      if (t.typeID == 24 || t.typeID == 25)
        clash = true;
    check(!clash, "the ME types do not occupy the decoder's motion vector IDs");
    check(integration::meVectorTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk8) >= 200,
          "they start above the decoder's range");
    check(integration::meVectorTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk64) !=
              integration::meVectorTypeId(Algorithm::OdysseyOpenLoop, BlockSize::Blk64),
          "and the two algorithms get different IDs for the same size");
  }

  // --- a real estimate lands in the overlay ---------------------------------------------------
  {
    auto estimator = me::makeEstimator(Algorithm::SvtIntegerMe);
    check(estimator != nullptr, "the factory builds the SVT estimator");

    me::MePictureSet pictures;
    pictures.current     = makePlane(0);
    pictures.reference   = makePlane(3);
    pictures.refDistance = 1;

    me::MeParams params;
    params.staticBypass = false;
    params.blockSizes   = me::BlockSizeSet();
    params.blockSizes.add(BlockSize::Blk64);

    integration::syncMeStatTypes(data, Algorithm::SvtIntegerMe, params.blockSizes);
    data.setFrameIndex(0);

    const auto result = estimator->estimateFrame(pictures, params);
    check(result.ok(), "the estimate runs: " + result.error);

    integration::fillMeStatistics(data, result);

    const int vecId    = integration::meVectorTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk64);
    const int costId   = integration::meCostTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk64);
    const int commonId = integration::meCommonSadTypeId(Algorithm::SvtIntegerMe, BlockSize::Blk64);

    check(data.hasDataForTypeID(vecId), "the vectors reached the overlay");
    check(data.hasDataForTypeID(costId), "the native costs reached the overlay");
    check(data.hasDataForTypeID(commonId), "the common SAD reached the overlay");

    const auto vectors = data.getFrameTypeData(vecId);
    checkEq(static_cast<long long>(vectors.vectorData.size()), 4, "one vector per 64x64 block");

    const auto costs  = data.getFrameTypeData(costId);
    const auto common = data.getFrameTypeData(commonId);
    checkEq(static_cast<long long>(costs.valueData.size()), 4, "one native cost per block");
    checkEq(static_cast<long long>(common.valueData.size()), 4, "one common SAD per block");

    // The values the overlay holds must be the ones the estimator produced, not a rescaled copy.
    bool matched = false;
    for (const auto &b : result.blocks)
      if (b.block.x == 0 && b.block.y == 0)
      {
        for (const auto &v : vectors.vectorData)
          if (v.pos[0] == 0 && v.pos[1] == 0)
            matched = v.point[0].x == b.mv.x && v.point[0].y == b.mv.y;
      }
    check(matched, "the stored vector is the estimator's, in eighth-pel");
  }

  // --- clearing takes everything of ours away, and nothing else -------------------------------
  {
    stats::StatisticsType foreign(24, "Motion Vector 0", 4);
    foreign.setInitialState();
    data.addStatType(foreign);

    integration::clearMeStatTypes(data);

    bool anyMeLeft = false;
    for (const auto &t : data.getStatisticsTypes())
      if (t.typeID >= integration::kMeStatTypeBase)
        anyMeLeft = true;
    check(!anyMeLeft, "clearing removes every ME type");
    check(findType(data, 24) != nullptr, "and leaves the decoder's own type alone");
  }

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
