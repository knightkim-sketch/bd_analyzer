// The stabilisation (DIS) warp applied to raw YUV frames as they are read, for "Apply to view" in the
// Global motion window.
//
// The table maps an absolute frame index of the item to the inverse-mapping warp of that frame
// (src/filter/Stabilizer, StabilizerFrame::warp). It is computed once over the whole sequence and
// shared read-only between the GUI and the caching threads; `generation` changes whenever the table
// does, so caches keyed on it are dropped.
#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>

#include <common/Typedef.h>
#include <video/yuv/PixelFormatYUV.h>

#include "filter/AffineWarp.h"

namespace bda::integration
{

struct StabilizationTable
{
  std::map<int, filter::AffineParams> warps; //!< By absolute frame index; a missing frame is left as it is
  std::uint64_t                       generation{};
};
using StabilizationPtr = std::shared_ptr<const StabilizationTable>;

//!< Planar 4:2:0 or 4:0:0, 8 or 10 bit little endian: what the warp handles. `why` says otherwise.
bool stabilizationSupported(const video::yuv::PixelFormatYUV &format, const Size &size, QString *why = nullptr);

/* Warp frame `frameIndex` of `raw` in place with its entry of the table (Y, and Cb/Cr with the
 * chroma warp, Y4M C420jpeg siting as the export uses). False, leaving `raw` untouched, when the
 * frame has no entry, the format is not supported or the buffer is short.
 */
bool applyStabilizationWarp(QByteArray                        &raw,
                            const video::yuv::PixelFormatYUV &format,
                            const Size                       &size,
                            const StabilizationTable         &table,
                            int                               frameIndex);

} // namespace bda::integration
