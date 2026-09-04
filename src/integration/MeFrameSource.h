// Turning what the playlist holds into what the estimators take.
//
// Part of the src/integration boundary: this is allowed to know both about upstream's raw YUV
// frames and about the Qt-free ME core, and it is the only place that translates between them.
#pragma once

#include <QByteArray>

#include <common/Typedef.h>
#include <video/yuv/PixelFormatYUV.h>

#include "me/MePlane.h"

namespace bda::integration
{

/* The luma plane of one raw YUV frame, as the 8-bit plane the estimators search.
 *
 * Sources deeper than 8 bit are shifted down, not scaled: `value >> (bitDepth - 8)`. That is what
 * odyssey's open-loop ME does to a 10-bit source (cvt_10bit_to_8bit_img, dst = src >> 2) and what
 * SVT's 8-bit kernels need. Rescaling to the full 0..255 range instead would brighten the picture
 * and change every SAD, so the encoders' arithmetic is what gets reproduced.
 *
 * The reader underneath is upstream's own (stats::makeLumaReader), which already handles bit depth,
 * endianness and plane layout. Returns an empty plane when the format cannot be read - a packed or
 * byte-packed format, a bit depth outside 8..16, or a truncated frame - and the caller is expected
 * to report that rather than search a plane of zeros.
 */
me::MePlane lumaPlaneFromRawYuv(const QByteArray                 &rawYUV,
                                const video::yuv::PixelFormatYUV &format,
                                const Size                       &frameSize,
                                int                               pad);

} // namespace bda::integration
