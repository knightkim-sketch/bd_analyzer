// Unit test: the Y4M sequence generator's scene (src/generator/Scene).
//
// Expectations are written from the geometry and the blend formula, not from the code's output:
//   * a rectangle covers exactly the pixels whose centres fall in its box, chroma included;
//   * an object with MV(3, 2) per frame is at (x + 3f, y + 2f) at frame f and the picture of frame
//     f + 1 is frame f moved by (3, 2) where the object is;
//   * the upper layer wins, swapping the order swaps the result, alpha 128 blends as specified;
//   * wrap brings an object back on the other side, an object waits before its start frame;
//   * a fractional MV keeps its fraction: MV(0.5, 0.25) is MV(2, 1) after four frames;
//   * the written y4m reads back (src/me/YuvLumaReader) with the same size, length and luma.
#include <cstdio>
#include <iostream>
#include <string>

#include "generator/Scene.h"
#include "me/YuvLumaReader.h"

using namespace bda::generator;

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

int lumaAt(const Frame420 &f, int x, int y) { return f.y[std::size_t(y) * f.width + x]; }

Scene smallScene()
{
  Scene s;
  s.width = 64, s.height = 48, s.frames = 6;
  s.background = {40, 128, 128};
  return s;
}

Layer rect(double x, double y, double w, double h, Yuv8 colour)
{
  Layer l;
  l.kind = ShapeKind::Rect;
  l.x = x, l.y = y, l.w = w, l.h = h;
  l.colour = colour;
  return l;
}

} // namespace

