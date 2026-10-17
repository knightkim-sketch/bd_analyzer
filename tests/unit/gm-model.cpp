// Unit test: RANSAC global model over block ME vectors (src/me/GlobalModel).
//
// The vector fields are made from known models, rounded to eighth-pel as an estimator would give
// them, with 30% of the blocks replaced by unrelated vectors (moving objects, mismatches). Checked:
//   * translation, rotzoom and affine are recovered within a hundredth of a pixel / of a percent;
//   * exactly the corrupted blocks are the outliers;
//   * the fit is deterministic - two runs give bit-identical parameters and masks;
//   * static-bypass blocks are (0,0) points (a still pair fits the zero model), blocks past the
//     picture edge are not points;
//   * too few points is an error, not a made-up model;
//   * the synthesised seed covers every block of every size of the superblock grid with the model's
//     vector at its centre.
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

#include "me/GlobalModel.h"

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

constexpr int kW = 640, kH = 360;

// A vector field of 16x16 blocks from model `p`; every block whose index hashes into the first 30%
// gets an unrelated vector instead. `corrupt` records which.
MeFrameResult field(const std::array<double, 6> &p, std::vector<bool> &corrupt)
{
  MeFrameResult r;
  corrupt.clear();
  unsigned state = 12345;
  for (int y = 0; y < kH; y += 16)
    for (int x = 0; x < kW; x += 16)
    {
      const double cx = x + 8, cy = y + 8;
      const double rx = p[0] + p[2] * cx + p[3] * cy, ry = p[1] + p[4] * cx + p[5] * cy;
      MeBlockResult b;
      b.block = {x, y, BlockSize::Blk16};
      b.mv    = {std::int32_t(std::lround((rx - cx) * 8)), std::int32_t(std::lround((ry - cy) * 8))};
      state   = state * 1664525u + 1013904223u;
      const bool bad = (state >> 8) % 100 < 30;
      // A corrupted vector is 10 to 40 pixels off the model on each axis, so there is no doubt which
      // side of the 1.25 px threshold it falls.
      if (bad)
      {
        const int ox = 10 + int((state >> 4) % 31), oy = 10 + int((state >> 12) % 31);
        b.mv.x += ((state >> 20) & 1 ? ox : -ox) * 8;
        b.mv.y += ((state >> 21) & 1 ? oy : -oy) * 8;
      }
      corrupt.push_back(bad);
      r.blocks.push_back(b);
    }
  return r;
}

// The mask is per point, and the bottom row of 16x16 blocks (y = 352 of 360) is not a point: match
// by block position.
bool maskMatches(const GmModelResult &m, const std::vector<bool> &corrupt)
{
  constexpr int cols = kW / 16;
  for (std::size_t i = 0; i < m.blocks.size(); ++i)
  {
    const auto idx = std::size_t(m.blocks[i].y / 16 * cols + m.blocks[i].x / 16);
    if ((m.inlier[i] != 0) == corrupt[idx])
      return false;
  }
  return !m.blocks.empty();
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
} // namespace

