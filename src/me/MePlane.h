// An 8-bit luma plane with a replicated border, plus the exact rescalers the two encoders use.
//
// Qt-free by design - see MeTypes.h.
//
// The rescalers live here, one per encoder, because the kernels are not interchangeable in
// general - and because how the pyramid is *built* matters as much as the kernel:
//
//   SVT      svt_aom_downsample_2d_c   phase-shifted by half a step: averages (x-1, x) across the
//                                      previous and current line, rounds (sum + 2) >> 2
//   odyssey  meanpooling_2d            starts at (0, 0), averages the whole step x step box,
//                                      rounds (sum + (1 << (step - 1))) >> step
//
// At step 2 those two produce identical output - same box, same rounding. They diverge from step 4
// up, where the SVT kernel samples only the middle 2x2 and drops the other twelve pixels.
//
// SVT builds its HME pyramid by cascading step 2 (pic_analysis_process.c:1504-1536):
//   quarter   = downsample_2d(full,    step 2)
//   sixteenth = downsample_2d(quarter, step 2)
// with the direct step 4 call reserved for the case where HME level 1 is switched off. So the
// normal path only ever uses step 2, and the thing to get right is the *cascade*: two successive
// 2x2 means are not a 4x4 mean, and neither is the step 4 kernel. Use downscaleSvtPyramid() rather
// than reaching for step 4.
#pragma once

#include <cstdint>
#include <vector>

namespace bda::me
{

/* Padding widths, from odyssey's pad_src_frame():
 *   pad_size = NUM_PAD_PIXELS >> (decim ? 1 : 0),  NUM_PAD_PIXELS = 64
 * so a full-resolution plane gets 64 and a decimated one gets 32. Getting this wrong only shows
 * up at the frame edges, which is exactly where it is easiest not to notice.
 */
inline constexpr int kOdysseyPadFull = 64;
inline constexpr int kOdysseyPadHalf = 32;

class MePlane
{
public:
  MePlane() = default;
  MePlane(int width, int height, int pad);

  int width() const { return this->width_; }
  int height() const { return this->height_; }
  int pad() const { return this->pad_; }
  int stride() const { return this->stride_; }
  bool empty() const { return this->width_ <= 0 || this->height_ <= 0; }

  // Coordinates may run into the border, i.e. -pad <= x < width + pad.
  std::uint8_t       &at(int x, int y) { return this->buf_[this->offset(x, y)]; }
  const std::uint8_t &at(int x, int y) const { return this->buf_[this->offset(x, y)]; }

  std::uint8_t       *row(int y) { return &this->at(0, y); }
  const std::uint8_t *row(int y) const { return &this->at(0, y); }

  /* Copy an 8-bit luma plane in and replicate the edges into the border. */
  static MePlane fromLuma8(const std::uint8_t *src, int width, int height, int srcStride, int pad);

  /* Copy a 10-bit luma plane in, shifted down to 8-bit first.
   *
   * odyssey's open-loop ME does this unconditionally (ody_me_common.c, gated on
   * module_type == OPEN_LOOP && ODS_DEPTH == 10, with dst = src >> 2), so a 10-bit clip is
   * analysed in the 8-bit domain. Reproducing the shift is not optional: skipping it is the
   * reason a 10-bit source would otherwise disagree with the encoder.
   */
  static MePlane
  fromLuma10AsShifted8(const std::uint16_t *src, int width, int height, int srcStride, int pad);

  /* Replicate the edges into the border again. Call after writing into the active area. */
  void padEdges();

  /* SVT-AV1: svt_aom_downsample_2d_c with decim_step = step. Faithful for any step; see the note
   * at the top of this file before choosing one.
   */
  MePlane downscaleSvt(int step, int outPad) const;

  /* odyssey: downscale_2d[MEANPOOL_2D], the default downscaler_type. */
  MePlane downscaleOdysseyMeanpool(int step, int outPad) const;

  /* odyssey: upscale_2d[BILINEAR_2D], the default upscaler_type.
   *
   * This is the half -> "org" step that produces src_half_to_org_frame. The result is full
   * resolution but it is not the original picture: it has been through a downscale and back, and
   * odyssey runs its final VBS search on this, not on the source. Feeding the real source here
   * gives different SSE and different vectors.
   */
  MePlane upscaleOdysseyBilinear(int scale, int outPad) const;

private:
  std::size_t offset(int x, int y) const
  {
    return static_cast<std::size_t>(y + this->pad_) * static_cast<std::size_t>(this->stride_) +
           static_cast<std::size_t>(x + this->pad_);
  }

  int                       width_  = 0;
  int                       height_ = 0;
  int                       pad_    = 0;
  int                       stride_ = 0;
  std::vector<std::uint8_t> buf_;
};

/* The quarter and sixteenth pictures SVT's HME actually runs on.
 *
 * A free function rather than a member because the result holds two MePlanes, and a nested struct
 * cannot contain the class it is nested in.
 */
struct SvtPyramid
{
  MePlane quarter;
  MePlane sixteenth;
};

SvtPyramid buildSvtPyramid(const MePlane &full, int quarterPad, int sixteenthPad);

} // namespace bda::me
