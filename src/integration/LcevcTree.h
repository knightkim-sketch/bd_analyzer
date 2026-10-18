// LCEVC enhancement data in the bitstream analysis tree: the bridge between the Qt-free syntax parser
// (src/lcevc) and YUView's TreeItem, used by the AVC/HEVC/VVC Annex-B parsers and the AV1 OBU parser.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <common/Typedef.h>

#include "lcevc/LcevcSyntax.h"

class TreeItem;

namespace bda::integration
{

/* If the NAL unit in `data` (its start code, if any, ends at `startCodeOffset`) is an LCEVC NAL unit,
 * parse it, name `nalRoot` "NAL <nalId>: LCEVC IDR / non-IDR", hang its syntax under it and return
 * true. False for any other NAL unit - the caller parses it as usual. `nalRoot` may be null.
 */
bool addLcevcNalUnit(const ByteVector              &data,
                     std::size_t                    startCodeOffset,
                     const std::shared_ptr<TreeItem> &nalRoot,
                     lcevc::SyntaxParser           &parser,
                     int                            nalId);

/* An AV1 metadata OBU's payload (after the OBU header and size): when it is ITU-T T.35 carrying
 * LCEVC, parse it and hang an "LCEVC (ITU-T T.35)" group under `obuRoot`. True when it was LCEVC.
 */
bool addLcevcAv1Metadata(const std::uint8_t            *payload,
                         std::size_t                    size,
                         const std::shared_ptr<TreeItem> &obuRoot,
                         lcevc::SyntaxParser           &parser);

} // namespace bda::integration
