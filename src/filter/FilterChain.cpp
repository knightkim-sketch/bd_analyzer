#include "FilterChain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>

#ifdef BDA_HAVE_OPENCV
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace bda::filter
{

namespace
{

ParamInfo number(const std::string &name, double min, double max, double def, const std::string &tip)
{
  ParamInfo p;
  p.name         = name;
  p.min          = min;
  p.max          = max;
  p.defaultValue = def;
  p.step         = 1;
  p.integer      = true;
  p.tooltip      = tip;
  return p;
}

ParamInfo real(const std::string &name,
               double             min,
               double             max,
               double             def,
               double             step,
               const std::string &tip)
{
  auto p    = number(name, min, max, def, tip);
  p.step    = step;
  p.integer = false;
  return p;
}

#ifdef BDA_HAVE_OPENCV
ParamInfo choice(const std::string &name, std::vector<std::string> choices, int def)
{
  auto p    = number(name, 0, double(choices.size()) - 1, def, {});
  p.choices = std::move(choices);
  return p;
}
#endif

std::vector<StageInfo> buildCatalog()
{
  std::vector<StageInfo> c;

  StageInfo custom{StageType::Custom3x3, "Custom 3x3", "Legacy", {}, {}};
  custom.tooltip = "The 3x3 kernel of the previous version of this pane. The result is divided by "
                   "the sum of the coefficients, unless the sum is zero (edge detectors).";
  for (int i = 0; i < 9; i++)
    custom.params.push_back(real("k" + std::to_string(i / 3) + std::to_string(i % 3),
                                 -1000,
                                 1000,
                                 i == 4 ? 1 : 0,
                                 1,
                                 "Coefficient at (row, col) = (" + std::to_string(i / 3) + ", " +
                                     std::to_string(i % 3) + ")"));
  c.push_back(custom);

#ifdef BDA_HAVE_OPENCV
  c.push_back({StageType::Gaussian,
               "Gaussian blur",
               "Smoothing",
               {number("Kernel size", 1, 31, 5, "Odd; an even value is raised by one"),
                real("Sigma", 0, 20, 0, 0.1, "0 derives sigma from the kernel size")},
               "cv::GaussianBlur"});
  c.push_back({StageType::Box,
               "Box blur",
               "Smoothing",
               {number("Kernel size", 1, 31, 3, "Width and height of the averaging box")},
               "cv::blur (normalized box filter)"});
  c.push_back({StageType::Median,
               "Median blur",
               "Smoothing",
               {number("Kernel size", 3, 9, 3, "Odd. Above 5 a 10-bit source is filtered at 8 bit")},
               "cv::medianBlur"});
  c.push_back({StageType::Bilateral,
               "Bilateral",
               "Smoothing",
               {number("Diameter", 1, 15, 9, "Pixel neighbourhood diameter"),
                real("Sigma colour", 1, 200, 50, 1, "In 8-bit sample units"),
                real("Sigma space", 1, 200, 50, 1, "In pixels")},
               "cv::bilateralFilter: smooths flat areas, keeps edges"});
  c.push_back({StageType::SobelMagnitude,
               "Sobel magnitude",
               "Edges",
               {number("Kernel size", 1, 7, 3, "1, 3, 5 or 7"),
                real("Gain", 0.01, 16, 1, 0.05, "Output = gain * sqrt(gx^2 + gy^2), clipped")},
               "cv::Sobel in x and y, gradient magnitude"});
  c.push_back({StageType::ScharrMagnitude,
               "Scharr magnitude",
               "Edges",
               {real("Gain", 0.01, 16, 0.25, 0.05, "Output = gain * sqrt(gx^2 + gy^2), clipped")},
               "cv::Scharr in x and y, gradient magnitude"});
  c.push_back({StageType::Laplacian,
               "Laplacian",
               "Edges",
               {number("Kernel size", 1, 7, 3, "1, 3, 5 or 7"),
                real("Gain", 0.01, 16, 1, 0.05, "Output = gain * |laplacian|, clipped")},
               "cv::Laplacian, absolute value"});
  c.push_back({StageType::Canny,
               "Canny edges",
               "Edges",
               {real("Low threshold", 0, 1000, 50, 1, "Hysteresis, in 8-bit units"),
                real("High threshold", 0, 1000, 150, 1, "Hysteresis, in 8-bit units")},
               "cv::Canny: binary edge map"});
  c.push_back({StageType::UnsharpMask,
               "Unsharp mask",
               "Sharpen",
               {real("Sigma", 0.1, 20, 2, 0.1, "Gaussian sigma of the blurred copy"),
                real("Amount", 0, 10, 1, 0.1, "Output = src + amount * (src - blurred)")},
               "Sharpening by subtracting a Gaussian blur"});
  c.push_back({StageType::Morphology,
               "Morphology",
               "Morphology",
               {choice("Operation",
                       {"Erode", "Dilate", "Open", "Close", "Gradient", "Top hat", "Black hat"},
                       0),
                choice("Shape", {"Rectangle", "Ellipse", "Cross"}, 0),
                number("Kernel size", 1, 31, 3, "Structuring element width and height"),
                number("Iterations", 1, 10, 1, {})},
               "cv::morphologyEx"});
  c.push_back({StageType::EqualizeHist,
               "Histogram equalize",
               "Contrast",
               {},
               "cv::equalizeHist (computed at 8 bit)"});
  c.push_back({StageType::Clahe,
               "CLAHE",
               "Contrast",
               {real("Clip limit", 0.1, 40, 2, 0.1, {}), number("Tiles", 1, 32, 8, "Tiles per side")},
               "cv::createCLAHE: contrast-limited adaptive histogram equalization"});
  c.push_back({StageType::Threshold,
               "Threshold",
               "Threshold",
               {choice("Mode", {"Binary", "Binary inverted", "Otsu", "Triangle"}, 0),
                real("Threshold", 0, 255, 128, 1, "In 8-bit units; ignored by Otsu and Triangle")},
               "cv::threshold: binary output"});
  c.push_back({StageType::AdaptiveThreshold,
               "Adaptive threshold",
               "Threshold",
               {choice("Method", {"Mean", "Gaussian"}, 0),
                number("Block size", 3, 99, 11, "Odd neighbourhood size"),
                real("C", -50, 50, 2, 1, "Subtracted from the local mean, in 8-bit units")},
               "cv::adaptiveThreshold: binary output (computed at 8 bit)"});
  c.push_back({StageType::PyramidDownUp,
               "Pyramid down-up",
               "Resample",
               {number("Levels", 1, 4, 1, "Halvings before scaling back up")},
               "cv::pyrDown then cv::pyrUp: removes the detail of the finest levels"});
  c.push_back({StageType::KeypointMap,
               "Keypoint map",
               "features2d",
               {choice("Detector", {"ORB", "FAST", "GFTT"}, 0),
                number("Max features", 10, 20000, 500, "The strongest ones are kept"),
                number("FAST threshold", 1, 255, 20, "ORB and FAST, in 8-bit units"),
                number("Radius", 0, 16, 2, "Drawn disc radius in pixels"),
                choice("Background", {"Black", "Picture"}, 0)},
               "Feature points drawn as white discs (detected at 8 bit)"});
  c.push_back({StageType::GmCompensation,
               "GM compensation (RANSAC)",
               "calib3d",
               {choice("Model", {"Similarity", "Affine", "Homography"}, 0),
                number("Max features", 100, 20000, 1000, "ORB features per frame"),
                real("RANSAC threshold", 0.5, 20, 2, 0.5, "Reprojection error in pixels"),
                choice("Output", {"Aligned frame", "|aligned - previous|"}, 0)},
               "ORB matches against the previous frame, global motion by RANSAC "
               "(estimateAffinePartial2D / estimateAffine2D / findHomography), the current frame "
               "warped onto the previous one. Frame 0 passes through. At most one per chain."});
#endif
  return c;
}

double param(const FilterStage &stage, std::size_t i)
{
  if (i < stage.params.size())
    return stage.params[i];
  const auto *info = stageInfo(stage.type);
  return (info && i < info->params.size()) ? info->params[i].defaultValue : 0.0;
}

#ifdef BDA_HAVE_OPENCV
int oddAtLeast(double v, int minimum)
{
  auto k = std::max(minimum, int(std::lround(v)));
  return (k % 2 == 0) ? k + 1 : k;
}
#endif

// Plain C++ so it exists without OpenCV. Same arithmetic as upstream's ConvolutionFilter, which the
// unit test checks sample for sample.
void applyCustom3x3(const FilterStage &stage, LumaPlane &plane)
{
  double kernel[9];
  double sum = 0;
  for (int i = 0; i < 9; i++)
  {
    kernel[i] = param(stage, std::size_t(i));
    sum += kernel[i];
  }
  const double divisor = (std::abs(sum) < 1e-12) ? 1.0 : sum;
  const int    w = plane.width, h = plane.height, maxValue = plane.maxValue();
  const auto   source = plane.samples;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
    {
      double acc = 0;
      for (int ky = -1; ky <= 1; ky++)
      {
        const int sy = std::clamp(y + ky, 0, h - 1);
        for (int kx = -1; kx <= 1; kx++)
        {
          const int sx = std::clamp(x + kx, 0, w - 1);
          acc += kernel[(ky + 1) * 3 + (kx + 1)] * double(source[std::size_t(sy) * w + sx]);
        }
      }
      const auto scaled = std::lround(acc / divisor);
      plane.samples[std::size_t(y) * w + x] =
          std::uint16_t(std::clamp<long>(scaled, 0, long(maxValue)));
    }
}

#ifdef BDA_HAVE_OPENCV

// A view on the plane's samples: writing to it writes the plane.
cv::Mat view(LumaPlane &plane)
{
  return cv::Mat(plane.height, plane.width, CV_16UC1, plane.samples.data());
}

double to8Scale(const LumaPlane &plane) { return 1.0 / double(1 << (plane.bitDepth - 8)); }

cv::Mat to8(LumaPlane &plane)
{
  cv::Mat m;
  view(plane).convertTo(m, CV_8U, to8Scale(plane));
  return m;
}

cv::Mat to32(LumaPlane &plane)
{
  cv::Mat m;
  view(plane).convertTo(m, CV_32F);
  return m;
}

// Write a result back, clipped to the plane's range. 8-bit results are binary masks or equalized
// pictures, so they are stretched over the full range (255 -> maxValue) rather than shifted.
void store(const cv::Mat &result, LumaPlane &plane)
{
  cv::Mat out;
  if (result.depth() == CV_8U)
    result.convertTo(out, CV_16U, double(plane.maxValue()) / 255.0);
  else
    result.convertTo(out, CV_16U); // rounds and saturates at 0 and 65535
  cv::min(out, double(plane.maxValue()), out);
  out.copyTo(view(plane));
}

void applyGmCompensation(const FilterStage &stage, LumaPlane &plane, const LumaPlane *previous)
{
  if (!previous || previous->width != plane.width || previous->height != plane.height ||
      previous->bitDepth != plane.bitDepth)
    return;
  auto prev = *previous;

  const int    model     = int(param(stage, 0));
  const int    features  = int(param(stage, 1));
  const double threshold = param(stage, 2);
  const int    output    = int(param(stage, 3));

  auto                      orb = cv::ORB::create(features);
  std::vector<cv::KeyPoint> kc, kp;
  cv::Mat                   dc, dp;
  orb->detectAndCompute(to8(plane), cv::noArray(), kc, dc);
  orb->detectAndCompute(to8(prev), cv::noArray(), kp, dp);
  if (dc.empty() || dp.empty())
    return;
  std::vector<cv::DMatch> matches;
  cv::BFMatcher(cv::NORM_HAMMING, true).match(dc, dp, matches);
  std::vector<cv::Point2f> from, to;
  for (const auto &m : matches)
  {
    from.push_back(kc[std::size_t(m.queryIdx)].pt);
    to.push_back(kp[std::size_t(m.trainIdx)].pt);
  }

  // Current -> previous, so warping the current frame lines it up with the previous one.
  cv::Mat transform;
  if (model == 2)
  {
    if (from.size() >= 4)
      transform = cv::findHomography(from, to, cv::RANSAC, threshold);
  }
  else if (from.size() >= 3)
    transform = (model == 0) ? cv::estimateAffinePartial2D(from, to, cv::noArray(), cv::RANSAC, threshold)
                             : cv::estimateAffine2D(from, to, cv::noArray(), cv::RANSAC, threshold);
  if (transform.empty())
    // Too few matches for a model: leave the frame as it is rather than invent a motion.
    return;

  cv::Mat aligned;
  const auto size = cv::Size(plane.width, plane.height);
  if (model == 2)
    cv::warpPerspective(view(plane), aligned, transform, size, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
  else
    cv::warpAffine(view(plane), aligned, transform, size, cv::INTER_LINEAR, cv::BORDER_REPLICATE);

  if (output == 1)
    cv::absdiff(aligned, view(prev), aligned);
  store(aligned, plane);
}

void applyKeypointMap(const FilterStage &stage, LumaPlane &plane)
{
  const int detector  = int(param(stage, 0));
  const int maxCount  = int(param(stage, 1));
  const int fast      = int(param(stage, 2));
  const int radius    = int(param(stage, 3));
  const int overPic   = int(param(stage, 4));

  const auto                gray = to8(plane);
  std::vector<cv::KeyPoint> points;
  if (detector == 0)
    cv::ORB::create(maxCount, 1.2f, 8, 31, 0, 2, cv::ORB::HARRIS_SCORE, 31, fast)->detect(gray, points);
  else if (detector == 1)
    cv::FastFeatureDetector::create(fast)->detect(gray, points);
  else
    cv::GFTTDetector::create(maxCount)->detect(gray, points);
  cv::KeyPointsFilter::retainBest(points, maxCount);

  cv::Mat out = overPic ? view(plane).clone() : cv::Mat::zeros(plane.height, plane.width, CV_16UC1);
  for (const auto &p : points)
    cv::circle(out, p.pt, radius, cv::Scalar(plane.maxValue()), cv::FILLED);
  out.copyTo(view(plane));
}

void applyOpenCvStage(const FilterStage &stage, LumaPlane &plane)
{
  const double unit = double(1 << (plane.bitDepth - 8)); // one 8-bit step at this bit depth
  auto         src  = view(plane);

  switch (stage.type)
  {
  case StageType::Gaussian:
  {
    cv::Mat out;
    const int k = oddAtLeast(param(stage, 0), 1);
    cv::GaussianBlur(src, out, cv::Size(k, k), param(stage, 1), 0, cv::BORDER_REPLICATE);
    store(out, plane);
    break;
  }
  case StageType::Box:
  {
    cv::Mat out;
    const int k = std::max(1, int(std::lround(param(stage, 0))));
    cv::blur(src, out, cv::Size(k, k), cv::Point(-1, -1), cv::BORDER_REPLICATE);
    store(out, plane);
    break;
  }
  case StageType::Median:
  {
    const int k = oddAtLeast(param(stage, 0), 3);
    cv::Mat   out;
    if (plane.bitDepth == 8 || k <= 5)
    {
      // medianBlur takes 16-bit input for 3 and 5 only.
      if (plane.bitDepth == 8)
      {
        cv::medianBlur(to8(plane), out, k);
        out.convertTo(out, CV_16U);
      }
      else
        cv::medianBlur(src, out, k);
      store(out, plane);
    }
    else
    {
      cv::medianBlur(to8(plane), out, k);
      out.convertTo(out, CV_16U, unit);
      store(out, plane);
    }
    break;
  }
  case StageType::Bilateral:
  {
    cv::Mat out;
    cv::bilateralFilter(to32(plane),
                        out,
                        std::max(1, int(std::lround(param(stage, 0)))),
                        param(stage, 1) * unit,
                        param(stage, 2),
                        cv::BORDER_REPLICATE);
    store(out, plane);
    break;
  }
  case StageType::SobelMagnitude:
  case StageType::ScharrMagnitude:
  {
    const bool sobel = stage.type == StageType::SobelMagnitude;
    const auto f     = to32(plane);
    cv::Mat    gx, gy, mag;
    if (sobel)
    {
      const int k = std::clamp(oddAtLeast(param(stage, 0), 1), 1, 7);
      cv::Sobel(f, gx, CV_32F, 1, 0, k, 1, 0, cv::BORDER_REPLICATE);
      cv::Sobel(f, gy, CV_32F, 0, 1, k, 1, 0, cv::BORDER_REPLICATE);
    }
    else
    {
      cv::Scharr(f, gx, CV_32F, 1, 0, 1, 0, cv::BORDER_REPLICATE);
      cv::Scharr(f, gy, CV_32F, 0, 1, 1, 0, cv::BORDER_REPLICATE);
    }
    cv::magnitude(gx, gy, mag);
    store(mag * param(stage, sobel ? 1 : 0), plane);
    break;
  }
  case StageType::Laplacian:
  {
    cv::Mat   out;
    const int k = std::clamp(oddAtLeast(param(stage, 0), 1), 1, 7);
    cv::Laplacian(to32(plane), out, CV_32F, k, 1, 0, cv::BORDER_REPLICATE);
    store(cv::abs(out) * param(stage, 1), plane);
    break;
  }
  case StageType::Canny:
  {
    cv::Mat out;
    cv::Canny(to8(plane), out, param(stage, 0), param(stage, 1));
    store(out, plane);
    break;
  }
  case StageType::UnsharpMask:
  {
    const auto f = to32(plane);
    cv::Mat    blurred;
    cv::GaussianBlur(f, blurred, cv::Size(0, 0), param(stage, 0), 0, cv::BORDER_REPLICATE);
    store(f + (f - blurred) * param(stage, 1), plane);
    break;
  }
  case StageType::Morphology:
  {
    static const int ops[]    = {cv::MORPH_ERODE,
                                 cv::MORPH_DILATE,
                                 cv::MORPH_OPEN,
                                 cv::MORPH_CLOSE,
                                 cv::MORPH_GRADIENT,
                                 cv::MORPH_TOPHAT,
                                 cv::MORPH_BLACKHAT};
    static const int shapes[] = {cv::MORPH_RECT, cv::MORPH_ELLIPSE, cv::MORPH_CROSS};
    const int        op       = ops[std::clamp(int(param(stage, 0)), 0, 6)];
    const int        shape    = shapes[std::clamp(int(param(stage, 1)), 0, 2)];
    const int        k        = std::max(1, int(std::lround(param(stage, 2))));
    cv::Mat          out;
    cv::morphologyEx(src,
                     out,
                     op,
                     cv::getStructuringElement(shape, cv::Size(k, k)),
                     cv::Point(-1, -1),
                     std::max(1, int(std::lround(param(stage, 3)))),
                     cv::BORDER_REPLICATE);
    store(out, plane);
    break;
  }
  case StageType::EqualizeHist:
  {
    cv::Mat out;
    cv::equalizeHist(to8(plane), out);
    store(out, plane);
    break;
  }
  case StageType::Clahe:
  {
    const int tiles = std::max(1, int(std::lround(param(stage, 1))));
    cv::Mat   out;
    cv::createCLAHE(param(stage, 0), cv::Size(tiles, tiles))->apply(to8(plane), out);
    store(out, plane);
    break;
  }
  case StageType::Threshold:
  {
    const int mode = std::clamp(int(param(stage, 0)), 0, 3);
    cv::Mat   out;
    if (mode <= 1)
      cv::threshold(to32(plane),
                    out,
                    param(stage, 1) * unit,
                    plane.maxValue(),
                    mode == 0 ? cv::THRESH_BINARY : cv::THRESH_BINARY_INV);
    else
      cv::threshold(
          to8(plane), out, 0, 255, cv::THRESH_BINARY | (mode == 2 ? cv::THRESH_OTSU : cv::THRESH_TRIANGLE));
    store(out, plane);
    break;
  }
  case StageType::AdaptiveThreshold:
  {
    cv::Mat out;
    cv::adaptiveThreshold(to8(plane),
                          out,
                          255,
                          int(param(stage, 0)) == 0 ? cv::ADAPTIVE_THRESH_MEAN_C
                                                     : cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY,
                          oddAtLeast(param(stage, 1), 3),
                          param(stage, 2));
    store(out, plane);
    break;
  }
  case StageType::PyramidDownUp:
  {
    const int             levels = std::clamp(int(std::lround(param(stage, 0))), 1, 4);
    std::vector<cv::Size> sizes{src.size()};
    cv::Mat               cur = src.clone();
    for (int i = 0; i < levels && cur.cols > 1 && cur.rows > 1; i++)
    {
      cv::pyrDown(cur, cur);
      sizes.push_back(cur.size());
    }
    for (std::size_t i = sizes.size() - 1; i > 0; i--)
      cv::pyrUp(cur, cur, sizes[i - 1]);
    store(cur, plane);
    break;
  }
  case StageType::KeypointMap:
    applyKeypointMap(stage, plane);
    break;
  default:
    break;
  }
}

#endif // BDA_HAVE_OPENCV

// One stage that is not a GM compensation. A stage whose parameters do not match its descriptor (a
// chain made by a build with other stages) passes the picture through.
void applyStageUnguarded(const FilterStage &stage, LumaPlane &plane)
{
  const auto *info = stageInfo(stage.type);
  if (!info || stage.params.size() != info->params.size())
    return;
  if (stage.type == StageType::Custom3x3)
  {
    applyCustom3x3(stage, plane);
    return;
  }
#ifdef BDA_HAVE_OPENCV
  applyOpenCvStage(stage, plane);
#endif
}

/* The chain runs on the caching threads, where an escaping exception ends the program. OpenCV
 * throws on input it does not accept (a picture smaller than a detector's border, say), so a stage
 * that throws is skipped: the picture it was given goes on unchanged. The stage writes its result
 * only at the end, so there is no half-written plane to undo.
 */
void applyStage(const FilterStage &stage, LumaPlane &plane)
{
  try
  {
    applyStageUnguarded(stage, plane);
  }
  catch (const std::exception &)
  {
  }
}

} // namespace

const std::vector<StageInfo> &stageCatalog()
{
  static const auto catalog = buildCatalog();
  return catalog;
}

const StageInfo *stageInfo(StageType type)
{
  for (const auto &info : stageCatalog())
    if (info.type == type)
      return &info;
  return nullptr;
}

FilterStage FilterStage::withDefaults(StageType type)
{
  FilterStage stage;
  stage.type = type;
  if (const auto *info = stageInfo(type))
    for (const auto &p : info->params)
      stage.params.push_back(p.defaultValue);
  return stage;
}

bool FilterChain::isNoOp() const
{
  return std::none_of(
      this->stages.begin(), this->stages.end(), [](const FilterStage &s) { return s.enabled; });
}

std::string FilterChain::key() const
{
  if (this->isNoOp())
    return "chain:none";
  std::string key = "chain:";
  char        buffer[40];
  for (const auto &stage : this->stages)
  {
    if (!stage.enabled)
      continue;
    key += std::to_string(int(stage.type)) + "(";
    for (const auto v : stage.params)
    {
      std::snprintf(buffer, sizeof(buffer), "%.17g,", v);
      key += buffer;
    }
    key += ")";
  }
  return key;
}

int FilterChain::gmStageIndex() const
{
  for (std::size_t i = 0; i < this->stages.size(); i++)
    if (this->stages[i].enabled && this->stages[i].type == StageType::GmCompensation)
      return int(i);
  return -1;
}

void FilterChain::apply(LumaPlane &plane, const PreviousFrameProvider &previous) const
{
  if (!plane.valid() || this->isNoOp())
    return;
  const int gm = this->gmStageIndex();
  for (std::size_t i = 0; i < this->stages.size(); i++)
  {
    const auto &stage = this->stages[i];
    if (!stage.enabled)
      continue;
    if (stage.type != StageType::GmCompensation)
    {
      applyStage(stage, plane);
      continue;
    }
    if (int(i) != gm)
      // A second GM stage: the UI does not allow it, and a chain loaded from elsewhere gets the
      // first one only.
      continue;
#ifdef BDA_HAVE_OPENCV
    // The reference goes through exactly the stages in front of this one.
    LumaPlane reference;
    bool      haveReference = previous && previous(reference) && reference.valid();
    if (haveReference)
      for (std::size_t j = 0; j < i; j++)
        if (this->stages[j].enabled)
          applyStage(this->stages[j], reference);
    try
    {
      applyGmCompensation(stage, plane, haveReference ? &reference : nullptr);
    }
    catch (const std::exception &)
    {
      // As in applyStage: a frame the matcher cannot handle passes through.
    }
#else
    (void)previous;
#endif
  }
}

} // namespace bda::filter
