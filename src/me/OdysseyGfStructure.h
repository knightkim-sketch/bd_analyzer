// odyssey's GF (golden-frame) group structure as its open-loop ME sees it: which layer each frame
// sits on, the order the lookahead analyses frames in, and which frames each one references and
// borrows temporal MVs (TMV) from.
//
// Reproduces odyssey-pristine 0fab39b0 with TDMP off and a single tile:
//   layers          common/source/ody_structure.c:102-141, :388-418 (hl_build_up_sublayers)
//   analysis order  analyzer/source/lookahead.c:4465-4474 - layer 5 first, layer 1 last
//   references/TMV  common/source/ody_me_common.c:324-469 (hl_set_references)
//   stored MV ref   common/source/ody_framebuffer.c:657-729 (hl_update_reference_info)
// The buffer model - {prev golden, prev last, golden} plus the current group - and the split of a
// tail shorter than the GF interval were fitted to odyssey's own analyzer trace of a 40-frame run
// (docs/ai/30-designs/odyssey-openloop-adv-design.md section 3).
//
// Qt-free like the rest of src/me.
#pragma once

#include <map>
#include <vector>

namespace bda::me
{

enum class OdyFrameKind
{
  Key, //!< Frame 0: intra, no ME.
  B,   //!< Inside a group: one reference per list.
  P,   //!< Group anchor (odyssey's ARF / last frame): list 0 only, up to two references.
};

struct OdyFrameInfo
{
  int          poc{};
  OdyFrameKind kind{OdyFrameKind::Key};
  int          layer{}; //!< 1 = anchor ... 5 = leaf; 0 for the key frame.
  int          groupFirst{}, groupAnchor{};
  //!< References per list, nearest first - the order the SB loop searches them in.
  std::vector<int> refs[2];
  //!< Frames whose stored MVs are TMV candidates per list, in candidate-slot order (at most 8).
  std::vector<int> tmvFrames[2];
  //!< The reference recorded with this frame's stored MVs per list (-1: none, e.g. list 1 of P).
  int storedRef[2]{-1, -1};
};

struct OdyGfPlan
{
  std::vector<int>            analysisOrder; //!< POCs in the order the lookahead analyses them.
  std::map<int, OdyFrameInfo> frames;        //!< Every POC in [0, frameCount).
};

/* The analysis plan for frames [0, frameCount). gfInterval is a power of two from 1 to 16;
 * hierarchyLevel is odyssey's hierarchy_lv (4 by default) - deeper midpoints become leaves.
 */
OdyGfPlan planOdysseyGf(int frameCount, int gfInterval, int hierarchyLevel = 4);

} // namespace bda::me
