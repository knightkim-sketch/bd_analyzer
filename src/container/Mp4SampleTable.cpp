#include "container/Mp4SampleTable.h"

#include <algorithm>
#include <map>

namespace bda::container
{

namespace
{

struct Reader
{
  const std::uint8_t *data{};
  std::uint64_t       size{};
  bool                overran{};

  std::uint32_t u32(std::uint64_t offset)
  {
    if (offset + 4 > this->size)
    {
      this->overran = true;
      return 0;
    }
    return (std::uint32_t(this->data[offset]) << 24) | (std::uint32_t(this->data[offset + 1]) << 16) |
           (std::uint32_t(this->data[offset + 2]) << 8) | std::uint32_t(this->data[offset + 3]);
  }

  std::uint64_t u64(std::uint64_t offset)
  {
    return (std::uint64_t(this->u32(offset)) << 32) | this->u32(offset + 4);
  }

  std::uint16_t u16(std::uint64_t offset)
  {
    if (offset + 2 > this->size)
    {
      this->overran = true;
      return 0;
    }
    return std::uint16_t((std::uint16_t(this->data[offset]) << 8) | this->data[offset + 1]);
  }

  std::string fourcc(std::uint64_t offset)
  {
    if (offset + 4 > this->size)
    {
      this->overran = true;
      return {};
    }
    return std::string(reinterpret_cast<const char *>(this->data + offset), 4);
  }
};

//!< An entry count sanity bound. A table claiming more entries than the file has bytes is corrupt.
bool plausibleCount(std::uint32_t count, std::uint64_t fileSize, std::uint32_t entrySize)
{
  return std::uint64_t(count) * entrySize <= fileSize;
}

struct SampleToChunk
{
  std::uint32_t firstChunk{};
  std::uint32_t samplesPerChunk{};
};

/* Turn the chunk table and the size table into located samples.
 *
 * The awkward part of ISO BMFF: sizes are per sample, offsets are per *chunk*, and how many
 * samples sit in a chunk is a run-length table. So a sample's offset is its chunk's offset plus
 * the sizes of the samples ahead of it in that same chunk - it is not recorded anywhere directly.
 */
std::vector<Mp4Sample> locateSamples(const std::vector<std::uint32_t> &sizes,
                                     const std::vector<std::uint64_t> &chunkOffsets,
                                     const std::vector<SampleToChunk> &runs,
                                     std::string                      &error)
{
  std::vector<Mp4Sample> samples;
  if (chunkOffsets.empty() || runs.empty())
  {
    if (!sizes.empty())
      error = "The track has sample sizes but no chunk table, so no sample can be located.";
    return samples;
  }

  samples.reserve(sizes.size());

  std::size_t   nextSample = 0;
  std::size_t   run        = 0;
  for (std::size_t chunk = 0; chunk < chunkOffsets.size() && nextSample < sizes.size(); ++chunk)
  {
    /* Advance to the run that covers this chunk. first_chunk is 1-based and the runs are ordered,
     * so the applicable run is the last one whose first_chunk is not past us.
     */
    while (run + 1 < runs.size() && runs[run + 1].firstChunk <= chunk + 1)
      ++run;

    const auto perChunk = runs[run].samplesPerChunk;
    if (perChunk == 0)
    {
      error = "A stsc run declares zero samples per chunk, which cannot be walked.";
      return samples;
    }

    auto offset = chunkOffsets[chunk];
    for (std::uint32_t index = 0; index < perChunk && nextSample < sizes.size(); ++index)
    {
      Mp4Sample sample;
      sample.offset = offset;
      sample.size   = sizes[nextSample];
      samples.push_back(sample);

      offset += sizes[nextSample];
      ++nextSample;
    }
  }

  if (nextSample < sizes.size())
    error = "The chunk table ran out with " + std::to_string(sizes.size() - nextSample) +
            " samples still unplaced.";
  return samples;
}

void readTimes(Reader &reader, const Mp4Box *stts, std::vector<Mp4Sample> &samples)
{
  if (stts == nullptr)
    return;

  const auto payload = stts->payloadOffset();
  const auto count   = reader.u32(payload + 4);
  if (!plausibleCount(count, reader.size, 8))
    return;

  std::uint64_t time  = 0;
  std::size_t   index = 0;
  for (std::uint32_t entry = 0; entry < count && index < samples.size(); ++entry)
  {
    const auto runLength = reader.u32(payload + 8 + std::uint64_t(entry) * 8);
    const auto delta     = reader.u32(payload + 12 + std::uint64_t(entry) * 8);
    for (std::uint32_t repeat = 0; repeat < runLength && index < samples.size(); ++repeat)
    {
      samples[index].decodeTime = time;
      time += delta;
      ++index;
    }
  }
}

void readSyncFlags(Reader &reader, const Mp4Box *stss, std::vector<Mp4Sample> &samples)
{
  /* No stss means every sample is a random access point - that is the spec's default, and getting
   * it backwards would make an all-intra track look like it has no key frames at all.
   */
  if (stss == nullptr)
  {
    for (auto &sample : samples)
      sample.sync = true;
    return;
  }

  const auto payload = stss->payloadOffset();
  const auto count   = reader.u32(payload + 4);
  if (!plausibleCount(count, reader.size, 4))
    return;

  for (std::uint32_t entry = 0; entry < count; ++entry)
  {
    const auto number = reader.u32(payload + 8 + std::uint64_t(entry) * 4); // 1-based
    if (number >= 1 && number <= samples.size())
      samples[number - 1].sync = true;
  }
}

/* Per track defaults from `mvex/trex`, used by any fragment that does not carry its own.
 *
 * A fragmented file usually states the sample size once here and then never again, so without
 * this every sample in the file comes out zero sized.
 */
struct TrackFragmentDefaults
{
  std::uint32_t sampleDuration{};
  std::uint32_t sampleSize{};
  std::uint32_t sampleFlags{};
};

// A sample is a random access point unless its flags say otherwise. ISO 14496-12, 8.8.3.1.
bool syncFromSampleFlags(std::uint32_t flags)
{
  constexpr std::uint32_t kNonSyncSample = 0x00010000;
  return (flags & kNonSyncSample) == 0;
}

std::map<std::uint32_t, TrackFragmentDefaults> readTrackExtends(Reader &reader, const Mp4Box &moov)
{
  std::map<std::uint32_t, TrackFragmentDefaults> defaults;
  const auto                                    *mvex = findMp4Box(moov.children, "mvex");
  if (mvex == nullptr)
    return defaults;

  for (const auto *trex : findMp4Children(*mvex, "trex"))
  {
    const auto payload = trex->payloadOffset();
    const auto trackId = reader.u32(payload + 4);
    // payload: version/flags(4), track_ID(4), default_sample_description_index(4), then the three.
    defaults[trackId] = {reader.u32(payload + 12), reader.u32(payload + 16), reader.u32(payload + 20)};
  }
  return defaults;
}

/* Locate the samples a fragmented file keeps in its `moof` boxes.
 *
 * Each `moof/traf` names a track and describes a run of samples: `tfhd` says where the run's data
 * starts and what the defaults are, `trun` gives the per sample sizes. The samples sit end to end
 * from the run's start, exactly as they do inside a chunk - the difference is only where the
 * description lives.
 *
 * Returned per track id, because `traf` names the track and the caller matches it to `tkhd`.
 */
std::map<std::uint32_t, std::vector<Mp4Sample>>
readFragmentSamples(Reader &reader, const Mp4ParseResult &boxes, const Mp4Box &moov)
{
  // tfhd flags
  constexpr std::uint32_t kBaseDataOffsetPresent      = 0x000001;
  constexpr std::uint32_t kSampleDescriptionPresent   = 0x000002;
  constexpr std::uint32_t kDefaultSampleDuration      = 0x000008;
  constexpr std::uint32_t kDefaultSampleSize          = 0x000010;
  constexpr std::uint32_t kDefaultSampleFlags         = 0x000020;
  constexpr std::uint32_t kDefaultBaseIsMoof          = 0x020000;
  // trun flags
  constexpr std::uint32_t kDataOffsetPresent          = 0x000001;
  constexpr std::uint32_t kFirstSampleFlagsPresent    = 0x000004;
  constexpr std::uint32_t kSampleDurationPresent      = 0x000100;
  constexpr std::uint32_t kSampleSizePresent          = 0x000200;
  constexpr std::uint32_t kSampleFlagsPresent         = 0x000400;
  constexpr std::uint32_t kCompositionOffsetPresent   = 0x000800;

  const auto trex = readTrackExtends(reader, moov);

  std::map<std::uint32_t, std::vector<Mp4Sample>> samples;
  std::map<std::uint32_t, std::uint64_t>          decodeTime;

  for (const auto &moof : boxes.boxes)
  {
    if (moof.type != "moof")
      continue;

    for (const auto *traf : findMp4Children(moof, "traf"))
    {
      const auto *tfhd = findMp4Box(traf->children, "tfhd");
      if (tfhd == nullptr)
        continue;

      auto       at      = tfhd->payloadOffset();
      const auto tfhdFlags = reader.u32(at) & 0x00ffffff; // low 24 bits; the top byte is version
      at += 4;
      const auto trackId = reader.u32(at);
      at += 4;

      /* Where this run's data starts. With neither an explicit offset nor default-base-is-moof the
       * spec chains each fragment onto the end of the previous one; that form is rare and getting
       * it subtly wrong would place samples plausibly but incorrectly, so the moof start is used
       * and the offsets are bounds checked by the caller either way.
       */
      std::uint64_t baseOffset = moof.offset;
      if (tfhdFlags & kBaseDataOffsetPresent)
      {
        baseOffset = reader.u64(at);
        at += 8;
      }
      else if ((tfhdFlags & kDefaultBaseIsMoof) == 0)
      {
        baseOffset = moof.offset;
      }
      if (tfhdFlags & kSampleDescriptionPresent)
        at += 4;

      auto fragmentDefaults = trex.count(trackId) != 0 ? trex.at(trackId) : TrackFragmentDefaults{};
      if (tfhdFlags & kDefaultSampleDuration)
      {
        fragmentDefaults.sampleDuration = reader.u32(at);
        at += 4;
      }
      if (tfhdFlags & kDefaultSampleSize)
      {
        fragmentDefaults.sampleSize = reader.u32(at);
        at += 4;
      }
      if (tfhdFlags & kDefaultSampleFlags)
      {
        fragmentDefaults.sampleFlags = reader.u32(at);
        at += 4;
      }

      // tfdt restates the decode time, which keeps a seek from having to sum every fragment before.
      if (const auto *tfdt = findMp4Box(traf->children, "tfdt"))
      {
        const auto tfdtPayload = tfdt->payloadOffset();
        const auto version     = reader.u32(tfdtPayload) >> 24;
        decodeTime[trackId] =
            version == 1 ? reader.u64(tfdtPayload + 4) : reader.u32(tfdtPayload + 4);
      }

      for (const auto *trun : findMp4Children(*traf, "trun"))
      {
        auto       runAt     = trun->payloadOffset();
        const auto trunFlags = reader.u32(runAt) & 0x00ffffff;
        runAt += 4;
        const auto sampleCount = reader.u32(runAt);
        runAt += 4;

        auto dataOffset = baseOffset;
        if (trunFlags & kDataOffsetPresent)
        {
          // Signed: a run can start before the box that describes it.
          dataOffset = baseOffset + std::int32_t(reader.u32(runAt));
          runAt += 4;
        }

        std::uint32_t firstSampleFlags = fragmentDefaults.sampleFlags;
        if (trunFlags & kFirstSampleFlagsPresent)
        {
          firstSampleFlags = reader.u32(runAt);
          runAt += 4;
        }

        if (!plausibleCount(sampleCount, reader.size, 1))
          continue;

        auto at2 = dataOffset;
        for (std::uint32_t index = 0; index < sampleCount; ++index)
        {
          auto duration = fragmentDefaults.sampleDuration;
          auto size     = fragmentDefaults.sampleSize;
          auto flags    = index == 0 ? firstSampleFlags : fragmentDefaults.sampleFlags;

          if (trunFlags & kSampleDurationPresent)
          {
            duration = reader.u32(runAt);
            runAt += 4;
          }
          if (trunFlags & kSampleSizePresent)
          {
            size = reader.u32(runAt);
            runAt += 4;
          }
          if (trunFlags & kSampleFlagsPresent)
          {
            const auto perSample = reader.u32(runAt);
            runAt += 4;
            if (index != 0 || (trunFlags & kFirstSampleFlagsPresent) == 0)
              flags = perSample;
          }
          if (trunFlags & kCompositionOffsetPresent)
            runAt += 4;

          Mp4Sample sample;
          sample.offset     = at2;
          sample.size       = size;
          sample.decodeTime = decodeTime[trackId];
          sample.sync       = syncFromSampleFlags(flags);
          samples[trackId].push_back(sample);

          at2 += size;
          decodeTime[trackId] += duration;
        }
      }
    }
  }
  return samples;
}

//!< The av1C payload is a 4-byte configuration record, then the configOBUs to the end of the box.
std::vector<std::uint8_t> readAv1Config(const std::uint8_t *data, const Mp4Box &av1c)
{
  constexpr std::uint64_t kRecordBytes = 4;
  if (av1c.payloadSize() <= kRecordBytes)
    return {};
  const auto begin = data + av1c.payloadOffset() + kRecordBytes;
  return std::vector<std::uint8_t>(begin, begin + (av1c.payloadSize() - kRecordBytes));
}

} // namespace

std::vector<Mp4Track>
readMp4Tracks(const std::uint8_t *data, std::size_t size, const Mp4ParseResult &boxes)
{
  std::vector<Mp4Track> tracks;
  if (data == nullptr)
    return tracks;

  const auto *moov = findMp4Box(boxes.boxes, "moov");
  if (moov == nullptr)
    return tracks;

  /* A fragmented file keeps its sample table in the fragments, not in `moov`.
   *
   * `mvex` in the movie header announces that, and each `moof` carries the real run of samples in
   * its `trun`. This reads the sample table only, so on such a file every track comes back with
   * nothing - and an empty list with no explanation reads as a parser failure rather than as a
   * shape we do not open. `mvex` is the reliable signal: a file can be fragmented and, at the
   * moment it is being written, not have a `moof` yet.
   */
  const bool fragmented = findMp4Box(moov->children, "mvex") != nullptr ||
                          findMp4Box(boxes.boxes, "moof") != nullptr;

  Reader fragmentReader{data, size, false};
  const auto fragmentSamples =
      fragmented ? readFragmentSamples(fragmentReader, boxes, *moov)
                 : std::map<std::uint32_t, std::vector<Mp4Sample>>{};

  Reader reader{data, size, false};

  for (const auto *trak : findMp4Children(*moov, "trak"))
  {
    Mp4Track track;

    if (const auto *tkhd = findMp4Box(trak->children, "tkhd"))
    {
      /* Version 1 widens creation/modification/duration by four bytes each, which moves track_id.
       * Reading it at the version 0 offset on a version 1 header yields a timestamp as the id.
       */
      const auto payload = tkhd->payloadOffset();
      const auto version = data[payload] ;
      track.id           = reader.u32(payload + (version == 1 ? 20 : 12));
    }

    const auto *mdia = findMp4Box(trak->children, "mdia");
    if (mdia == nullptr)
    {
      track.error = "The track has no mdia box.";
      tracks.push_back(track);
      continue;
    }

    if (const auto *mdhd = findMp4Box(mdia->children, "mdhd"))
    {
      const auto payload = mdhd->payloadOffset();
      const auto version = data[payload];
      track.timescale    = reader.u32(payload + (version == 1 ? 20 : 12));
    }
    if (const auto *hdlr = findMp4Box(mdia->children, "hdlr"))
      track.handlerType = reader.fourcc(hdlr->payloadOffset() + 8);

    const auto *stbl = findMp4Box(mdia->children, "minf/stbl");
    if (stbl == nullptr)
    {
      track.error = "The track has no sample table (mdia/minf/stbl).";
      tracks.push_back(track);
      continue;
    }

    // --- what kind of samples ---------------------------------------------------------------
    if (const auto *stsd = findMp4Box(stbl->children, "stsd"); stsd != nullptr && !stsd->children.empty())
    {
      const auto &entry  = stsd->children.front();
      track.sampleFormat = entry.type;
      track.width        = reader.u16(entry.offset + 32);
      track.height       = reader.u16(entry.offset + 34);
      for (const auto *av1c : findMp4Children(entry, "av1C"))
        track.av1ConfigObus = readAv1Config(data, *av1c);
    }

    // --- sizes ------------------------------------------------------------------------------
    std::vector<std::uint32_t> sizes;
    if (const auto *stsz = findMp4Box(stbl->children, "stsz"))
    {
      const auto payload  = stsz->payloadOffset();
      const auto uniform  = reader.u32(payload + 4);
      const auto count    = reader.u32(payload + 8);
      if (plausibleCount(count, size, uniform == 0 ? 4 : 1))
      {
        sizes.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
          sizes.push_back(uniform != 0 ? uniform
                                       : reader.u32(payload + 12 + std::uint64_t(index) * 4));
      }
      else
      {
        track.error = "stsz declares " + std::to_string(count) +
                      " samples, more than the file could hold.";
      }
    }
    else
    {
      track.error = "The track has no stsz box, so sample sizes are unknown.";
    }

    // --- chunk offsets ----------------------------------------------------------------------
    std::vector<std::uint64_t> chunkOffsets;
    if (const auto *stco = findMp4Box(stbl->children, "stco"))
    {
      const auto payload = stco->payloadOffset();
      const auto count   = reader.u32(payload + 4);
      if (plausibleCount(count, size, 4))
        for (std::uint32_t index = 0; index < count; ++index)
          chunkOffsets.push_back(reader.u32(payload + 8 + std::uint64_t(index) * 4));
    }
    else if (const auto *co64 = findMp4Box(stbl->children, "co64"))
    {
      // Files over 4 GB use 64-bit offsets. Same table, wider entries.
      const auto payload = co64->payloadOffset();
      const auto count   = reader.u32(payload + 4);
      if (plausibleCount(count, size, 8))
        for (std::uint32_t index = 0; index < count; ++index)
          chunkOffsets.push_back(reader.u64(payload + 8 + std::uint64_t(index) * 8));
    }

    // --- samples per chunk ------------------------------------------------------------------
    std::vector<SampleToChunk> runs;
    if (const auto *stsc = findMp4Box(stbl->children, "stsc"))
    {
      const auto payload = stsc->payloadOffset();
      const auto count   = reader.u32(payload + 4);
      if (plausibleCount(count, size, 12))
        for (std::uint32_t index = 0; index < count; ++index)
          runs.push_back({reader.u32(payload + 8 + std::uint64_t(index) * 12),
                          reader.u32(payload + 12 + std::uint64_t(index) * 12)});
    }

    std::string locateError;
    track.samples = locateSamples(sizes, chunkOffsets, runs, locateError);

    /* A fragmented file's sample table is empty by design, and the samples are in the fragments
     * instead. Only fall back to them when the table really had nothing: a file can carry both,
     * and the table is the authoritative description of what it covers.
     */
    bool fromFragments = false;
    if (track.samples.empty())
      if (const auto found = fragmentSamples.find(track.id); found != fragmentSamples.end())
      {
        track.samples  = found->second;
        fromFragments  = true;
        locateError.clear();
      }

    if (track.error.empty())
      track.error = locateError;

    /* Only when the samples came from the sample table. Fragment samples already carry their times
     * from `tfdt` plus the run's durations, and their sync flags from the `trun`.
     *
     * readSyncFlags in particular must not run on them: no `stss` means "every sample is a random
     * access point", which is right for a table that has none and wrong for a fragmented file,
     * which simply keeps that information somewhere else. It silently marked all 100 video samples
     * as key frames where the file has 10.
     */
    if (!fromFragments)
    {
      readTimes(reader, findMp4Box(stbl->children, "stts"), track.samples);
      readSyncFlags(reader, findMp4Box(stbl->children, "stss"), track.samples);
    }

    /* A sample that points outside the file is the failure that matters here: the hexdump panes
     * would read past the buffer. Report it rather than handing out the offset.
     */
    for (const auto &sample : track.samples)
      if (sample.offset + sample.size > size)
      {
        track.error = "A sample at offset " + std::to_string(sample.offset) + " (" +
                      std::to_string(sample.size) + " bytes) runs past the end of the file.";
        break;
      }

    if (track.error.empty() && reader.overran)
      track.error = "A sample table entry was read past the end of the file.";

    if (track.error.empty() && track.samples.empty() && fragmented)
      track.error = "This file is fragmented and no fragment describes this track: the sample "
                    "table is empty by design, and no moof carried a trun for it.";

    tracks.push_back(track);
  }

  return tracks;
}

} // namespace bda::container