int main()
{
  std::vector<bool> corrupt;

  // Translation: content moves so that the reference lies at (+5.25, -3) from the current picture.
  {
    const auto    me = field({5.25, -3, 1, 0, 0, 1}, corrupt);
    GmModelParams params;
    params.type  = GmModelType::Translation;
    const auto m = fitGlobalModel(me, kW, kH, params);
    check(m.ok() && near(m.p[0], 5.25, 0.01) && near(m.p[1], -3, 0.01),
          "translation (+5.25, -3) recovered through 30% outliers (" + std::to_string(m.p[0]) + ", " +
              std::to_string(m.p[1]) + ")");
    check(maskMatches(m, corrupt), "and exactly the corrupted blocks are outliers (" + std::to_string(m.inliers) +
                                       " of " + std::to_string(m.total) + ")");
  }

  // RotZoom about the picture centre: scale 1.02, 1.5 degrees.
  {
    const double s = 1.02, a = 1.5 * M_PI / 180, c = s * std::cos(a), d = s * std::sin(a);
    const double ox = kW / 2.0, oy = kH / 2.0;
    const std::array<double, 6> truth{ox - c * ox - d * oy + 2, oy + d * ox - c * oy - 1, c, d, -d, c};
    const auto    me = field(truth, corrupt);
    GmModelParams params; // RotZoom is the default, as in libaom
    const auto    m = fitGlobalModel(me, kW, kH, params);
    check(m.ok() && near(m.p[2], c, 1e-4) && near(m.p[3], d, 1e-4) && near(m.p[0], truth[0], 0.02) &&
              near(m.p[1], truth[1], 0.02),
          "rotzoom (scale 1.02, 1.5 deg, +2/-1 px) recovered");
    check(near(m.scale(), 1.02, 1e-4) && near(std::fabs(m.angleDegrees()), 1.5, 0.01),
          "scale " + std::to_string(m.scale()) + ", angle " + std::to_string(m.angleDegrees()) + " deg");
    check(maskMatches(m, corrupt), "with the corrupted blocks as outliers");
    check(m.rms < 0.1, "inlier rms below a tenth of a pixel (eighth-pel rounding only)");

    // Determinism.
    const auto again = fitGlobalModel(me, kW, kH, params);
    check(std::memcmp(m.p.data(), again.p.data(), sizeof(double) * 6) == 0 && m.inlier == again.inlier,
          "two runs give bit-identical parameters and masks");
  }

  // Affine: shear the RotZoom cannot express.
  {
    const std::array<double, 6> truth{3, -2, 1.01, 0.03, -0.01, 0.98};
    const auto    me = field(truth, corrupt);
    GmModelParams params;
    params.type  = GmModelType::Affine;
    const auto m = fitGlobalModel(me, kW, kH, params);
    bool       close = m.ok();
    for (int i = 0; i < 6; ++i)
      close = close && near(m.p[std::size_t(i)], truth[std::size_t(i)], i < 2 ? 0.02 : 1e-4);
    check(close, "affine with shear recovered");
    check(maskMatches(m, corrupt), "with the corrupted blocks as outliers");

    params.type        = GmModelType::RotZoom;
    const auto rz      = fitGlobalModel(me, kW, kH, params);
    check(!rz.ok() || rz.inliers < m.inliers, "a rotzoom fits that sheared field worse than the affine");
  }

  // Points that do not count.
  {
    MeFrameResult me;
    for (int x = 0; x < kW; x += 16)
    {
      MeBlockResult b;
      b.block        = {x, 0, BlockSize::Blk16};
      b.staticBypass = x < 320;
      me.blocks.push_back(b);
    }
    MeBlockResult edge;
    edge.block = {kW - 8, 0, BlockSize::Blk16}; // hangs past the right edge
    me.blocks.push_back(edge);
    GmModelParams params;
    params.type  = GmModelType::Translation;
    const auto m = fitGlobalModel(me, kW, kH, params);
    check(m.total == 40, "static-bypass blocks count as (0,0), a block past the edge is not a point (" +
                             std::to_string(m.total) + ")");
    check(m.ok() && m.inliers == 40 && std::fabs(m.p[0]) < 1e-9 && std::fabs(m.p[1]) < 1e-9,
          "so a still frame pair gives the zero model, not an error");

    MeFrameResult few;
    for (int i = 0; i < 9; ++i)
      few.blocks.push_back({{i * 16, 16, BlockSize::Blk16}, {}});
    params.type = GmModelType::RotZoom;
    const auto f = fitGlobalModel(few, kW, kH, params);
    check(!f.ok() && f.error.find("too few points") != std::string::npos,
          "9 points are too few for a rotzoom (needs 2 x 5): " + f.error);
  }

  // Synthesised seed.
  {
    GmModelResult m;
    m.p        = {4, -2, 1, 0, 0, 1};
    const auto seed = synthesiseModelSeed(m, kW, kH);
    // 640 x 384 grid (360 rounded up to 64): 3840 + 960 + 240 + 60 blocks.
    check(seed.blocks.size() == 3840 + 960 + 240 + 60, "the seed covers every block of every size of the SB grid");
    bool all = true;
    for (const auto &b : seed.blocks)
      all = all && b.mv == MotionVector::fromFullPel(4, -2);
    check(all, "each with the model's vector at its centre");
  }

  std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
