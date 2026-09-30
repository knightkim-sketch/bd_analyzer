// Walk an AV1 low-overhead bitstream into its OBUs, and compare two streams payload by payload.
//
// Step 3 of the Find diff pipeline. The header comparison (step 2) says which OBU first disagrees,
// but on real mismatches the only differing syntax element is obu_size - the streams made the same
// decisions and coded them differently. The divergence is inside the payload, and that is what this
// compares.
//
// Bytes, not bit offsets. The decoder's per-block bit positions are relative to the packet payload
// libavformat produced, which is not the packet of the displayed frame once a frame is hidden;
// comparing whole OBU payloads sidesteps that entirely and gives an exact first differing byte.
//
// Qt-free, and it parses no syntax beyond the OBU header it needs to find the next OBU.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bda::diff
{

/* AV1 OBU types, only the ones this walker needs to name. Spec 6.2.2. */
enum class ObuType
{
  SequenceHeader = 1,
  TemporalDelimiter = 2,
  FrameHeader = 3,
  TileGroup = 4,
  Metadata = 5,
  Frame = 6,
  RedundantFrameHeader = 7,
  TileList = 8,
  Padding = 15,
  Other = 0,
};

const char *obuTypeName(ObuType type);

struct Obu
{
  std::size_t temporalUnit{}; //!< Index of the temporal delimiter run this OBU belongs to.
  std::size_t indexInUnit{};  //!< Position within that temporal unit, counting from the delimiter.
  ObuType     type{ObuType::Other};
  unsigned    temporalId{};
  unsigned    spatialId{};

  std::size_t headerOffset{};  //!< Offset of the OBU header in the file.
  std::size_t payloadOffset{}; //!< Offset of the first payload byte in the file.
  std::size_t payloadSize{};

  std::string label() const;
};

struct ObuScanResult
{
  std::vector<Obu> obus;
  bool             ok{};
  std::string      error; //!< Set when the walk stopped early; obus holds what was read until then.
};

/* Split a stream into OBUs.
 *
 * Expects the low-overhead bitstream format - an OBU header with obu_has_size_field set, then a
 * leb128 size. A stream without size fields cannot be split without decoding it, and is refused
 * rather than guessed at.
 */
ObuScanResult scanObus(const std::uint8_t *data, std::size_t size);

enum class PayloadDiffKind
{
  Equal,
  SizeDiffers,    //!< Same OBU, different payload length.
  ContentDiffers, //!< Same length, different bytes.
  OnlyInA,
  OnlyInB,
};

struct PayloadDiff
{
  PayloadDiffKind kind{};
  Obu             a;
  Obu             b;
  /* Offset of the first differing byte, counted from the start of the payload.
   *
   * For a length difference this is the first byte that differs within the common prefix, or the
   * length of that prefix when one payload is simply the other plus more. The distinction matters:
   * a stream that agrees for 900 bytes and then diverges is a different situation from one that
   * diverges immediately.
   */
  std::size_t firstDifferingByte{};
  bool        hasFirstDifferingByte{};
};

struct PayloadDiffResult
{
  std::vector<PayloadDiff> diffs; //!< Only the OBUs that differ, in stream order.
  std::size_t              comparedObus{};
  bool                     identical() const { return this->diffs.empty(); }
  const PayloadDiff *      first() const { return diffs.empty() ? nullptr : &diffs.front(); }
};

/* Compare two scans OBU by OBU, pairing them by position.
 *
 * Position, not type: both streams are expected to code the same source with the same structure,
 * and step 2 has already reported it when they do not. An OBU present on one side only is reported
 * rather than skipped, because that is exactly the case where pairing by position stops meaning
 * anything and the reader needs to know.
 */
PayloadDiffResult comparePayloads(const std::vector<Obu> &a,
                                  const std::uint8_t *    dataA,
                                  const std::vector<Obu> &b,
                                  const std::uint8_t *    dataB);

} // namespace bda::diff
