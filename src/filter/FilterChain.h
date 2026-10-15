// A cascade of luma filters: each stage's output is the next stage's input.
//
// Qt-free on purpose, like src/me: the YUV properties pane (through upstream's ConvolutionFilter),
// the motion estimation input and the filtered Y4M export all run the same chain, and the unit test
// drives it without Qt or libYUViewLib.
//
// The stages are the common OpenCV image filters plus two that are not image filters in OpenCV
// terms and are turned into one here:
//   KeypointMap      features2d detectors (ORB / FAST / GFTT) drawn as a point mask
//   GmCompensation   ORB matches + RANSAC global motion (calib3d), the current frame warped onto
//                    the previous one
// Everything that needs OpenCV is compiled only with BDA_HAVE_OPENCV (README design rule 5). Without
// it the catalog holds Custom3x3 alone, which is plain C++.
//
// Samples are uint16 with an explicit bit depth (8..16), so 8- and 10-bit sources go through the
// same code. Stages that OpenCV implements for 8-bit only scale down to 8 bits and back up; their
// output is binary or equalized anyway, so the lost low bits carry nothing.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bda::filter
{

struct LumaPlane
{
  int                        width{};
  int                        height{};
  int                        bitDepth{8};
  std::vector<std::uint16_t> samples; //!< width * height, row major, no border.

  bool valid() const
  {
    return width > 0 && height > 0 && bitDepth >= 8 && bitDepth <= 16 &&
           samples.size() == std::size_t(width) * std::size_t(height);
  }
  int maxValue() const { return (1 << bitDepth) - 1; }
};

// The numeric values are what key() writes, so append new types at the end.
enum class StageType : int
{
  Custom3x3 = 0,
  Gaussian,
  Box,
  Median,
  Bilateral,
  SobelMagnitude,
  ScharrMagnitude,
  Laplacian,
  Canny,
  UnsharpMask,
  Morphology,
  EqualizeHist,
  Clahe,
  Threshold,
  AdaptiveThreshold,
  PyramidDownUp,
  KeypointMap,
  GmCompensation,
};

/* One parameter of a stage. A parameter with choices is an index into them (shown as a combo box);
 * otherwise it is a number in [min, max], an integer when step is 1 and integer is set.
 * Thresholds and colour sigmas are given in 8-bit units and scaled to the source bit depth, so the
 * same numbers mean the same thing for 8- and 10-bit material.
 */
struct ParamInfo
{
  std::string              name;
  double                   min{};
  double                   max{};
  double                   defaultValue{};
  double                   step{1};
  bool                     integer{true};
  std::vector<std::string> choices;
  std::string              tooltip;
};

struct StageInfo
{
  StageType              type;
  std::string            name;
  std::string            group;
  std::vector<ParamInfo> params;
  std::string            tooltip;
};

//!< The stages this build offers, in menu order. Without OpenCV that is Custom3x3 alone.
const std::vector<StageInfo> &stageCatalog();
//!< nullptr for a type this build does not offer.
const StageInfo *stageInfo(StageType type);

struct FilterStage
{
  StageType           type{StageType::Custom3x3};
  std::vector<double> params; //!< One per StageInfo::params, in that order.
  bool                enabled{true};

  static FilterStage withDefaults(StageType type);

  bool operator==(const FilterStage &other) const
  {
    return this->type == other.type && this->params == other.params &&
           this->enabled == other.enabled;
  }
  bool operator!=(const FilterStage &other) const { return !(*this == other); }
};

/* Supplies the previous frame's *unfiltered* luma for a GM compensation stage. Returns false when
 * there is none (frame 0, or it could not be read); the stage then passes its input through.
 */
using PreviousFrameProvider = std::function<bool(LumaPlane &previous)>;

struct FilterChain
{
  std::vector<FilterStage> stages;

  //!< True when applying the chain would not change anything: no stage enabled.
  bool isNoOp() const;

  /* Identifies the chain for the statistics cache. Disabled stages do not take part, so toggling a
   * stage off and on again gets back the same key.
   */
  std::string key() const;

  //!< Index of the enabled GM compensation stage, or -1. A chain holds at most one of them.
  int  gmStageIndex() const;
  bool needsPreviousFrame() const { return this->gmStageIndex() >= 0; }

  /* Run the enabled stages over plane in order. A GM stage compares against the previous frame run
   * through the stages in front of it, so both pictures went through the same filtering before they
   * are matched. previous may be empty when the chain has no GM stage.
   */
  void apply(LumaPlane &plane, const PreviousFrameProvider &previous = {}) const;

  bool operator==(const FilterChain &other) const { return this->stages == other.stages; }
  bool operator!=(const FilterChain &other) const { return !(*this == other); }
};

} // namespace bda::filter
