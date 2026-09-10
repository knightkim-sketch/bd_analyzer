// ISO base media file format (MP4) box structure.
//
// Free of Qt and of the upstream tree, like src/bdrate and src/assist, so it unit tests without a
// build of the library behind it.
//
// Why this exists when the app already opens .mp4: it opens them through libavformat, which hands
// back packets and extradata and nothing about the container itself. There is no way to see the
// box hierarchy, and no way to point at where in the file a sample actually lives - which is what
// the frame and block hexdump panes need. This walks the boxes and reads the sample table so an
// MP4 can be inspected the way an IVF already can.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bda::container
{

//!< One box. `size` includes the header, so `offset + size` is where the next sibling starts.
struct Mp4Box
{
  std::string   type; //!< The four character code, verbatim.
  std::uint64_t offset{};
  std::uint64_t size{};
  std::uint32_t headerSize{}; //!< 8 normally, 16 when the 64-bit largesize form is used.

  /* Set when the box declared `size == 0`, meaning "to the end of the file". Legal only for the
   * last top level box, and worth flagging rather than silently normalising: a 0 in the middle of
   * a file is a sign of a truncated or streamed capture.
   */
  bool extendsToEnd{};

  std::vector<Mp4Box> children;

  std::uint64_t payloadOffset() const { return this->offset + this->headerSize; }
  std::uint64_t payloadSize() const { return this->size - this->headerSize; }
};

struct Mp4ParseResult
{
  std::vector<Mp4Box> boxes; //!< Top level boxes, in file order.

  /* Set when the structure could not be walked any further. Parsing keeps whatever it read before
   * the problem: a file that is being written, or was cut short, still has a usable moov and is
   * worth showing.
   */
  std::string error;

  //!< A box claimed more bytes than the file holds. `error` is set too; this says which kind.
  bool truncated{};

  bool ok() const { return this->error.empty(); }
};

/* Walk the box tree.
 *
 * Recursion is limited to the boxes that are known to contain other boxes. That is not an
 * optimisation - `mdat` holds coded samples, and treating those bytes as a box header produces a
 * tree of convincing-looking nonsense.
 */
Mp4ParseResult parseMp4Boxes(const std::uint8_t *data, std::size_t size);

//!< True for the container boxes parseMp4Boxes() descends into. Exposed so the UI can match it.
bool isMp4ContainerBox(const std::string &type);

/* Find one box by a slash separated path from the top level, e.g. "moov/trak/mdia/minf/stbl".
 *
 * Returns the first match at each level. That is enough for the boxes this is used for, and a file
 * with several tracks is handled by searching within a `trak` the caller already picked, not by
 * making this smarter.
 */
const Mp4Box *findMp4Box(const std::vector<Mp4Box> &boxes, const std::string &path);

//!< Every direct child with this type.
std::vector<const Mp4Box *> findMp4Children(const Mp4Box &parent, const std::string &type);

} // namespace bda::container
