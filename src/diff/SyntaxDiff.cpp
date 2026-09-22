#include "SyntaxDiff.h"

#include <algorithm>
#include <cstdint>

namespace bda::diff
{

namespace
{

/* One step of the alignment: either a pair (both indices set) or an unpaired element. */
struct Step
{
  std::size_t a{NoIndex};
  std::size_t b{NoIndex};
};

/* Longest common subsequence over the element names.
 *
 * Names, not values: an element whose value changed is still the same element, and pairing it is
 * exactly what lets us report a value mismatch instead of two unrelated insertions.
 */
std::vector<Step> alignByLcs(const std::vector<SyntaxElement> &a, const std::vector<SyntaxElement> &b)
{
  const auto n = a.size();
  const auto m = b.size();

  // table[i][j] = LCS length of a[i..] and b[j..]; one extra row and column of zeroes.
  std::vector<std::vector<std::uint32_t>> table(n + 1, std::vector<std::uint32_t>(m + 1, 0));
  for (std::size_t i = n; i-- > 0;)
    for (std::size_t j = m; j-- > 0;)
      table[i][j] = (a[i].name == b[j].name)
                        ? table[i + 1][j + 1] + 1
                        : std::max(table[i + 1][j], table[i][j + 1]);

  std::vector<Step> steps;
  steps.reserve(n + m);
  std::size_t i = 0, j = 0;
  while (i < n && j < m)
  {
    if (a[i].name == b[j].name)
    {
      steps.push_back({i, j});
      ++i;
      ++j;
    }
    else if (table[i + 1][j] >= table[i][j + 1])
      steps.push_back({i++, NoIndex});
    else
      steps.push_back({NoIndex, j++});
  }
  for (; i < n; ++i)
    steps.push_back({i, NoIndex});
  for (; j < m; ++j)
    steps.push_back({NoIndex, j});
  return steps;
}

/* Forward scan that resynchronises within a window, for inputs too large to align exactly.
 *
 * On a name mismatch it looks ahead in both sequences for the nearest name that would let the scan
 * line up again, and reports the skipped elements as unpaired. Whichever side needs the shorter
 * skip wins; a tie is resolved in favour of A so the result is deterministic.
 */
std::vector<Step> alignByWindow(const std::vector<SyntaxElement> &a,
                                const std::vector<SyntaxElement> &b,
                                const std::size_t                 window)
{
  const auto n = a.size();
  const auto m = b.size();

  std::vector<Step> steps;
  std::size_t       i = 0, j = 0;
  while (i < n && j < m)
  {
    if (a[i].name == b[j].name)
    {
      steps.push_back({i++, j++});
      continue;
    }

    auto skipInA = NoIndex;
    auto skipInB = NoIndex;
    for (std::size_t k = 1; k <= window; ++k)
    {
      if (skipInA == NoIndex && i + k < n && a[i + k].name == b[j].name)
        skipInA = k;
      if (skipInB == NoIndex && j + k < m && a[i].name == b[j + k].name)
        skipInB = k;
      if (skipInA != NoIndex || skipInB != NoIndex)
        break;
    }

    if (skipInA == NoIndex && skipInB == NoIndex)
    {
      // No resynchronisation in reach. Treat this one position as changed on both sides and move
      // on, rather than declaring the whole remainder unpaired.
      steps.push_back({i++, NoIndex});
      steps.push_back({NoIndex, j++});
      continue;
    }
    if (skipInB == NoIndex || (skipInA != NoIndex && skipInA <= skipInB))
      for (std::size_t k = 0; k < skipInA; ++k)
        steps.push_back({i++, NoIndex});
    else
      for (std::size_t k = 0; k < skipInB; ++k)
        steps.push_back({NoIndex, j++});
  }
  for (; i < n; ++i)
    steps.push_back({i, NoIndex});
  for (; j < m; ++j)
    steps.push_back({NoIndex, j});
  return steps;
}

} // namespace

SyntaxDiffResult compareSyntax(const std::vector<SyntaxElement> &a,
                               const std::vector<SyntaxElement> &b,
                               const SyntaxDiffOptions &         options)
{
  SyntaxDiffResult result;

  const auto cells = (a.size() + 1) * (b.size() + 1);
  const bool exact = cells <= options.maxTableCells;
  result.degraded  = !exact;

  const auto steps = exact ? alignByLcs(a, b) : alignByWindow(a, b, options.resyncWindow);

  for (const auto &step : steps)
  {
    if (step.a != NoIndex && step.b != NoIndex)
    {
      ++result.alignedPairs;
      if (a[step.a].value != b[step.b].value)
        result.diffs.push_back({DiffKind::ValueMismatch,
                                step.a,
                                step.b,
                                a[step.a].name,
                                a[step.a].value,
                                b[step.b].value});
    }
    else if (step.a != NoIndex)
      result.diffs.push_back(
          {DiffKind::OnlyInA, step.a, NoIndex, a[step.a].name, a[step.a].value, {}});
    else
      result.diffs.push_back(
          {DiffKind::OnlyInB, NoIndex, step.b, b[step.b].name, {}, b[step.b].value});
  }
  return result;
}

SectionDiffResult compareSections(const std::vector<SyntaxSection> &a,
                                  const std::vector<SyntaxSection> &b,
                                  const SyntaxDiffOptions &         options)
{
  SectionDiffResult result;
  const auto        common = std::min(a.size(), b.size());

  for (std::size_t i = 0; i < common; ++i)
  {
    SectionDiff section;
    section.label  = a[i].label;
    section.result = compareSyntax(a[i].elements, b[i].elements, options);
    result.totalDiffs += section.result.diffs.size();
    result.sections.push_back(std::move(section));
  }
  for (std::size_t i = common; i < a.size(); ++i)
  {
    result.sections.push_back({a[i].label, true, false, {}});
    ++result.totalDiffs;
  }
  for (std::size_t i = common; i < b.size(); ++i)
  {
    result.sections.push_back({b[i].label, false, true, {}});
    ++result.totalDiffs;
  }
  return result;
}

} // namespace bda::diff
