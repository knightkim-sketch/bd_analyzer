// Read the luma plane of one frame from a Y4M or raw planar YUV file, without Qt.
//
// The global motion CLI and its tests run headless and the estimator only looks at luma, so this
// reads exactly that: planar 4:2:0 (or monochrome) at 8 or 10 bits. 10-bit samples are shifted
// down to 8 bits the way odyssey's open-loop ME does (MePlane::fromLuma10AsShifted8).
//
// Anything else - 4:2:2, 4:4:4, interleaved formats, other bit depths - is refused with a reason,
// not guessed at: reading a frame with the wrong frame size shifts every later frame silently.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "MePlane.h"

namespace bda::me
{

class YuvLumaReader
{
public:
  /* A .y4m file, recognised by its "YUV4MPEG2 " magic. */
  bool openY4m(const std::string &path, std::string &error);
  /* Raw planar 4:2:0, which carries no header: the size and depth have to be given. */
  bool openRaw(const std::string &path, int width, int height, int bitDepth, std::string &error);

  int width() const { return this->w; }
  int height() const { return this->h; }
  int bitDepth() const { return this->depth; }
  int frameCount() const { return int(this->frameOffsets.size()); }

  /* Luma of frame k (0-based, file order = display order) as an 8-bit plane with the given border.
   * Empty with error set when the frame cannot be read.
   */
  MePlane readLuma(int k, int pad, std::string &error);

private:
  std::size_t frameBytes() const;

  std::ifstream             file;
  int                       w{}, h{}, depth{8};
  bool                      hasChroma{true};
  std::vector<std::int64_t> frameOffsets; //!< Byte offset of each frame's luma samples.
};

} // namespace bda::me
