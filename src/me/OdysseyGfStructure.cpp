#include "OdysseyGfStructure.h"

#include <algorithm>
#include <set>

namespace bda::me
{
namespace
{

constexpr int kLeafLayer = 5; // odyssey's MAX_ARF_LAYERS

// hl_build_up_sublayers: frames strictly between start and end, relative to the group start.
void buildSublayers(std::map<int, int> &layerOf, int start, int end, int depth, int maxDepth)
{
  const int remaining = end - start - 1;
  if (remaining <= 0)
    return;
  if (depth > maxDepth || remaining < 3)
  {
    for (int f = start + 1; f < end; ++f)
      layerOf[f] = kLeafLayer;
    return;
  }
  const int half = (start + end) / 2;
  layerOf[half]  = depth;
  buildSublayers(layerOf, start, half, depth + 1, maxDepth);
  buildSublayers(layerOf, half, end, depth + 1, maxDepth);
}

int largestPowerOfTwoAtMost(int n)
{
  int p = 1;
  while (p * 2 <= n)
    p *= 2;
  return p;
}

} // namespace

OdyGfPlan planOdysseyGf(int frameCount, int gfInterval, int hierarchyLevel)
{
  OdyGfPlan plan;
  if (frameCount <= 0)
    return plan;
  gfInterval = std::clamp(largestPowerOfTwoAtMost(std::max(1, gfInterval)), 1, 16);

  plan.frames[0].poc  = 0;
  plan.frames[0].kind = OdyFrameKind::Key;
  plan.analysisOrder.push_back(0);

  // Groups: full GF intervals, then the tail split into the largest powers of two that fit
  // (odyssey's trace: 7 frames left with GF 16 became 4 + 2 + 1).
  std::vector<std::pair<int, int>> groups; // [first, anchor]
  for (int pos = 1; pos < frameCount;)
  {
    const int left = frameCount - pos;
    const int size = left >= gfInterval ? gfInterval : largestPowerOfTwoAtMost(left);
    groups.emplace_back(pos, pos + size - 1);
    pos += size;
  }

  std::set<int> analysed; // the key frame is never analysed: odyssey runs no ME on it
  int           golden = 0, prevGolden = -1, prevLast = -1;

  for (const auto &[first, anchor] : groups)
  {
    // Layers, relative to the previous anchor at offset 0.
    std::map<int, int> rel;
    rel[anchor - (first - 1)] = 1;
    buildSublayers(rel, 0, anchor - (first - 1), 2, hierarchyLevel);
    for (const auto &[offset, layer] : rel)
    {
      auto &f       = plan.frames[first - 1 + offset];
      f.poc         = first - 1 + offset;
      f.layer       = layer;
      f.kind        = layer == 1 ? OdyFrameKind::P : OdyFrameKind::B;
      f.groupFirst  = first;
      f.groupAnchor = anchor;
    }

    // The analyser's reference buffer while this group is analysed.
    std::vector<int> buffer;
    if (prevGolden >= 0)
      buffer.push_back(prevGolden);
    if (prevLast >= 1 && prevLast != golden)
      buffer.push_back(prevLast);
    buffer.push_back(golden);
    for (int p = first; p <= anchor; ++p)
      buffer.push_back(p);
    const auto layerOf = [&plan](int poc) { return plan.frames[poc].layer; };

    // Bottom-up: layer 5 first, display order within a layer.
    for (int layer = kLeafLayer; layer >= 1; --layer)
      for (int poc = first; poc <= anchor; ++poc)
      {
        auto &cur = plan.frames[poc];
        if (cur.layer != layer)
          continue;

        // References: nearest frame on each side whose layer is not above ours.
        int best0 = -1, best1 = -1;
        for (int r : buffer)
        {
          if (r == poc || layerOf(r) > cur.layer)
            continue;
          if (r < poc && (best0 < 0 || r > best0))
            best0 = r;
          if (r > poc && (best1 < 0 || r < best1))
            best1 = r;
        }
        if (best0 >= 0)
          cur.refs[0].push_back(best0);
        if (cur.kind == OdyFrameKind::P)
        {
          // P has no list 1 but a second list-0 reference: the golden before the last one.
          if (prevGolden >= 0 && prevGolden != best0)
            cur.refs[0].push_back(prevGolden);
        }
        else if (best1 >= 0)
          cur.refs[1].push_back(best1);

        // TMV frames.
        for (int list = 0; list < 2; ++list)
        {
          auto &tmv = cur.tmvFrames[list];
          if (cur.layer == 1)
          {
            if (analysed.count(golden) && layerOf(golden) == 1)
              tmv.push_back(golden);
            // Then up to two per layer from layer 2 down, scanning the buffer from its newest
            // slot - odyssey walks ANAL_NUM_REF_FRAMES-1 down to 0.
            for (int l = 2; l <= kLeafLayer && tmv.size() < 8; ++l)
            {
              int found = 0;
              for (auto it = buffer.rbegin(); it != buffer.rend() && found < 2; ++it)
                if (analysed.count(*it) && layerOf(*it) == l)
                {
                  tmv.push_back(*it);
                  ++found;
                }
            }
          }
          else
          {
            for (int d = 1; d <= 4; ++d)
              for (int side : {-d, d})
              {
                const int t = poc + side;
                if (std::find(buffer.begin(), buffer.end(), t) == buffer.end() || !analysed.count(t))
                  continue;
                if (list == 1 && layerOf(t) < 2)
                  continue;
                tmv.push_back(t);
              }
          }
        }

        // The reference stored with this frame's MVs (hl_update_reference_info): strictly lower
        // layers only, except that an anchor may point at another anchor. Anchors store list 0 only.
        for (int list = 0; list < (cur.layer == 1 ? 1 : 2); ++list)
        {
          int best = -1;
          for (int r : buffer)
          {
            if (r == poc)
              continue;
            const bool layerOk = cur.layer <= 1 ? layerOf(r) <= cur.layer : layerOf(r) < cur.layer;
            if (!layerOk)
              continue;
            if (list == 0 && r < poc && (best < 0 || r > best))
              best = r;
            if (list == 1 && r > poc && (best < 0 || r < best))
              best = r;
          }
          cur.storedRef[list] = best;
        }

        plan.analysisOrder.push_back(poc);
        analysed.insert(poc);
      }

    prevGolden = golden;
    prevLast   = anchor - 1;
    golden     = anchor;
  }
  return plan;
}

} // namespace bda::me
