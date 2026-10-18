// File > Open LCEVC (base + enhancement)...: a base stream and its LCEVC enhancement in a file of
// their own (enh.nal + enh.nal.sizes, as the LCEVC encoders write them), opened as one item that
// decodes enhanced.
#pragma once

class QWidget;
class playlistItemCompressedVideo;

namespace bda::integration
{

/* Ask for the base stream, then the enhancement, and build the item. Nullptr when cancelled or when
 * the base cannot be opened; problems are reported to the user here. The caller adds the item to the
 * playlist.
 */
playlistItemCompressedVideo *openLcevcSeparate(QWidget *parent);

} // namespace bda::integration
