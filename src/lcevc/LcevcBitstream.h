// Finding LCEVC enhancement data (MPEG-5 Part 2) in a base bitstream. Qt-free.
//
// The rules follow the MPEG LCEVC Test Model (LTM, decoder/src/ScanEnhancement.cpp), which is the
// reference this project decodes with:
//   * a native LCEVC NAL unit in an Annex-B stream (H.264, HEVC, VVC): start code 00 00 01, then a
//     two-byte header - forbidden_zero 0, forbidden_one 1, nal_unit_type (5 bits), 9 reserved bits
//     all 1, so the second byte is 0xFF - with nal_unit_type 28 (non-IDR) or 29 (IDR), 30 reserved;
//   * ITU-T T.35 registered user data carrying 0xB4 0x00 0x50 0x00 (country United Kingdom,
//     provider 0x0050) followed by a whole LCEVC NAL unit with its start code - the form an SEI and an
//     AV1 metadata OBU (metadata_type 4) both use.
// The payload handed on is the NAL unit's RBSP after the two-byte header, with emulation prevention
// removed and the rbsp_trailing 0x80 byte dropped, exactly what LTM passes to its deserializer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bda::lcevc
{

inline constexpr int kNalNonIdr = 28;
inline constexpr int kNalIdr    = 29;

struct LcevcNal
{
  std::size_t               offset{}; //!< Of the start code in the scanned buffer
  std::size_t               size{};   //!< Bytes from the start code to the end of the NAL unit
  int                       nalUnitType{};
  std::vector<std::uint8_t> payload; //!< RBSP after the header, unescaped, trailing 0x80 dropped
  bool isIdr() const { return this->nalUnitType == kNalIdr; }
};

//!< True when the two header bytes at `p` are an LCEVC NAL unit header with a valid type.
bool isLcevcNalHeader(const std::uint8_t *p, std::size_t available);

//!< Every LCEVC NAL unit in an Annex-B buffer (an access unit, a packet or a whole file).
std::vector<LcevcNal> findLcevcNals(const std::uint8_t *data, std::size_t size);

//!< Emulation prevention removed (00 00 03 -> 00 00).
std::vector<std::uint8_t> unescapeRbsp(const std::uint8_t *data, std::size_t size);

/* The LCEVC NAL units inside an ITU-T T.35 payload that starts at the country code (an SEI
 * user_data_registered_itu_t_t35 body, or an AV1 metadata OBU body after metadata_type). Empty when
 * the payload is not LCEVC.
 */
std::vector<LcevcNal> findLcevcInT35(const std::uint8_t *t35, std::size_t size);

/* The LCEVC NAL units in the metadata OBUs of an AV1 temporal unit (low-overhead OBU stream, i.e.
 * what an IVF frame or an MP4 sample holds).
 */
std::vector<LcevcNal> findLcevcInAv1TemporalUnit(const std::uint8_t *data, std::size_t size);

/* One LCEVC NAL unit as an Annex-B byte string: start code 00 00 01, the two-byte header, the payload
 * escaped again and the 0x80 stop byte. The form both decoders take (LTM's scanner and LCEVCdec's
 * SendDecoderEnhancementData), whatever carried the unit.
 */
std::vector<std::uint8_t> toAnnexB(const LcevcNal &nal);

/* An LCEVC NAL unit handed over on its own - with or without its start code, maybe with trailing
 * zero bytes - as one parser unit; nothing when it is some other NAL unit.
 */
std::optional<LcevcNal> lcevcNalFromUnit(const std::uint8_t *data, std::size_t size);

//!< The codec of the base NAL units in an Annex-B stream, judged by its first non-LCEVC NAL unit.
enum class AnnexBCodec
{
  Unknown,
  AVC,
  HEVC,
  VVC
};
AnnexBCodec detectAnnexBBaseCodec(const std::uint8_t *data, std::size_t size);

/* Per frame of an IVF file (AV1 temporal units), the first LCEVC NAL unit its metadata OBUs carry,
 * as toAnnexB() gives it; empty for a frame without one. Empty when the data is not IVF.
 */
std::vector<std::vector<std::uint8_t>> lcevcPerIvfFrame(const std::uint8_t *data, std::size_t size);

} // namespace bda::lcevc
