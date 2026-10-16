#include "IMotionEstimator.h"
#include "OdysseyClosedLoopMe.h"
#include "OdysseyOpenLoopMe.h"
#include "SvtIntegerMe.h"

namespace bda::me
{

MotionEstimatorPtr makeEstimator(Algorithm algorithm)
{
  switch (algorithm)
  {
  case Algorithm::SvtIntegerMe:
    return std::make_unique<SvtIntegerMe>();
  case Algorithm::OdysseyOpenLoop:
    return std::make_unique<OdysseyOpenLoopMe>();
  case Algorithm::OdysseyClosedLoop:
    return std::make_unique<OdysseyClosedLoopMe>();
  case Algorithm::OdysseyOpenLoopAdv:
    /* Not a frame-pair estimator: it analyses a whole GF structure in odyssey's order through
     * estimateOdysseyOpenLoopAdv() (OdysseyOpenLoopAdvMe.h), which the GOP run path calls.
     */
    return nullptr;
  }
  return nullptr;
}

} // namespace bda::me
