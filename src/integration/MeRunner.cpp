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
    // The closed-loop estimator is the second phase; saying so beats an empty overlay.
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

  return estimator->estimateFrame(pictures, request.params);
}

} // namespace bda::integration
