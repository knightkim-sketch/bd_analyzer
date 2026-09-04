// One call that takes two raw YUV frames and gives back a frame of ME results.
//
// Deliberately a plain function over plain data: it has to run on a worker thread, so it touches
// no widgets, no signals and nothing that belongs to the GUI thread. QByteArray, Size and
// PixelFormatYUV are values here, copied into the request before it leaves the GUI thread.
//
// Part of the src/integration boundary.
#pragma once

#include <QByteArray>

#include <common/Typedef.h>
#include <video/yuv/PixelFormatYUV.h>

#include "me/MeTypes.h"

namespace bda::integration
{

struct MeRunRequest
{
  QByteArray currentFrame;   // raw YUV of the displayed frame
  QByteArray referenceFrame; // raw YUV of frame (current - frameInterval)

  video::yuv::PixelFormatYUV format;
  Size                       frameSize;

  int frameIdx    = 0;
  int refFrameIdx = 0;

  /* Carries the cancel token. The token itself lives with whoever started the run - the request is
   * copied onto the worker thread, and copying an atomic would defeat the point.
   */
  me::MeParams params;
};

/* Run the estimate. Safe to call from a worker thread.
 *
 * Fails softly: an unreadable format, a truncated frame or an algorithm that is not implemented
 * yet all come back as a result with `error` set, so the caller can put the reason on screen
 * instead of showing an empty overlay and leaving the user to guess.
 */
me::MeFrameResult runMe(const MeRunRequest &request);

/* Whether a frame index pair is usable, given how many frames the item has.
 *
 * Separate from runMe() because the UI needs the answer before it has any frame data - it decides
 * whether to start a run at all, and what to say when it does not. A negative interval reaches
 * forward, which is why this cannot just check for a negative reference index.
 */
bool meFrameRangeOk(int frameIdx, int frameInterval, int frameCount, std::string *reason = nullptr);

/* The reference index for a displayed frame and a signed interval. */
inline int meReferenceFrameIdx(int frameIdx, int frameInterval)
{
  return frameIdx - frameInterval;
}

} // namespace bda::integration
