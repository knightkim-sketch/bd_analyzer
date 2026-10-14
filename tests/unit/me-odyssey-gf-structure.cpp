// Unit test: odyssey's GF structure for open-loop ME (src/me/OdysseyGfStructure).
//
// The expected table is odyssey's own: odyssey-pristine 0fab39b0 encoding 40 frames of
// Motorcycle_512x288 with --max-gf-interval 16 --enable-tdmp 0 --enable-tf 0, read from
// trace_anl_me/input_trace/analyzer_frame_info_*.txt (cur_order_hint, refN_order_hint,
// refN_tmv_buf_cnt). Nothing here is derived from the code under test.
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "me/OdysseyGfStructure.h"

using namespace bda::me;

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

struct Row
{
  int poc;
  int ref0, ref1; // list-0 reference; list-1 reference, or P's second list-0 reference (-1: none)
  int tmv0, tmv1; // TMV buffer counts as odyssey traced them (-1: no second search)
};

// clang-format off
const Row kTrace[] = {
  { 1,  0,  2, 0, 0}, { 2,  0,  4, 3, 3}, { 3,  2,  4, 1, 1}, { 4,  0,  8, 6, 6},
  { 5,  4,  6, 2, 2}, { 6,  4,  8, 5, 5}, { 7,  6,  8, 2, 2}, { 8,  0, 16, 8, 8},
  { 9,  8, 10, 2, 2}, {10,  8, 12, 5, 5}, {11, 10, 12, 2, 2}, {12,  8, 16, 6, 6},
  {13, 12, 14, 2, 2}, {14, 12, 16, 4, 4}, {15, 14, 16, 2, 2}, {16,  0, -1, 7, -1},
  {17, 16, 18, 2, 1}, {18, 16, 20, 5, 4}, {19, 18, 20, 3, 2}, {20, 16, 24, 7, 6},
  {21, 20, 22, 2, 2}, {22, 20, 24, 5, 5}, {23, 22, 24, 2, 2}, {24, 16, 32, 8, 8},
  {25, 24, 26, 2, 2}, {26, 24, 28, 5, 5}, {27, 26, 28, 2, 2}, {28, 24, 32, 6, 6},
  {29, 28, 30, 2, 2}, {30, 28, 32, 4, 4}, {31, 30, 32, 2, 2}, {32, 16,  0, 8, 8},
  {33, 32, 34, 2, 1}, {34, 32, 36, 4, 3}, {35, 34, 36, 3, 2}, {36, 32, 16, 4, 4},
  {37, 36, 38, 2, 1}, {38, 36, 32, 3, 3}, {39, 38, 36, 2, 2},
};
// clang-format on

std::string str(const std::vector<int> &v)
{
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i)
    s += (i ? "," : "") + std::to_string(v[i]);
  return s + "]";
}

} // namespace

int main()
{
  const auto plan = planOdysseyGf(40, 16);

  // Layers and order of the first group (design section 1.1 / 1.2).
  check(plan.frames.at(16).layer == 1 && plan.frames.at(8).layer == 2 &&
            plan.frames.at(4).layer == 3 && plan.frames.at(12).layer == 3 &&
            plan.frames.at(2).layer == 4 && plan.frames.at(14).layer == 4 &&
            plan.frames.at(1).layer == 5 && plan.frames.at(15).layer == 5,
        "GF 16 layers: 16 L1, 8 L2, 4/12 L3, 2..14 L4, odd L5");
  const std::vector<int> firstGroup(plan.analysisOrder.begin() + 1, plan.analysisOrder.begin() + 17);
  check(firstGroup == std::vector<int>({1, 3, 5, 7, 9, 11, 13, 15, 2, 6, 10, 14, 4, 12, 8, 16}),
        "analysis order is bottom-up: " + str(firstGroup));
  check(plan.frames.at(36).kind == OdyFrameKind::P && plan.frames.at(38).kind == OdyFrameKind::P &&
            plan.frames.at(39).kind == OdyFrameKind::P && plan.frames.at(37).kind == OdyFrameKind::B,
        "the 7-frame tail splits 4 + 2 + 1: anchors 36, 38, 39");

  // Every frame against odyssey's trace.
  for (const auto &row : kTrace)
  {
    const auto &f   = plan.frames.at(row.poc);
    const bool  isP = f.kind == OdyFrameKind::P;
    std::vector<int> wantL0{row.ref0}, wantL1;
    if (row.ref1 >= 0)
      (isP ? wantL0 : wantL1).push_back(row.ref1);
    const bool refsOk = f.refs[0] == wantL0 && f.refs[1] == wantL1;
    // P's second search is another list-0 reference: same TMV list.
    const int  tmv1   = isP ? int(f.tmvFrames[0].size()) : int(f.tmvFrames[1].size());
    const bool tmvOk  = int(f.tmvFrames[0].size()) == row.tmv0 && (row.tmv1 < 0 || tmv1 == row.tmv1);
    check(refsOk && tmvOk,
          "POC " + std::to_string(row.poc) + ": L0 " + str(f.refs[0]) + " L1 " + str(f.refs[1]) +
              " TMV " + std::to_string(f.tmvFrames[0].size()) + "/" + std::to_string(tmv1) +
              " (odyssey: " + std::to_string(row.ref0) + "," + std::to_string(row.ref1) + " TMV " +
              std::to_string(row.tmv0) + "/" + std::to_string(row.tmv1) + ")");
  }

  // Smaller GF intervals (design section 1.1).
  const auto gf8 = planOdysseyGf(9, 8);
  check(gf8.frames.at(8).layer == 1 && gf8.frames.at(4).layer == 2 && gf8.frames.at(2).layer == 3 &&
            gf8.frames.at(6).layer == 3 && gf8.frames.at(1).layer == 5,
        "GF 8 layers: 8 L1, 4 L2, 2/6 L3, odd L5");
  const auto gf2 = planOdysseyGf(5, 2);
  check(gf2.frames.at(2).layer == 1 && gf2.frames.at(1).layer == 5 && gf2.frames.at(4).layer == 1,
        "GF 2: anchors on even frames, odd frames are leaves");

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
