#pragma once

#include <common/Color.h>

/* The colours of the overlays bd-analyzer adds on top of the decoders' own (ME, GM).
 *
 * Kept apart by hue family so two tools on screen at once do not share a colour:
 *   bitstream (decoder)  red / blue / green / purple, superblock grid blue, selection pink - left as
 *                        the decoders set them, they match VQ Analyzer
 *   motion estimation    warm: orange, gold, chartreuse, salmon
 *   global motion        cyan family: cyan, pale cyan, grey
 * See docs/ai/30-designs/ui-redesign-decisions.md D-08.
 */
namespace bda::integration::palette
{

// No white: it vanishes on bright pictures (test 26 paints the overlay on white).
inline const Color meSvtInteger{255, 140, 0};
inline const Color meOpenLoop{255, 215, 0};
inline const Color meClosedLoop{160, 255, 0};
inline const Color meOpenLoopAdv{255, 128, 128};

inline const Color gmAccepted{0, 220, 255};
inline const Color gmModel{170, 255, 255};
inline const Color gmRejected{150, 150, 150};

} // namespace bda::integration::palette
