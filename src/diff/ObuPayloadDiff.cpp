#include "ObuPayloadDiff.h"

#include <algorithm>

namespace bda::diff
{

const char *obuTypeName(const ObuType type)
{
  switch (type)
  {
  case ObuType::SequenceHeader:       return "Sequence Header";
  case ObuType::TemporalDelimiter:    return "Temporal Delimiter";
  case ObuType::FrameHeader:          return "Frame Header";
  case ObuType::TileGroup:            return "Tile Group";
  case ObuType::Metadata:             return "Metadata";
  case ObuType::Frame:                return "Frame";
  case ObuType::RedundantFrameHeader: return "Redundant Frame Header";
  case ObuType::TileList:             return "Tile List";
  case ObuType::Padding:              return "Padding";
  case ObuType::Other:                break;
  }
  return "Other";
}

std::string Obu::label() const
{
  return "TU " + std::to_string(this->temporalUnit) + " / OBU " +
         std::to_string(this->indexInUnit) + ": " + obuTypeName(this->type);
}

namespace
{

/* leb128, spec 4.10.5. Returns false when the value runs past the buffer or past its eight byte
 * limit; a size read out of a truncated file would otherwise become a plausible-looking offset.
 */
bool readLeb128(const std::uint8_t *data,
                const std::size_t   size,
                std::size_t &       pos,
                std::uint64_t &     value)
{
  value = 0;
  for (unsigned i = 0; i < 8; ++i)
  {
    if (pos >= size)
      return false;
    const auto byte = data[pos++];
    value |= std::uint64_t(byte & 0x7f) << (i * 7);
    if ((byte & 0x80) == 0)
      return true;
  }
  return false;
}

} // namespace

ObuScanResult scanObus(const std::uint8_t *data, const std::size_t size)
{
  ObuScanResult result;
  if (data == nullptr || size == 0)
  {
    result.error = "empty stream";
    return result;
  }

  std::size_t pos          = 0;
  std::size_t temporalUnit = 0;
  std::size_t indexInUnit  = 0;
  bool        sawDelimiter = false;

  while (pos < size)
  {
    Obu obu;
    obu.headerOffset = pos;

    const auto header = data[pos++];
    if ((header & 0x80) != 0)
    {
      result.error = "obu_forbidden_bit set at offset " + std::to_string(obu.headerOffset);
      return result;
    }
    const auto typeValue     = unsigned((header >> 3) & 0x0f);
    const bool extensionFlag = (header & 0x04) != 0;
    const bool hasSizeField  = (header & 0x02) != 0;

    obu.type = (typeValue == 1 || typeValue == 2 || typeValue == 3 || typeValue == 4 ||
                typeValue == 5 || typeValue == 6 || typeValue == 7 || typeValue == 8 ||
                typeValue == 15)
                   ? static_cast<ObuType>(typeValue)
                   : ObuType::Other;

    if (extensionFlag)
    {
      if (pos >= size)
      {
        result.error = "truncated OBU extension header";
        return result;
      }
      const auto extension = data[pos++];
      obu.temporalId       = unsigned((extension >> 5) & 0x07);
      obu.spatialId        = unsigned((extension >> 3) & 0x03);
    }

    if (!hasSizeField)
    {
      /* Without obu_size the OBU runs to the end of the temporal unit, and finding that end means
       * decoding. Refused rather than guessed: a wrong boundary here would compare two unrelated
       * byte runs and report a difference that is an artefact of the walk.
       */
      result.error = "obu_has_size_field is not set at offset " + std::to_string(obu.headerOffset) +
                     "; this walker needs the low-overhead bitstream format";
      return result;
    }

    std::uint64_t payloadSize = 0;
    if (!readLeb128(data, size, pos, payloadSize))
    {
      result.error = "truncated obu_size at offset " + std::to_string(obu.headerOffset);
      return result;
    }
    obu.payloadOffset = pos;
    obu.payloadSize   = std::size_t(payloadSize);
    if (obu.payloadOffset + obu.payloadSize > size)
    {
      result.error = "OBU payload runs past the end of the stream at offset " +
                     std::to_string(obu.headerOffset);
      return result;
    }

    if (obu.type == ObuType::TemporalDelimiter)
    {
      // The delimiter opens a temporal unit; it is itself OBU 0 of that unit.
      if (sawDelimiter)
        ++temporalUnit;
      sawDelimiter = true;
      indexInUnit  = 0;
    }
    obu.temporalUnit = temporalUnit;
    obu.indexInUnit  = indexInUnit++;

    result.obus.push_back(obu);
    pos = obu.payloadOffset + obu.payloadSize;
  }

  result.ok = true;
  return result;
}

PayloadDiffResult comparePayloads(const std::vector<Obu> &a,
                                  const std::uint8_t *    dataA,
                                  const std::vector<Obu> &b,
                                  const std::uint8_t *    dataB)
{
  PayloadDiffResult result;
  const auto        common = std::min(a.size(), b.size());
  result.comparedObus      = common;

  for (std::size_t i = 0; i < common; ++i)
  {
    const auto &oa = a[i];
    const auto &ob = b[i];

    const auto prefix = std::min(oa.payloadSize, ob.payloadSize);
    std::size_t firstDiff = prefix;
    bool        found     = false;
    for (std::size_t k = 0; k < prefix; ++k)
      if (dataA[oa.payloadOffset + k] != dataB[ob.payloadOffset + k])
      {
        firstDiff = k;
        found     = true;
        break;
      }

    if (oa.payloadSize == ob.payloadSize && !found)
      continue;

    PayloadDiff diff;
    diff.a    = oa;
    diff.b    = ob;
    diff.kind = (oa.payloadSize == ob.payloadSize) ? PayloadDiffKind::ContentDiffers
                                                   : PayloadDiffKind::SizeDiffers;
    // For a pure length difference there is no differing byte in the common prefix; reporting the
    // prefix length says "they agreed this far", which is the useful number either way.
    diff.firstDifferingByte    = firstDiff;
    diff.hasFirstDifferingByte = found || oa.payloadSize != ob.payloadSize;
    result.diffs.push_back(diff);
  }

  for (std::size_t i = common; i < a.size(); ++i)
    result.diffs.push_back({PayloadDiffKind::OnlyInA, a[i], {}, 0, false});
  for (std::size_t i = common; i < b.size(); ++i)
    result.diffs.push_back({PayloadDiffKind::OnlyInB, {}, b[i], 0, false});

  return result;
}

} // namespace bda::diff
