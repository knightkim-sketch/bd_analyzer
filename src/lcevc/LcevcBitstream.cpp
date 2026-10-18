#include "LcevcBitstream.h"

#include <cctype>
#include <cstring>

namespace bda::lcevc
{

namespace
{

bool isStartCode(const std::uint8_t *p) { return p[0] == 0 && p[1] == 0 && p[2] == 1; }

// leb128() as AV1 codes OBU sizes and metadata_type. False when it runs past `end`.
bool readLeb128(const std::uint8_t *&p, const std::uint8_t *end, std::uint64_t &value)
{
  value = 0;
  for (int i = 0; i < 8; ++i)
  {
    if (p >= end)
      return false;
    const std::uint8_t b = *p++;
    value |= std::uint64_t(b & 0x7f) << (7 * i);
    if (!(b & 0x80))
      return true;
  }
  return false;
}

} // namespace

bool isLcevcNalHeader(const std::uint8_t *p, std::size_t available)
{
  if (available < 2 || (p[0] & 0xc0) != 0x40 || p[1] != 0xff)
    return false;
  const int type = (p[0] & 0x3e) >> 1;
  return type >= kNalNonIdr && type <= 30;
}

std::vector<std::uint8_t> unescapeRbsp(const std::uint8_t *data, std::size_t size)
{
  std::vector<std::uint8_t> out;
  out.reserve(size);
  int zeros = 0;
  for (std::size_t i = 0; i < size; ++i)
  {
    if (zeros >= 2 && data[i] == 0x03)
    {
      zeros = 0;
      continue;
    }
    zeros = data[i] == 0 ? zeros + 1 : 0;
    out.push_back(data[i]);
  }
  return out;
}

std::vector<LcevcNal> findLcevcNals(const std::uint8_t *data, std::size_t size)
{
  std::vector<LcevcNal> out;
  if (size < 5)
    return out;
  for (std::size_t i = 0; i + 4 < size; ++i)
  {
    if (!isStartCode(data + i) || !isLcevcNalHeader(data + i + 3, size - i - 3))
      continue;
    // The unit runs to the next start code; a 00 00 00 01 one leaves its leading zero outside.
    std::size_t end = size;
    for (std::size_t j = i + 4; j + 3 <= size; ++j)
      if (isStartCode(data + j))
      {
        end = data[j - 1] == 0 ? j - 1 : j;
        break;
      }
    LcevcNal nal;
    nal.offset      = i;
    nal.size        = end - i;
    nal.nalUnitType = (data[i + 3] & 0x3e) >> 1;
    nal.payload     = unescapeRbsp(data + i + 5, end - (i + 5));
    // The rbsp_trailing_bits byte (0x80) is not part of the data (LTM rbsp_decoder::copy).
    if (!nal.payload.empty())
      nal.payload.pop_back();
    out.push_back(std::move(nal));
    i = end - 1;
  }
  return out;
}

std::vector<LcevcNal> findLcevcInT35(const std::uint8_t *t35, std::size_t size)
{
  static const std::uint8_t kLcevcT35[4] = {0xb4, 0x00, 0x50, 0x00};
  if (size < sizeof(kLcevcT35) || std::memcmp(t35, kLcevcT35, sizeof(kLcevcT35)) != 0)
    return {};
  return findLcevcNals(t35 + sizeof(kLcevcT35), size - sizeof(kLcevcT35));
}

std::vector<LcevcNal> findLcevcInAv1TemporalUnit(const std::uint8_t *data, std::size_t size)
{
  constexpr int kObuMetadata  = 5;
  constexpr int kMetadataT35  = 4;
  std::vector<LcevcNal> out;
  const std::uint8_t   *p = data, *end = data + size;
  while (p < end)
  {
    const std::uint8_t header   = *p++;
    const int          type     = (header >> 3) & 0x0f;
    const bool         ext      = header & 0x04;
    const bool         hasSize  = header & 0x02;
    if (ext)
    {
      if (p >= end)
        break;
      ++p;
    }
    std::uint64_t obuSize = std::uint64_t(end - p);
    if (hasSize && !readLeb128(p, end, obuSize))
      break;
    if (obuSize > std::uint64_t(end - p))
      break;
    const std::uint8_t *body = p, *bodyEnd = p + obuSize;
    p = bodyEnd;
    if (type != kObuMetadata)
      continue;
    std::uint64_t metadataType = 0;
    if (!readLeb128(body, bodyEnd, metadataType) || metadataType != kMetadataT35)
      continue;
    // The OBU ends with trailing_bits: a 1 bit then zeros - drop them so the NAL ends where it does.
    while (bodyEnd > body && bodyEnd[-1] == 0)
      --bodyEnd;
    if (bodyEnd > body && bodyEnd[-1] == 0x80)
      --bodyEnd;
    auto found = findLcevcInT35(body, std::size_t(bodyEnd - body));
    for (auto &nal : found)
      out.push_back(std::move(nal));
  }
  return out;
}

std::vector<std::uint8_t> toAnnexB(const LcevcNal &nal)
{
  std::vector<std::uint8_t> out = {0x00, 0x00, 0x01, std::uint8_t(0x41 | (nal.nalUnitType << 1)), 0xff};
  out.reserve(nal.payload.size() + nal.payload.size() / 64 + 8);
  int zeros = 0;
  for (const auto b : nal.payload)
  {
    if (zeros >= 2 && b <= 0x03)
    {
      out.push_back(0x03);
      zeros = 0;
    }
    zeros = b == 0 ? zeros + 1 : 0;
    out.push_back(b);
  }
  out.push_back(0x80);
  return out;
}

std::optional<LcevcNal> lcevcNalFromUnit(const std::uint8_t *data, std::size_t size)
{
  std::size_t start = 0;
  if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
    start = 4;
  else if (size >= 3 && isStartCode(data))
    start = 3;
  if (!isLcevcNalHeader(data + start, size - start))
    return {};
  LcevcNal nal;
  nal.size        = size;
  nal.nalUnitType = (data[start] & 0x3e) >> 1;
  nal.payload     = unescapeRbsp(data + start + 2, size - start - 2);
  while (!nal.payload.empty() && nal.payload.back() == 0)
    nal.payload.pop_back();
  if (!nal.payload.empty())
    nal.payload.pop_back();
  return nal;
}

std::optional<int> signalledBitstreamVersion(const LcevcNal &nal)
{
  const auto &d   = nal.payload;
  std::size_t pos = 0;
  while (pos < d.size())
  {
    const int   sizeType = d[pos] >> 5, type = d[pos] & 0x1f;
    std::size_t size     = 0;
    ++pos;
    if (sizeType <= 5)
      size = std::size_t(sizeType);
    else if (sizeType == 7)
    {
      // multibyte: 7 bits per byte, most significant group first, 0x80 = more to come
      for (;;)
      {
        if (pos >= d.size())
          return std::nullopt;
        const auto b = d[pos++];
        size         = (size << 7) | (b & 0x7f);
        if (!(b & 0x80))
          break;
      }
    }
    else
      return std::nullopt;
    if (size > d.size() - pos)
      return std::nullopt;
    if (type == 1) // global_config: a version must come before it
      return std::nullopt;
    static const std::uint8_t kVersionSei[] = {0x00, 0x04, 0xb4, 0x00, 0x50, 0x00};
    if (type == 5 && size >= sizeof(kVersionSei) + 1 &&
        std::memcmp(d.data() + pos, kVersionSei, sizeof(kVersionSei)) == 0)
      return int(d[pos + sizeof(kVersionSei)]);
    pos += size;
  }
  return std::nullopt;
}

AnnexBCodec detectAnnexBBaseCodec(const std::uint8_t *data, std::size_t size)
{
  for (std::size_t i = 0; i + 5 <= size; ++i)
  {
    if (!isStartCode(data + i))
      continue;
    const std::uint8_t *h = data + i + 3;
    i += 2;
    if (isLcevcNalHeader(h, 2))
      continue;
    const int hevcType = (h[0] >> 1) & 0x3f;
    if (!(h[0] & 0x81) && h[1] == 0x01 && ((hevcType >= 32 && hevcType <= 35) || hevcType == 39))
      return AnnexBCodec::HEVC;
    const int vvcType = h[1] >> 3;
    if (h[0] == 0 && (h[1] & 0x07) != 0 &&
        (vvcType == 14 || vvcType == 15 || vvcType == 16 || vvcType == 20 || vvcType == 23))
      return AnnexBCodec::VVC;
    const int avcType = h[0] & 0x1f;
    if (!(h[0] & 0x80) && (avcType == 6 || avcType == 7 || avcType == 8 || avcType == 9))
      return AnnexBCodec::AVC;
    return AnnexBCodec::Unknown;
  }
  return AnnexBCodec::Unknown;
}

std::vector<std::vector<std::uint8_t>> lcevcPerIvfFrame(const std::uint8_t *data, std::size_t size)
{
  std::vector<std::vector<std::uint8_t>> out;
  if (size < 32 || std::memcmp(data, "DKIF", 4) != 0)
    return out;
  std::size_t pos = std::size_t(data[6]) | std::size_t(data[7]) << 8;
  while (pos + 12 <= size)
  {
    const std::size_t frameSize = std::size_t(data[pos]) | std::size_t(data[pos + 1]) << 8 |
                                  std::size_t(data[pos + 2]) << 16 | std::size_t(data[pos + 3]) << 24;
    pos += 12;
    if (frameSize > size - pos)
      break;
    const auto nals = findLcevcInAv1TemporalUnit(data + pos, frameSize);
    out.push_back(nals.empty() ? std::vector<std::uint8_t>{} : toAnnexB(nals.front()));
    pos += frameSize;
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> lcevcPerPictureFromNalFile(const std::uint8_t             *data,
                                                                   std::size_t                     size,
                                                                   const std::vector<std::size_t> *sizes)
{
  std::vector<std::vector<std::uint8_t>> out;
  if (!sizes)
  {
    for (const auto &nal : findLcevcNals(data, size))
      out.push_back(toAnnexB(nal));
    return out;
  }
  std::size_t pos = 0;
  for (const auto n : *sizes)
  {
    if (n > size - pos)
      break;
    const auto nals = findLcevcNals(data + pos, n);
    out.push_back(nals.empty() ? std::vector<std::uint8_t>{} : toAnnexB(nals.front()));
    pos += n;
  }
  return out;
}

std::optional<std::vector<std::size_t>> parseSizesFile(const std::string &text)
{
  std::vector<std::size_t> sizes;
  std::size_t              i = 0;
  while (i < text.size())
  {
    if (std::isspace(static_cast<unsigned char>(text[i])))
    {
      ++i;
      continue;
    }
    std::size_t value = 0;
    if (!std::isdigit(static_cast<unsigned char>(text[i])))
      return std::nullopt;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
      value = value * 10 + std::size_t(text[i++] - '0');
    sizes.push_back(value);
  }
  return sizes;
}

std::vector<std::uint8_t> muxLcevcIntoIvf(const std::uint8_t                           *ivf,
                                          std::size_t                                   size,
                                          const std::vector<std::vector<std::uint8_t>> &perFrame)
{
  constexpr int kObuSequenceHeader    = 1;
  constexpr int kObuTemporalDelimiter = 2;
  if (size < 32 || std::memcmp(ivf, "DKIF", 4) != 0)
    return {};
  const std::size_t         headerSize = std::size_t(ivf[6]) | std::size_t(ivf[7]) << 8;
  std::vector<std::uint8_t> out(ivf, ivf + std::min(headerSize, size));
  auto putLeb128 = [](std::vector<std::uint8_t> &v, std::size_t value) {
    do
    {
      v.push_back(std::uint8_t((value & 0x7f) | (value > 0x7f ? 0x80 : 0)));
      value >>= 7;
    } while (value);
  };

  std::size_t pos = headerSize;
  for (std::size_t frame = 0; pos + 12 <= size; ++frame)
  {
    const std::size_t frameSize = std::size_t(ivf[pos]) | std::size_t(ivf[pos + 1]) << 8 |
                                  std::size_t(ivf[pos + 2]) << 16 | std::size_t(ivf[pos + 3]) << 24;
    const std::uint8_t *frameHeader = ivf + pos;
    pos += 12;
    if (frameSize > size - pos)
      return {};
    const std::uint8_t *tu = ivf + pos;
    pos += frameSize;

    std::vector<std::uint8_t> unit;
    if (frame < perFrame.size() && !perFrame[frame].empty())
    {
      // Past the temporal delimiter and the sequence headers.
      const std::uint8_t *p = tu, *end = tu + frameSize;
      while (p < end)
      {
        const int type = (*p >> 3) & 0x0f;
        if (type != kObuTemporalDelimiter && type != kObuSequenceHeader)
          break;
        if (!(*p & 0x02))
          return {};
        const std::uint8_t *q = p + 1 + ((*p & 0x04) ? 1 : 0);
        std::uint64_t       obuSize = 0;
        if (q > end || !readLeb128(q, end, obuSize) || obuSize > std::uint64_t(end - q))
          return {};
        p = q + obuSize;
      }
      std::vector<std::uint8_t> body = {0x04, 0xb4, 0x00, 0x50, 0x00};
      body.insert(body.end(), perFrame[frame].begin(), perFrame[frame].end());
      body.push_back(0x80);
      unit.assign(tu, p);
      unit.push_back(0x2a); // obu_type 5 (OBU_METADATA), obu_has_size_field
      putLeb128(unit, body.size());
      unit.insert(unit.end(), body.begin(), body.end());
      unit.insert(unit.end(), p, end);
    }
    else
      unit.assign(tu, tu + frameSize);

    const auto n = unit.size();
    out.insert(out.end(), {std::uint8_t(n), std::uint8_t(n >> 8), std::uint8_t(n >> 16), std::uint8_t(n >> 24)});
    out.insert(out.end(), frameHeader + 4, frameHeader + 12);
    out.insert(out.end(), unit.begin(), unit.end());
  }
  return out;
}

} // namespace bda::lcevc
