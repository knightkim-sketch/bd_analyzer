// File > Export filtered Y4M: the selected item's frames run through its luma filter chain (the
// Convolution filter pane) and written as YUV4MPEG2.
//
// Part of the src/integration boundary. The Global motion window and the CLI read files, not
// playlist items, so this is how they get to see a filtered sequence. Chroma is copied unchanged.
#pragma once

class QWidget;
class playlistItem;

namespace bda::integration
{

/* Asks for a file name, writes the item's frame range (start to end, inclusive) with a progress
 * dialog that can cancel, and reports the result. Explains in a message box when the item cannot be
 * exported: no raw source, or not planar 4:2:0 / 4:0:0 at 8 or 10 bit little endian.
 */
void exportFilteredY4m(playlistItem *item, QWidget *parent);

} // namespace bda::integration
