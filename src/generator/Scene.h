// Y4M sequence generator: the scene - a background and a stack of layers, each one object (a shape
// or an image patch) with its own motion - and its rendering to 4:2:0 frames.
//
// Qt-free like src/me, so the same scene renders in the generator window, in tests and from a CLI.
// Design: docs/ai/30-designs/y4m-sequence-generator-design.md
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bda::generator
{

struct Yuv8
{
  std::uint8_t y{128}, u{128}, v{128};
};

//!< BT.709 limited range, the convention the ME compare popup draws with (src/me/MeCompareCrop).
Yuv8 rgbToYuv709(int r, int g, int b);

enum class ShapeKind
{
  Rect,
  Circle, //!< An ellipse filling the box.
  Triangle, //!< Apex at the top centre, base along the bottom edge.
  Star,     //!< Five points, the outer points touching the box.
  Image,    //!< An RGBA patch scaled to the box (nearest neighbour).
};

enum class EdgeMode
{
  None,   //!< Keeps going and leaves the picture.
  Wrap,   //!< Leaves on one side, comes back in on the other.
  Bounce, //!< Reflects off the picture edges.
};

/* Motion of one object. Position at frame f is the frame-0 position plus MV(vx, vy) per frame for
 * every frame in [startFrame, endFrame]; before it the object waits, after it the object stays.
 */
struct Motion
{
  double   vx{}, vy{};       //!< Pixels per frame, MV(x, y).
  int      startFrame{0};
  int      endFrame{-1};     //!< -1: until the last frame.
  double   rotationPerFrame{}; //!< Degrees per frame, about the box centre.
  double   scalePerFrame{1.0}; //!< Size multiplier per frame.
  EdgeMode edge{EdgeMode::None};
};

struct Layer
{
  std::string name;
  bool        visible{true};
  ShapeKind   kind{ShapeKind::Rect};
  double      x{}, y{}, w{16}, h{16}; //!< Box at frame 0, pixels (top-left, size).
  double      rotation{};             //!< Degrees at frame 0.
  Yuv8        colour{235, 128, 128};
  std::uint8_t alpha{255};            //!< 255 opaque; an image patch multiplies by its own alpha.
  // Image patch, RGBA8888 rows.
  std::vector<std::uint8_t> rgba;
  int                       patchW{}, patchH{};
  Motion                    motion;
};

struct Scene
{
  int                width{1280}, height{720}, frames{60};
  int                fpsNum{30}, fpsDen{1};
  Yuv8               background{128, 128, 128}; //!< Flat grey: easy to check ME against
  std::vector<Layer> layers; //!< Index 0 is the bottom layer.
};

//!< Where a layer is at a frame: box centre, size and angle after its motion.
struct LayerState
{
  double cx{}, cy{}, w{}, h{}, rotation{};
};
LayerState layerStateAt(const Layer &layer, const Scene &scene, int frame);

struct Frame420
{
  int                       width{}, height{};
  std::vector<std::uint8_t> y, u, v; //!< Planes, no padding; chroma (width/2) x (height/2).
};

/* Frame `frame` of the scene: background, then every visible layer bottom to top, each pixel
 * blended as (a*src + (255-a)*dst + 127) / 255. A pixel belongs to a shape when its centre does.
 * Chroma takes the top-left luma position of each 2x2 block. Width and height must be even.
 */
Frame420 renderFrame(const Scene &scene, int frame);

/* The whole scene as YUV4MPEG2 (4:2:0, 8-bit, C420jpeg). Empty on success, else why not. */
std::string writeY4m(const Scene &scene, const std::string &path);

} // namespace bda::generator
