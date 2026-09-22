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

} // namespace bda::diff