int main()
{
  // Exact coverage of a rectangle.
  {
    auto s = smallScene();
    s.layers.push_back(rect(8, 6, 10, 10, {200, 90, 160}));
    const auto f = renderFrame(s, 0);
    check(lumaAt(f, 8, 6) == 200 && lumaAt(f, 17, 15) == 200, "the rectangle's corner pixels are filled");
    check(lumaAt(f, 18, 16) == 40 && lumaAt(f, 7, 5) == 40 && lumaAt(f, 18, 6) == 40,
          "and the pixels just outside are background");
    check(f.u[3 * 32 + 4] == 90 && f.v[3 * 32 + 4] == 160, "chroma inside takes the colour");
    check(f.u[0] == 128, "chroma outside stays background");
  }

  // Linear motion MV(3, 2).
  {
    auto s = smallScene();
    auto l = rect(8, 6, 10, 10, {200, 128, 128});
    l.motion.vx = 3, l.motion.vy = 2;
    s.layers.push_back(l);
    bool placed = true;
    for (int k = 0; k < 5; ++k)
    {
      const auto f = renderFrame(s, k);
      placed       = placed && lumaAt(f, 8 + 3 * k, 6 + 2 * k) == 200 && lumaAt(f, 7 + 3 * k, 6 + 2 * k) == 40 &&
               lumaAt(f, 17 + 3 * k, 15 + 2 * k) == 200 && lumaAt(f, 18 + 3 * k, 15 + 2 * k) == 40;
    }
    check(placed, "the box is at (8 + 3f, 6 + 2f) on frames 0 to 4");
    const auto a = renderFrame(s, 2), b = renderFrame(s, 3);
    bool       shifted = true;
    for (int y = 0; y < 40; ++y)
      for (int x = 0; x < 50; ++x)
        shifted = shifted && lumaAt(b, x + 3, y + 2) == lumaAt(a, x, y);
    check(shifted, "frame 3 is frame 2 moved by MV(3, 2)");
  }

  // Fractional MV(0.5, 0.25): the position keeps the fraction, whole pixels accumulate over frames.
  {
    auto s = smallScene();
    auto l = rect(8, 6, 10, 10, {200, 128, 128});
    l.motion.vx = 0.5, l.motion.vy = 0.25;
    s.layers.push_back(l);
    const auto st0 = layerStateAt(s.layers[0], s, 0), st1 = layerStateAt(s.layers[0], s, 1);
    check(st1.cx - st0.cx == 0.5 && st1.cy - st0.cy == 0.25,
          "MV(0.5, 0.25) moves the centre by exactly (0.5, 0.25) per frame");
    const auto a = renderFrame(s, 0), b = renderFrame(s, 4);
    bool       shifted = true;
    for (int y = 0; y < 40; ++y)
      for (int x = 0; x < 50; ++x)
        shifted = shifted && lumaAt(b, x + 2, y + 1) == lumaAt(a, x, y);
    check(shifted, "frame 4 is frame 0 moved by MV(2, 1) = 4 x MV(0.5, 0.25)");
  }

  // Layer order and alpha.
  {
    auto s = smallScene();
    s.layers.push_back(rect(10, 10, 20, 20, {100, 128, 128}));
    s.layers.push_back(rect(20, 20, 20, 20, {220, 128, 128}));
    check(lumaAt(renderFrame(s, 0), 25, 25) == 220, "the upper layer covers the lower");
    std::swap(s.layers[0], s.layers[1]);
    check(lumaAt(renderFrame(s, 0), 25, 25) == 100, "swapping the order swaps the result");
    s.layers[1].alpha = 128; // the 100-layer, now on top, half transparent over 220
    check(lumaAt(renderFrame(s, 0), 25, 25) == (128 * 100 + 127 * 220 + 127) / 255,
          "alpha 128 blends (128 src + 127 dst + 127) / 255");
    s.layers[1].visible = false;
    check(lumaAt(renderFrame(s, 0), 25, 25) == 220, "a hidden layer is not drawn");
  }

  // Wrap and start frame.
  {
    auto s = smallScene();
    auto l = rect(60, 10, 10, 10, {200, 128, 128});
    l.motion.vx = 2, l.motion.edge = EdgeMode::Wrap;
    s.layers.push_back(l);
    // Frame 3: left edge at 66; the period is 64 + 10, so it is back at 66 - 74 = -8.
    const auto f = renderFrame(s, 3);
    check(lumaAt(f, 0, 10) == 200 && lumaAt(f, 1, 10) == 200 && lumaAt(f, 2, 10) == 40,
          "wrap brings the box back in on the left");

    auto s2 = smallScene();
    auto l2 = rect(8, 6, 4, 4, {200, 128, 128});
    l2.motion.vx = 5, l2.motion.startFrame = 2;
    s2.layers.push_back(l2);
    check(lumaAt(renderFrame(s2, 1), 8, 6) == 200 && lumaAt(renderFrame(s2, 2), 8, 6) == 200 &&
              lumaAt(renderFrame(s2, 3), 13, 6) == 200 && lumaAt(renderFrame(s2, 3), 8, 6) == 40,
          "the object waits until its start frame, then moves");
  }

  // Circle and star.
  {
    auto s = smallScene();
    auto c = rect(0, 0, 32, 32, {200, 128, 128});
    c.kind = ShapeKind::Circle;
    s.layers.push_back(c);
    auto f = renderFrame(s, 0);
    check(lumaAt(f, 16, 16) == 200 && lumaAt(f, 0, 0) == 40 && lumaAt(f, 31, 31) == 40 && lumaAt(f, 16, 1) == 200,
          "circle: centre and top edge in, corners out");
    s.layers[0].kind = ShapeKind::Star;
    f                = renderFrame(s, 0);
    check(lumaAt(f, 16, 16) == 200 && lumaAt(f, 16, 2) == 200 && lumaAt(f, 0, 0) == 40 && lumaAt(f, 16, 30) == 40,
          "star: centre and top point in, corner and bottom middle (between the legs) out");
    s.layers[0].kind = ShapeKind::Triangle;
    f                = renderFrame(s, 0);
    check(lumaAt(f, 16, 2) == 200 && lumaAt(f, 2, 2) == 40 && lumaAt(f, 2, 30) == 200,
          "triangle: apex in, top-left out, bottom-left in");
  }

  // Image patch with its own alpha.
  {
    auto s = smallScene();
    Layer l = rect(4, 4, 2, 2, {});
    l.kind  = ShapeKind::Image;
    l.patchW = 2, l.patchH = 1;
    l.rgba   = {255, 255, 255, 255, 0, 0, 0, 0}; // white opaque, then transparent
    s.layers.push_back(l);
    const auto f = renderFrame(s, 0);
    check(lumaAt(f, 4, 4) == 235 && lumaAt(f, 5, 4) == 40, "image patch: white is Y 235, a transparent pixel shows the background");
  }

  // Y4M round trip.
  {
    auto s = smallScene();
    auto l = rect(8, 6, 10, 10, {200, 128, 128});
    l.motion.vx = 3, l.motion.vy = 2;
    s.layers.push_back(l);
    const std::string path = "/tmp/bd-generator-unit.y4m";
    check(writeY4m(s, path).empty(), "the y4m is written");
    bda::me::YuvLumaReader rd;
    std::string            err;
    const bool             open = rd.openY4m(path, err);
    check(open && rd.width() == 64 && rd.frameCount() == 6, "and reads back as 64 wide, 6 frames (" + err + ")");
    if (open)
    {
      const auto plane = rd.readLuma(4, 0, err);
      const auto f     = renderFrame(s, 4);
      bool       same  = !plane.empty();
      for (int y = 0; y < 48 && same; ++y)
        for (int x = 0; x < 64 && same; ++x)
          same = plane.at(x, y) == lumaAt(f, x, y);
      check(same, "frame 4's luma in the file is the rendered frame 4");
    }
    std::remove(path.c_str());
    s.width = 63;
    check(!writeY4m(s, path).empty(), "an odd width is refused");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
