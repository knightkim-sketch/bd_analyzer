#include "MePlane.h"

#include <algorithm>
#include <cassert>

namespace bda::me
{

MePlane::MePlane(int width, int height, int pad)
    : width_(std::max(0, width)), height_(std::max(0, height)), pad_(std::max(0, pad))
{
  this->stride_ = this->width_ + 2 * this->pad_;
  this->buf_.assign(static_cast<std::size_t>(this->stride_) *
                        static_cast<std::size_t>(this->height_ + 2 * this->pad_),
                    0);
}

MePlane
MePlane::fromLuma8(const std::uint8_t *src, int width, int height, int srcStride, int pad)
{
  MePlane plane(width, height, pad);
  if (plane.empty() || src == nullptr)
    return plane;

  for (int y = 0; y < height; ++y)
    std::copy_n(src + static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStride),
                static_cast<std::size_t>(width),
                plane.row(y));

  plane.padEdges();
  return plane;
}

MePlane MePlane::fromLuma10AsShifted8(
    const std::uint16_t *src, int width, int height, int srcStride, int pad)
{
  MePlane plane(width, height, pad);
  if (plane.empty() || src == nullptr)
    return plane;

  for (int y = 0; y < height; ++y)
  {
    const auto *in  = src + static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStride);
    auto       *out = plane.row(y);
    for (int x = 0; x < width; ++x)
      out[x] = static_cast<std::uint8_t>(in[x] >> 2); // cvt_10bit_to_8bit_img
  }

  plane.padEdges();
  return plane;
}

void MePlane::padEdges()
{
  if (this->empty() || this->pad_ == 0)
    return;

  // Left and right of every active row.
  for (int y = 0; y < this->height_; ++y)
  {
    const auto left  = this->at(0, y);
    const auto right = this->at(this->width_ - 1, y);
    for (int x = -this->pad_; x < 0; ++x)
      this->at(x, y) = left;
    for (int x = this->width_; x < this->width_ + this->pad_; ++x)
      this->at(x, y) = right;
  }

  // Whole rows above and below, corners included - they were filled by the loop above.
  const auto *top    = &this->at(-this->pad_, 0);
  const auto *bottom = &this->at(-this->pad_, this->height_ - 1);
  for (int y = -this->pad_; y < 0; ++y)
    std::copy_n(top, static_cast<std::size_t>(this->stride_), &this->at(-this->pad_, y));
  for (int y = this->height_; y < this->height_ + this->pad_; ++y)
    std::copy_n(bottom, static_cast<std::size_t>(this->stride_), &this->at(-this->pad_, y));
}

MePlane MePlane::downscaleSvt(int step, int outPad) const
{
  assert(step > 0);
  MePlane out(this->width_ / step, this->height_ / step, outPad);
  if (out.empty())
    return out;

  /* svt_aom_downsample_2d_c. The loop starts at half_decim_step and reads (index - 1, index) on
   * both the previous line and the current one, which is what makes this a phase-shifted average
   * rather than a plain box filter over [0, step). Reading one pixel above and to the left of the
   * start position is why the source needs its border.
   */
  const int halfStep = step >> 1;
  for (int oy = 0; oy < out.height(); ++oy)
  {
    const int   sy   = halfStep + oy * step;
    const auto *prev = &this->at(0, sy - 1);
    const auto *cur  = &this->at(0, sy);
    auto       *dst  = out.row(oy);
    for (int ox = 0; ox < out.width(); ++ox)
    {
      const int sx  = halfStep + ox * step;
      const auto sum = static_cast<std::uint32_t>(prev[sx - 1]) +
                       static_cast<std::uint32_t>(prev[sx]) +
                       static_cast<std::uint32_t>(cur[sx - 1]) + static_cast<std::uint32_t>(cur[sx]);
      dst[ox] = static_cast<std::uint8_t>((sum + 2) >> 2);
    }
  }

  out.padEdges();
  return out;
}

