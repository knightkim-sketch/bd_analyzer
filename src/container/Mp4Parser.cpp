#include "container/Mp4Parser.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace bda::container
{

namespace
{

constexpr std::uint32_t kBasicHeader = 8;  //!< size(4) + type(4)
constexpr std::uint32_t kLargeHeader = 16; //!< ... plus largesize(8)

std::uint32_t readU32(const std::uint8_t *data, std::uint64_t offset)
{
  return (std::uint32_t(data[offset]) << 24) | (std::uint32_t(data[offset + 1]) << 16) |
         (std::uint32_t(data[offset + 2]) << 8) | std::uint32_t(data[offset + 3]);
}

std::uint64_t readU64(const std::uint8_t *data, std::uint64_t offset)
{
  return (std::uint64_t(readU32(data, offset)) << 32) | readU32(data, offset + 4);
}

/* The boxes that contain other boxes.
 *
 * A deliberate allowlist. `mdat` is the reason: it holds coded video, and any 8 bytes of that will
 * parse as a box header and yield a plausible-looking subtree. Descending only into known
 * containers means an unrecognised box is reported as a leaf, which is honest, instead of as
 * whatever its payload happened to look like.
 */
const std::set<std::string> &containerBoxes()
{
  static const std::set<std::string> boxes{
      "moov", "trak", "mdia", "minf", "stbl", "edts", "dinf", "mvex",
      "moof", "traf", "mfra", "udta", "skip", "strk", "sinf", "schi",
  };
  return boxes;
}

/* Boxes that are a FullBox - four bytes of version and flags - before their children start.
 *
 * `meta` is the one that matters and the one that is routinely got wrong: parsing its children
 * from the payload start reads the version word as a box size and the tree collapses. Measured on
 * an ffmpeg-written file, `udta/meta` is present in ordinary output, so this is not an exotic path.
 */
const std::set<std::string> &fullBoxContainers()
{
  static const std::set<std::string> boxes{"meta"};
  return boxes;
}

//!< `stsd` holds sample entries rather than plain boxes, so it is walked by its own rule.
constexpr const char *kSampleDescriptionBox = "stsd";

/* Bytes a sample entry occupies before its own child boxes begin, counted from the entry's box
 * header. There is no single answer - it depends on what kind of entry it is:
 *
 *   visual: 8 header + 78 fixed (6 reserved, 2 data_reference_index, 16 pre_defined/reserved,
 *           2+2 width/height, 4+4 resolutions, 4 reserved, 2 frame_count, 32 compressor_name,
 *           2 depth, 2 pre_defined) = 86. Verified: an `av01` entry at 457 had its `av1C` at 543.
 *   audio:  8 header + 28 fixed (6 reserved, 2 data_reference_index, 8 reserved, 2 channelcount,
 *           2 samplesize, 2 pre_defined, 2 reserved, 4 samplerate) = 36.
 *
 * Using the visual figure on an audio entry lands 50 bytes into the entry's `esds`, and the
 * descriptor bytes there parse as a box with a two-gigabyte size. That is exactly what happened
 * the first time this met a file with an audio track - a video-only file can never reach it.
 */
constexpr std::uint64_t kVisualSampleEntryPrefix = 86;
constexpr std::uint64_t kAudioSampleEntryPrefix  = 36;

//!< A plausible box header at `offset`: a sane size that fits, and a printable four character code.
bool looksLikeBoxAt(const std::uint8_t *data, std::uint64_t offset, std::uint64_t end)
{
  if (offset + kBasicHeader > end)
    return false;

  const auto size = readU32(data, offset);
  if (size < kBasicHeader || offset + size > end)
    return false;

  for (int character = 0; character < 4; ++character)
  {
    const auto value = data[offset + 4 + character];
    if (value < 0x20 || value > 0x7e)
      return false;
  }
  return true;
}

struct Walker
{
  const std::uint8_t *data{};
  std::uint64_t       size{};
  Mp4ParseResult     *result{};

  //!< Depth guard. Real files nest about six deep; anything past this is a malformed loop.
  static constexpr int kMaxDepth = 16;

  void fail(const std::string &message, bool isTruncation = false)
  {
    if (this->result->error.empty())
    {
      this->result->error     = message;
      this->result->truncated = isTruncation;
    }
  }

  //!< Parse siblings in [begin, end). Returns false once the structure stops making sense.
  bool walk(std::uint64_t begin, std::uint64_t end, int depth, std::vector<Mp4Box> &into)
  {
    if (depth > kMaxDepth)
    {
      this->fail("Box nesting deeper than " + std::to_string(kMaxDepth) + " levels.");
      return false;
    }

    std::uint64_t offset = begin;
    while (offset + kBasicHeader <= end)
    {
      Mp4Box box;
      box.offset     = offset;
      box.headerSize = kBasicHeader;

      const auto declared = readU32(this->data, offset);
      box.type            = std::string(reinterpret_cast<const char *>(this->data + offset + 4), 4);

      if (declared == 1)
      {
        if (offset + kLargeHeader > end)
        {
          this->fail("A 64-bit box size at offset " + std::to_string(offset) +
                         " runs past the end of its parent.",
                     true);
          return false;
        }
        box.headerSize = kLargeHeader;
        box.size       = readU64(this->data, offset + kBasicHeader);
      }
      else if (declared == 0)
      {
        /* "To the end of the enclosing container." Legal for the last top level box; kept as a
         * flag because in the middle of a file it means the writer was interrupted.
         */
        box.extendsToEnd = true;
        box.size         = end - offset;
      }
      else
      {
        box.size = declared;
      }

      if (box.size < box.headerSize)
      {
        this->fail("Box '" + box.type + "' at offset " + std::to_string(offset) + " declares size " +
                   std::to_string(box.size) + ", smaller than its own header.");
        return false;
      }
      if (offset + box.size > end)
      {
        this->fail("Box '" + box.type + "' at offset " + std::to_string(offset) + " declares " +
                       std::to_string(box.size) + " bytes but only " + std::to_string(end - offset) +
                       " remain.",
                   true);
        /* Keep it. A truncated final box is exactly what a file still being written looks like,
         * and its header is often the most useful thing on screen.
         */
        into.push_back(box);
        return false;
      }

      const auto payload    = box.payloadOffset();
      const auto payloadEnd = offset + box.size;

      if (box.type == kSampleDescriptionBox)
      {
        if (!this->walkSampleDescription(box, payload, payloadEnd, depth))
        {
          into.push_back(box);
          return false;
        }
      }
      else if (fullBoxContainers().count(box.type) != 0)
      {
        if (payload + 4 <= payloadEnd)
          this->walk(payload + 4, payloadEnd, depth + 1, box.children);
      }
      else if (containerBoxes().count(box.type) != 0)
      {
        this->walk(payload, payloadEnd, depth + 1, box.children);
      }

      into.push_back(box);
      offset += box.size;
    }

    /* Trailing bytes too few to be a header. Not an error at the top level of a padded file, but
     * inside a container it means the parent's size disagrees with its contents.
     */
    if (offset != end && depth > 0)
      this->fail(std::to_string(end - offset) + " bytes left over inside a container box at " +
                 std::to_string(offset) + ".");
    return this->result->ok();
  }

  /* `stsd` is version/flags, an entry count, then that many sample entries. Each entry is a box
   * whose payload starts with fixed fields before its own children - so the children cannot be
   * found by the generic rule.
   */
  bool walkSampleDescription(Mp4Box &box, std::uint64_t payload, std::uint64_t payloadEnd, int depth)
  {
    if (payload + 8 > payloadEnd)
    {
      this->fail("stsd at offset " + std::to_string(box.offset) + " is too short for its header.");
      return false;
    }

    const auto count  = readU32(this->data, payload + 4);
    auto       offset = payload + 8;

    for (std::uint32_t index = 0; index < count && offset + kBasicHeader <= payloadEnd; ++index)
    {
      Mp4Box entry;
      entry.offset     = offset;
      entry.headerSize = kBasicHeader;
      entry.size       = readU32(this->data, offset);
      entry.type = std::string(reinterpret_cast<const char *>(this->data + offset + 4), 4);

      if (entry.size < kBasicHeader || offset + entry.size > payloadEnd)
      {
        this->fail("Sample entry '" + entry.type + "' at offset " + std::to_string(offset) +
                       " does not fit in its stsd.",
                   true);
        return false;
      }

      /* Pick the prefix by looking, not by guessing from the four character code - that list would
       * be endless and every codec added later would land back in the `esds`. Whichever candidate
       * puts a plausible box header under it is the right one; if neither does, the entry is a
       * kind we do not know how to open and is reported as a leaf. That is the same rule as the
       * container allowlist: describe what is certain, invent nothing.
       */
      const auto entryEnd = offset + entry.size;
      for (const auto prefix : {kVisualSampleEntryPrefix, kAudioSampleEntryPrefix})
        if (looksLikeBoxAt(this->data, offset + prefix, entryEnd))
        {
          this->walk(offset + prefix, entryEnd, depth + 2, entry.children);
          break;
        }

      box.children.push_back(entry);
      offset += entry.size;
    }
    return true;
  }
};

} // namespace

bool isMp4ContainerBox(const std::string &type)
{
  return containerBoxes().count(type) != 0 || fullBoxContainers().count(type) != 0 ||
         type == kSampleDescriptionBox;
}

Mp4ParseResult parseMp4Boxes(const std::uint8_t *data, std::size_t size)
{
  Mp4ParseResult result;
  if (data == nullptr || size < kBasicHeader)
  {
    result.error = "The file is too short to contain a single box.";
    return result;
  }

  Walker walker{data, size, &result};
  walker.walk(0, size, 0, result.boxes);
  return result;
}

const Mp4Box *findMp4Box(const std::vector<Mp4Box> &boxes, const std::string &path)
{
  const std::vector<Mp4Box> *level   = &boxes;
  const Mp4Box              *current = nullptr;

  std::istringstream stream(path);
  std::string        segment;
  while (std::getline(stream, segment, '/'))
  {
    if (segment.empty())
      continue;

    const auto found =
        std::find_if(level->begin(), level->end(), [&segment](const Mp4Box &candidate) {
          return candidate.type == segment;
        });
    if (found == level->end())
      return nullptr;

    current = &*found;
    level   = &current->children;
  }
  return current;
}

std::vector<const Mp4Box *> findMp4Children(const Mp4Box &parent, const std::string &type)
{
  std::vector<const Mp4Box *> found;
  for (const auto &child : parent.children)
    if (child.type == type)
      found.push_back(&child);
  return found;
}

} // namespace bda::container
