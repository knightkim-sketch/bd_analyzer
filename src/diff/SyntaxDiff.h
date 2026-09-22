// Compare two sequences of parsed syntax elements and say where they diverge.
//
// Deliberately Qt-free and free of any bd_analyzer type: the caller flattens whatever it has - a
// parser tree, a block's entries - into name/value pairs and gets back a list of differences. That
// keeps the part worth testing out of the GUI, where it cannot be tested.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bda::diff
{

struct SyntaxElement
{
  std::string name;
  std::string value;
};

enum class DiffKind
{
  ValueMismatch, //!< Both streams have the element, with different values.
  OnlyInA,       //!< Present in A, with no counterpart in B.
  OnlyInB,       //!< Present in B, with no counterpart in A.
};

inline constexpr std::size_t NoIndex = static_cast<std::size_t>(-1);

struct SyntaxDiff
{
  DiffKind    kind{};
  std::size_t indexA{NoIndex}; //!< Position in A, NoIndex for OnlyInB.
  std::size_t indexB{NoIndex}; //!< Position in B, NoIndex for OnlyInA.
  std::string name;
  std::string valueA;
  std::string valueB;
};

struct SyntaxDiffOptions
{
  /* Alignment is a longest-common-subsequence over the element names, which costs one table cell
   * per pair. Header dumps run to a few hundred elements, so the table is small; a whole stream's
   * worth of block syntax is not. Past this many cells the alignment degrades to a forward scan
   * that resynchronises within a window, and `degraded` says so in the result.
   */
  std::size_t maxTableCells{4000000};
  std::size_t resyncWindow{64};
};

struct SyntaxDiffResult
{
  std::vector<SyntaxDiff> diffs;
  std::size_t             alignedPairs{}; //!< Elements matched by name, differing or not.
  bool                    degraded{};     //!< Alignment fell back to the windowed scan.

  bool identical() const { return this->diffs.empty(); }

  /* The first difference in bitstream order.
   *
   * This is the answer the caller usually wants: everything after the first divergence is suspect,
   * because a syntax element that reads differently moves every later read with it.
   */
  const SyntaxDiff *first() const { return this->diffs.empty() ? nullptr : &this->diffs.front(); }
};

SyntaxDiffResult compareSyntax(const std::vector<SyntaxElement> &a,
                               const std::vector<SyntaxElement> &b,
                               const SyntaxDiffOptions &         options = {});

/* A named run of elements - one OBU, one frame header, whatever the caller wants to compare as a
 * unit.
 *
 * Comparing a whole stream as one sequence is both slower and less useful than comparing it
 * section by section. A 24 frame clip flattens to roughly 13000 elements, which puts the exact
 * alignment far past any sane table size; split into frames it is a few hundred per section, so
 * the alignment stays exact. The report also gains the locality that matters - "frame 7 differs"
 * rather than "element 8412 differs".
 */
struct SyntaxSection
{
  std::string                label;
  std::vector<SyntaxElement> elements;
};

struct SectionDiff
{
  std::string      label;
  bool             onlyInA{}; //!< A has this section and B has no counterpart.
  bool             onlyInB{};
  SyntaxDiffResult result;    //!< Empty when the section exists on one side only.

  bool differs() const { return this->onlyInA || this->onlyInB || !this->result.identical(); }
};

struct SectionDiffResult
{
  std::vector<SectionDiff> sections;
  std::size_t              totalDiffs{};

  bool identical() const { return this->totalDiffs == 0; }

  const SectionDiff *firstDiffering() const
  {
    for (const auto &s : this->sections)
      if (s.differs())
        return &s;
    return nullptr;
  }
};

/* Compare section by section, pairing them by position.
 *
 * Position, not label: the labels repeat ("Frame", "Frame", ...) so matching on them would pair
 * arbitrary frames. Both streams are expected to code the same source in the same order, and the
 * caller is expected to have refused the comparison when the frame counts differ; sections past
 * the shorter side are reported as present on one side only rather than silently dropped.
 */
SectionDiffResult compareSections(const std::vector<SyntaxSection> &a,
                                  const std::vector<SyntaxSection> &b,
                                  const SyntaxDiffOptions &         options = {});

} // namespace bda::diff
