// Unit test: aligning and comparing two sequences of syntax elements.
//
// The alignment is the part that earns a test. Comparing values is trivial once the elements are
// paired; pairing them when one side has an element the other does not is where a comparison
// quietly turns into nonsense - every later element shifts by one and reads as changed.
#include "diff/SyntaxDiff.h"

#include <iostream>
#include <string>

using namespace bda::diff;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
  if (!ok)
    ++failures;
}

template <typename T> void checkEqual(const T &got, const T &want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok   " : "  FAIL ") << what << ": " << got;
  if (!ok)
    std::cout << " (expected " << want << ")";
  std::cout << "\n";
  if (!ok)
    ++failures;
}

std::vector<SyntaxElement> seq(std::initializer_list<std::pair<const char *, const char *>> items)
{
  std::vector<SyntaxElement> out;
  for (const auto &[n, v] : items)
    out.push_back({n, v});
  return out;
}
} // namespace

int main()
{
  {
    const auto a = seq({{"seq_profile", "0"}, {"still_picture", "0"}, {"base_q_idx", "26"}});
    const auto r = compareSyntax(a, a);
    check(r.identical(), "identical input reports no difference");
    checkEqual(r.alignedPairs, std::size_t(3), "every element pairs");
    check(r.first() == nullptr, "no first difference");
    check(!r.degraded, "small input aligns exactly");
  }

  {
    const auto a = seq({{"seq_profile", "0"}, {"base_q_idx", "26"}, {"tx_mode_select", "1"}});
    const auto b = seq({{"seq_profile", "0"}, {"base_q_idx", "70"}, {"tx_mode_select", "1"}});
    const auto r = compareSyntax(a, b);
    checkEqual(r.diffs.size(), std::size_t(1), "one difference");
    checkEqual(r.alignedPairs, std::size_t(3), "all three still pair");
    check(r.first() != nullptr && r.first()->kind == DiffKind::ValueMismatch, "value mismatch");
    checkEqual(r.first()->name, std::string("base_q_idx"), "names the element");
    checkEqual(r.first()->valueA, std::string("26"), "value from A");
    checkEqual(r.first()->valueB, std::string("70"), "value from B");
    checkEqual(r.first()->indexA, std::size_t(1), "index in A");
    checkEqual(r.first()->indexB, std::size_t(1), "index in B");
  }

  {
    // B reads one element A never had. Everything after it must still pair - that is the whole
    // point of aligning rather than comparing position by position.
    const auto a = seq({{"uniform_tile_spacing_flag", "1"}, {"context_update_tile_id", "3"}});
    const auto b = seq({{"uniform_tile_spacing_flag", "1"},
                        {"increment_tile_rows_log2", "1"},
                        {"context_update_tile_id", "3"}});
    const auto r = compareSyntax(a, b);
    checkEqual(r.diffs.size(), std::size_t(1), "only the extra element differs");
    check(r.first()->kind == DiffKind::OnlyInB, "classified as only in B");
    checkEqual(r.first()->name, std::string("increment_tile_rows_log2"), "names the extra element");
    checkEqual(r.first()->indexA, NoIndex, "no index in A");
    checkEqual(r.first()->indexB, std::size_t(1), "index in B");
    checkEqual(r.alignedPairs, std::size_t(2), "the surrounding elements still pair");
  }

  {
    const auto a = seq({{"a", "1"}, {"extra", "9"}, {"b", "2"}});
    const auto b = seq({{"a", "1"}, {"b", "2"}});
    const auto r = compareSyntax(a, b);
    checkEqual(r.diffs.size(), std::size_t(1), "only in A: one difference");
    check(r.first()->kind == DiffKind::OnlyInA, "classified as only in A");
    checkEqual(r.first()->indexB, NoIndex, "no index in B");
  }

  {
    // first() must be the earliest in bitstream order, not merely any difference.
    const auto a = seq({{"x", "1"}, {"y", "2"}, {"z", "3"}});
    const auto b = seq({{"x", "9"}, {"y", "8"}, {"z", "7"}});
    const auto r = compareSyntax(a, b);
    checkEqual(r.diffs.size(), std::size_t(3), "three value mismatches");
    checkEqual(r.first()->name, std::string("x"), "first difference is the earliest");
  }

  {
    // Force the windowed fallback and check it still finds the same answer on an easy input.
    SyntaxDiffOptions opts;
    opts.maxTableCells = 1;
    const auto a       = seq({{"a", "1"}, {"b", "2"}, {"c", "3"}});
    const auto b       = seq({{"a", "1"}, {"inserted", "9"}, {"b", "2"}, {"c", "3"}});
    const auto r       = compareSyntax(a, b, opts);
    check(r.degraded, "reports that alignment degraded");
    checkEqual(r.diffs.size(), std::size_t(1), "windowed scan finds the one insertion");
    check(r.first()->kind == DiffKind::OnlyInB, "and classifies it correctly");
    checkEqual(r.alignedPairs, std::size_t(3), "the rest still pairs");
  }

  {
    const auto empty = std::vector<SyntaxElement>{};
    const auto a     = seq({{"a", "1"}});
    checkEqual(compareSyntax(empty, empty).diffs.size(), std::size_t(0), "empty vs empty");
    checkEqual(compareSyntax(a, empty).diffs.size(), std::size_t(1), "everything only in A");
    checkEqual(compareSyntax(empty, a).diffs.size(), std::size_t(1), "everything only in B");
  }

  std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
  return failures == 0 ? 0 : 1;
}
