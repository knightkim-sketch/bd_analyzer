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

/* Camera shake: the whole composited picture (before the flips) is seen through a camera that
 * moves. At frame k the camera state (dx, dy, angle) moves the content forward as
 *   p = c + R(angle) (q - c) + (dx, dy),   c = (width / 2, height / 2),
 * q a scene position, p the picture position, both with pixel centres at +0.5 and R(a) turning +x
 * towards +y. Rendering maps every output pixel centre back through the inverse before the layers'
 * own inverse mapping, so the shaken frame is exact (no resampling of an already rendered frame).
 * For a pure translation, frame k equals the unshaken frame shifted by (dx, dy).
 *
 *   RandomWalk  every frame from 1 on adds a step drawn uniformly from [-amp/2, amp/2] per axis
 *               (angle: [-ampDeg/2, ampDeg/2]) and clamps to [-amp, amp]; frame 0 is at rest.
 *               The draws come from a 32-bit LCG (state * 1103515245 + 12345, the libaom/ANSI C
 *               constants) seeded with `seed`, three draws per frame (x, y, angle) whatever the
 *               amplitudes, so the walk is the same on every platform and compiler.
 *   Sine        dx = ampX sin(2 pi k / period), dy = ampY cos(2 pi k / period) - ampY (so frame 0
 *               is at rest too), angle = ampDeg sin(2 pi k / period).
 */
enum class CameraMode
{
  Off,
  RandomWalk,
  Sine,
};

struct CameraMotion
{
  CameraMode    mode{CameraMode::Off};
  double        ampX{4}, ampY{4}; //!< Pixels
  double        ampDeg{0};        //!< Degrees
  int           period{30};       //!< Frames, Sine only
  std::uint32_t seed{1};          //!< RandomWalk only
};

struct CameraState
{
  double dx{}, dy{}, angle{}; //!< Pixels, pixels, degrees
};

struct Scene
{
  int                width{1280}, height{720}, frames{60};
  int                fpsNum{30}, fpsDen{1};
  Yuv8               background{128, 128, 128}; //!< Flat grey: easy to check ME against
  std::vector<Layer> layers; //!< Index 0 is the bottom layer.
  // Whole-picture mirror of the composited frame, motion included: flipH negates MV x, flipV MV y.
  bool flipH{false}, flipV{false};
  CameraMotion camera; //!< Applied before the flips
};

//!< The camera at frame k in scene (unflipped) coordinates; all zero when the camera is Off.
CameraState cameraAt(const Scene &scene, int frame);

/* The camera as it shows in the written picture, i.e. after the flips: flipH negates dx and the
 * angle, flipV negates dy and the angle. This is the ground truth an estimator should recover.
 */
CameraState cameraInOutput(const Scene &scene, int frame);

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
 * With a camera (scene.camera), each output pixel centre is first taken back through the inverse
 * camera transform. Last, scene.flipH / flipV mirror all three planes: luma (x, y) moves to
 * (w-1-x, y) / (x, h-1-y).
 */
Frame420 renderFrame(const Scene &scene, int frame);

/* The whole scene as YUV4MPEG2 (4:2:0, 8-bit, C420jpeg). Empty on success, else why not. */
std::string writeY4m(const Scene &scene, const std::string &path);

/* The camera trajectory, one row per frame, in output coordinates (cameraInOutput):
 *   frame,dx,dy,angle
 * Empty on success, else why not. The generator writes it next to the y4m as <y4m>.camera.csv.
 */
std::string writeTrajectoryCsv(const Scene &scene, const std::string &path);

} // namespace bda::generator