MePlane MePlane::downscaleOdysseyMeanpool(int step, int outPad) const
{
  assert(step > 0);
  MePlane out(this->width_ / step, this->height_ / step, outPad);
  if (out.empty())
    return out;

  /* meanpooling_2d + meanpooling(). Unlike the SVT kernel this starts at (0, 0) and averages the
   * full step x step box.
   *
   * Kept in odyssey's exact form because the rounding is odd: it shifts by `decim_step`, not by
   * log2(step * step). Those happen to be equal only at step 2 (4 samples, >> 2) and step 4
   * (16 samples, >> 4) - at step 8 odyssey would divide 64 samples by 256. The ME path only ever
   * asks for step 2, so this is latent rather than a live bug, but "fixing" it here would make the
   * output stop matching the encoder, which is the one thing this class exists to avoid.
   *
   * Note also that at step 2 this and downscaleSvt() produce identical output - same box, same
   * rounding. They only diverge from step 4 up, where SVT samples the middle 2x2 instead of
   * averaging. tests/unit/me-plane-and-cost.cpp pins both halves of that down.
   */
  const std::uint32_t round = 1u << (step - 1);
  for (int oy = 0; oy < out.height(); ++oy)
  {
    auto *dst = out.row(oy);
    for (int ox = 0; ox < out.width(); ++ox)
    {
      std::uint32_t sum = 0;
      for (int y = 0; y < step; ++y)
      {
        const auto *line = &this->at(ox * step, oy * step + y);
        for (int x = 0; x < step; ++x)
          sum += line[x];
      }
      const auto v = (sum + round) >> step;
      dst[ox]      = static_cast<std::uint8_t>(std::min<std::uint32_t>(v, 255));
    }
  }

  out.padEdges();
  return out;
}

MePlane MePlane::upscaleOdysseyBilinear(int scale, int outPad) const
{
  assert(scale > 0);
  MePlane out(this->width_ * scale, this->height_ * scale, outPad);
  if (out.empty())
    return out;

  /* bilinear_2d + bilinear(). Each source sample expands into a scale x scale patch interpolated
   * against its right/below neighbours, with the neighbour offset dropped at the last row and
   * column (sft_ver / sft_hor go to 0) instead of reaching into the border.
   *
   * The rounding follows odyssey: (wsum + (1 << (scale - 1))) >> scale. For scale = 2 the weights
   * sum to 4 while the shift is 2, so this is odyssey's arithmetic reproduced rather than a
   * normalised bilinear - a "corrected" version would not match the encoder.
   */
  const std::uint32_t round = 1u << (scale - 1);
  for (int sy = 0; sy < this->height_; ++sy)
  {
    const int sftVer = (sy >= this->height_ - 1) ? 0 : 1;
    for (int sx = 0; sx < this->width_; ++sx)
    {
      const int sftHor = (sx >= this->width_ - 1) ? 0 : 1;

      const std::uint32_t p00 = this->at(sx, sy);
      const std::uint32_t p01 = this->at(sx + sftHor, sy);
      const std::uint32_t p10 = this->at(sx, sy + sftVer);
      const std::uint32_t p11 = this->at(sx + sftHor, sy + sftVer);

      for (int y = 0; y < scale; ++y)
        for (int x = 0; x < scale; ++x)
        {
          const std::uint32_t w0   = p00 * (scale - x) + p01 * x;
          const std::uint32_t w1   = p10 * (scale - x) + p11 * x;
          const std::uint32_t wsum = w0 * (scale - y) + w1 * y;
          const auto          v    = (wsum + round) >> scale;
          out.at(sx * scale + x, sy * scale + y) =
              static_cast<std::uint8_t>(std::min<std::uint32_t>(v, 255));
        }
    }
  }

  out.padEdges();
  return out;
}

SvtPyramid buildSvtPyramid(const MePlane &full, int quarterPad, int sixteenthPad)
{
  /* svt_aom_downsample_filtering_input_picture() (pic_analysis_process.c:1498-1546):
   *   quarter   from the padded input with decim_step 2
   *   sixteenth from the *quarter* picture, again with decim_step 2
   * and each one padded before the next stage reads it. The single decim_step 4 call in that
   * function is the branch taken only when HME level 1 is disabled, so it is not what the three
   * level pyramid is made of.
   *
   * Cascading matters: a step 4 downscale of the input is a different picture from two step 2
   * downscales, and both differ from a plain 4x4 mean.
   */
  SvtPyramid pyramid;
  pyramid.quarter   = full.downscaleSvt(2, quarterPad);
  pyramid.sixteenth = pyramid.quarter.downscaleSvt(2, sixteenthPad);
  return pyramid;
}

} // namespace bda::me
