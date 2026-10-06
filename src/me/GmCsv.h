// The CSV odyssey reads global motion candidates from, shared by the CLI and the GM window.
//
// The first nine columns are the loader's contract (analyzer/source/gm_mv.c, gm_parse_row:
// poc,ref_poc,win_i,win_j,n,dx,dy,accept_x,accept_y); everything after them is diagnostics the
// loader ignores. Qt-free.
#pragma once

#include <ostream>

#include "GlobalMotion.h"

namespace bda::me
{

void writeGmCsvHeader(std::ostream &out);

/* One row per window. poc and refPoc are display indices - decode order here would shift every
 * frame on the consumer side, and the only symptom would be "no gain".
 */
void writeGmCsvRows(std::ostream &out, int poc, int refPoc, const GmFrameResult &frame);

} // namespace bda::me
