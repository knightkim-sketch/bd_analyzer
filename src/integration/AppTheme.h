#pragma once

#include <QString>

namespace bda::integration
{

// The theme bd-analyzer adds next to YUView's style-sheet themes. Fusion with a dark palette, no
// style sheet, so every widget - including the ones the style sheet never covered - turns dark.
inline const QString kDarkFusionTheme = "Dark (Fusion)";

/* Apply the palette part of a theme. Called before the style sheet is set, so that the first call -
 * which records the platform's own style and palette to return to - sees them without a sheet. This
 * switches to Fusion with a dark palette for kDarkFusionTheme and back to the platform's own style and
 * palette for every other theme.
 */
void applyThemePalette(const QString &themeName);

} // namespace bda::integration
