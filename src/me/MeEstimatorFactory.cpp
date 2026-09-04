#include "IMotionEstimator.h"
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
    /* Second phase. It needs a reconstruction to search against and an open-loop result to centre
     * on, so it cannot be dropped in as a third independent estimator - returning null here is
     * what lets the UI grey the choice out instead of offering something that would produce an
     * empty overlay.
     */
    return nullptr;
  }
  return nullptr;
}

} // namespace bda::me
