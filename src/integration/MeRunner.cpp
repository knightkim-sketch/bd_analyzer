#include "MeRunner.h"

#include <string>

#include "MeFrameSource.h"
#include "me/IMotionEstimator.h"

namespace bda::integration
{

bool meFrameRangeOk(int frameIdx, int frameInterval, int frameCount, std::string *reason)
{
  if (frameCount <= 1)
  {
    if (reason)
      *reason = "the item has only one frame, so there is nothing to match against";
    return false;
  }
  if (frameInterval == 0)
  {
    if (reason)
      *reason = "a frame interval of 0 would match the frame against itself";
    return false;
  }

  const int refIdx = meReferenceFrameIdx(frameIdx, frameInterval);
  if (refIdx < 0 || refIdx >= frameCount)
  {
    /* Both ends matter because the interval is signed: a positive one runs off the start of the
     * sequence, a negative one off the end.
     */
    /* Named after the frame the user is looking at, not after the reference index it computed.
     * "frame -1 is outside the sequence" is true and unreadable: there is no frame -1 on screen,
     * and it does not say what to do about it. This is the state every freshly opened file starts
     * in - frame 0 with a backward interval - so it is the message that gets read most.
     */
    if (reason)
      *reason = "frame " + std::to_string(frameIdx) + " has no reference at interval " +
                std::to_string(frameInterval) + " (that would be frame " + std::to_string(refIdx) +
                "). Move to another frame, or change the interval.";
    return false;
  }
  return true;
}

me::MeFrameResult runMe(const MeRunRequest &request)
{
  me::MeFrameResult failed;
  failed.frameIdx    = request.frameIdx;
  failed.refFrameIdx = request.refFrameIdx;
  failed.algorithm   = request.params.algorithm;

  auto estimator = me::makeEstimator(request.params.algorithm);
  if (!estimator)
  {
    failed.error = "this algorithm is not implemented yet";
    return failed;
  }

  if (request.params.blockSizes.empty())
  {
    failed.error = "no block size is selected";
    return failed;
  }

  /* Padding is the full-resolution 64 both estimators want: odyssey pads its full-resolution
   * planes to NUM_PAD_PIXELS, and SVT's search reaches past the picture edge as well. Being
   * generous here costs a little memory and removes a class of edge-block bugs.
   */
  me::MePictureSet pictures;
  pictures.current =
      lumaPlaneFromRawYuv(request.currentFrame, request.format, request.frameSize, me::kOdysseyPadFull);
  pictures.reference = lumaPlaneFromRawYuv(
      request.referenceFrame, request.format, request.frameSize, me::kOdysseyPadFull);
  pictures.frameIdx    = request.frameIdx;
  pictures.refFrameIdx = request.refFrameIdx;

  /* The signed distance, not the absolute one. Both encoders scale their search by how far the
   * reference is, and the sign is what says which side it is on.
   */
  pictures.refDistance = request.params.frameInterval;

  if (pictures.current.empty() || pictures.reference.empty())
  {
    failed.error = "the luma plane could not be read - a packed pixel format, or a truncated frame";
    return failed;
  }

  if (std::string why; !estimator->canRun(pictures, &why))
  {
    failed.error = why;
    return failed;
  }

  /* odyssey closed-loop centres every block on another estimate, as the encoder centres it on its
   * open-loop result. The seed runs first on the same pair, for every block size so each block
   * finds its co-located vector whatever sizes are shown; it shares the cancel token, and the
   * progress bar follows the closed-loop pass that comes after it.
   */
  if (request.params.algorithm == me::Algorithm::OdysseyClosedLoop && request.params.seedAlgorithm &&
      *request.params.seedAlgorithm != me::Algorithm::OdysseyClosedLoop)
  {
    auto seedEstimator = me::makeEstimator(*request.params.seedAlgorithm);
    if (!seedEstimator)
    {
      failed.error = "the start vector algorithm cannot run on a frame pair";
      return failed;
    }
    auto seedParams          = request.params;
    seedParams.algorithm     = *request.params.seedAlgorithm;
    seedParams.blockSizes    = me::BlockSizeSet::all();
    seedParams.seedAlgorithm = std::nullopt;
    seedParams.progress      = nullptr;
    const auto seed          = seedEstimator->estimateFrame(pictures, seedParams);
    if (seed.cancelled || !seed.error.empty())
    {
      failed.cancelled = seed.cancelled;
      failed.error     = seed.error.empty() ? std::string() : "start vectors: " + seed.error;
      return failed;
    }
    auto params = request.params;
    params.seed = &seed;
    return estimator->estimateFrame(pictures, params);
  }

  return estimator->estimateFrame(pictures, request.params);
}

me::OdyAdvResult runMeAdv(const MeAdvRunRequest &request)
{
  // The same 8-bit luma the frame-pair estimators search (10-bit is shifted down, as odyssey does).
  const auto load = [&request](int poc) -> me::MePlane {
    if (poc < 0 || poc >= int(request.frames.size()))
      return {};
    return lumaPlaneFromRawYuv(request.frames[std::size_t(poc)], request.format, request.frameSize,
                               me::kOdysseyPadFull);
  };
  return me::estimateOdysseyOpenLoopAdv(load, request.frameCount, request.lastPoc, request.params);
}

} // namespace bda::integration
