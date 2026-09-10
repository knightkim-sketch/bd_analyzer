// What the assistant panel tells the model about what is on screen, and how that becomes text.
//
// Free of Qt and of the upstream tree, like src/bdrate and src/me, so the serialization can be
// unit tested without a build of the library behind it. The collector that fills these structs
// from playlist items lives in src/integration, which is where upstream types are allowed.
//
// The point of keeping this separate and tested: a wrong number here does not produce an error,
// it produces a confident wrong answer. The panel shows the user exactly this text before sending
// it, and these tests pin what that text says.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bda::assist
{

//!< One name/value line of block syntax, as the Block Info panel would show it.
struct SyntaxEntry
{
  std::string name;
  std::string value;
};

//!< The stream (or raw file) a question is about.
struct StreamInfo
{
  std::string path;
  int         width{};
  int         height{};
  int         superblockSize{}; //!< 64 or 128. Zero when the sequence header is not parsed yet.
  std::string codec;
  std::string decoder;      //!< "dav1d" or the fallback. Decides which statistics exist at all.
  std::string originalPath; //!< Attached source YUV/Y4M. Empty when none, which disables SSE.
};

/* Measurements of one region - a superblock, or a whole frame.
 *
 * `sse` and `sampleCount` are carried rather than a precomputed PSNR so the text can distinguish
 * "lossless" from "no original attached": both would otherwise arrive as a missing PSNR.
 */
struct RegionStats
{
  std::optional<double>       bits;
  std::optional<double>       sse;
  std::optional<std::int64_t> sampleCount;

  //!< PSNR when it exists. Nothing when the SSE is missing, or when the region is lossless.
  std::optional<double> psnr() const;
  bool                  lossless() const { return this->sse && *this->sse == 0.0; }
};

//!< The superblock the user clicked.
struct SuperblockInfo
{
  int                      column{};
  int                      row{};
  int                      pixelX{};
  int                      pixelY{};
  RegionStats              stats;
  std::vector<SyntaxEntry> syntax;
};

/* Everything the panel may attach to a question.
 *
 * Each part is optional because each has its own checkbox: the user decides how much goes out,
 * and the default is the current frame plus the clicked block. Sequence and BD-rate figures cost
 * a sweep to produce, so they are never attached implicitly.
 */
struct AssistContext
{
  std::optional<StreamInfo>     stream;
  std::optional<int>            frameIdx;
  std::optional<RegionStats>    frameStats;
  std::optional<SuperblockInfo> superblock;

  //!< BD-rate group name -> its figure as the window renders it ("+3.21 %", "no PSNR overlap").
  std::vector<std::pair<std::string, std::string>> bdRate;

  bool empty() const;
};

/* Render the context as the text that goes to the model, and to the "what was sent" box.
 *
 * Markdown rather than JSON: this is read by a person as often as by a model, and the panel shows
 * it verbatim. A missing value is written as an explicit absence ("no original attached") rather
 * than omitted, because a silently absent line reads as "not measured" when it often means
 * "cannot be measured here" - and that distinction is the whole point of showing it.
 */
std::string renderContext(const AssistContext &context);

} // namespace bda::assist
