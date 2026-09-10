// Tracks and samples read out of an MP4's sample table.
//
// This is the half that makes an MP4 analysable rather than merely inspectable: it turns
// stsc/stco/stsz into a flat list of (file offset, size) pairs, so the existing frame and block
// hexdump panes and the AV1 OBU parser can be pointed straight at bytes in the file - the same
// thing they already do for IVF.
//
// Qt-free, like Mp4Parser.h.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "container/Mp4Parser.h"

namespace bda::container
{

//!< One coded sample, located in the file.
struct Mp4Sample
{
  std::uint64_t offset{}; //!< Absolute file offset of the sample's first byte.
  std::uint32_t size{};
  std::uint64_t decodeTime{}; //!< In the track's timescale, accumulated from stts.
  bool          sync{};       //!< A random access point. True for every sample when stss is absent.
};

struct Mp4Track
{
  std::uint32_t id{};
  std::string   handlerType;  //!< "vide", "soun", ... from mdia/hdlr.
  std::string   sampleFormat; //!< The sample entry's four character code, e.g. "av01".
  std::uint32_t timescale{};
  int           width{};
  int           height{};

  /* The configOBUs from av1C: a sequence header (and possibly metadata) that is not in any sample
   * but is needed before the first one decodes. Empty for a track that is not AV1.
   */
  std::vector<std::uint8_t> av1ConfigObus;

  std::vector<Mp4Sample> samples;

  //!< Why the track could not be read fully. The samples read before the problem are kept.
  std::string error;

  bool isAv1() const { return this->sampleFormat == "av01"; }
};

/* Read every track in the file.
 *
 * `boxes` must come from parseMp4Boxes() over the same buffer - the offsets in it are absolute
 * file offsets and are used to read the table payloads.
 *
 * A track whose sample table is unreadable is still returned, with `error` set and whatever
 * samples were recovered. Dropping it would leave the panel with nothing to explain.
 */
std::vector<Mp4Track>
readMp4Tracks(const std::uint8_t *data, std::size_t size, const Mp4ParseResult &boxes);

} // namespace bda::container
