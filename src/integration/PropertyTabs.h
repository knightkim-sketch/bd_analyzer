#pragma once

#include <vector>

class QLayout;
class QVBoxLayout;

namespace bda::integration
{

/* Split an item's properties panel in two tabs instead of one long column:
 *   Video     how the item is read and processed - everything already in `column`
 *             (format, conversion, filters, decoder, display signal)
 *   Overlays  what is drawn on top - the statistics check boxes in `overlays`
 * The tab widget then takes the column's place. The tab last chosen is the one new panels open on,
 * so stepping through items keeps showing the same kind of settings.
 * See docs/ai/30-designs/ui-redesign-decisions.md D-16.
 */
void splitPropertiesIntoTabs(QVBoxLayout *column, const std::vector<QLayout *> &overlays);

} // namespace bda::integration
