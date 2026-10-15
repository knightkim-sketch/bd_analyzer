#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace bda::generator
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

std::uint8_t clip8(double v) { return std::uint8_t(std::clamp(std::lround(v), 0L, 255L)); }

// Position along one axis after `travel` pixels, with the edge rule, for an object of size `size`
// in a picture of size `picture`. `start` is the top-left coordinate at frame 0.
double edgePosition(double start, double travel, double size, int picture, EdgeMode edge)
{
  double p = start + travel;
  if (edge == EdgeMode::Wrap)
  {
    // Leaves fully on one side before reappearing on the other: the period is picture + size.
    const double period = picture + size;
    p                   = std::fmod(p + size, period);
    if (p < 0)
      p += period;
    p -= size;
  }
  else if (edge == EdgeMode::Bounce)
  {
    const double span = picture - size;
    if (span <= 0)
      return 0;
    double q = std::fmod(p, 2 * span);
    if (q < 0)
      q += 2 * span;
    p = q <= span ? q : 2 * span - q;
  }
  return p;
}

bool insidePolygon(const double *xs, const double *ys, int n, double x, double y)
{
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++)
    if ((ys[i] > y) != (ys[j] > y) && x < (xs[j] - xs[i]) * (y - ys[i]) / (ys[j] - ys[i]) + xs[i])
      in = !in;
  return in;
}

/* Coverage and colour of a layer at local box coordinates (lx, ly) in [0, w) x [0, h): returns the
 * alpha (0 = not covered) and sets the colour.
 */
int sample(const Layer &layer, double w, double h, double lx, double ly, Yuv8 &colour)
{
  if (lx < 0 || ly < 0 || lx >= w || ly >= h)
    return 0;
  colour = layer.colour;
  switch (layer.kind)
  {
  case ShapeKind::Rect:
    return layer.alpha;
  case ShapeKind::Circle:
  {
    const double nx = (lx - w / 2) / (w / 2), ny = (ly - h / 2) / (h / 2);
    return nx * nx + ny * ny <= 1.0 ? layer.alpha : 0;
  }
  case ShapeKind::Triangle:
  {
    const double xs[3] = {w / 2, w, 0}, ys[3] = {0, h, h};
    return insidePolygon(xs, ys, 3, lx, ly) ? layer.alpha : 0;
  }
  case ShapeKind::Star:
  {
    double xs[10], ys[10];
    for (int k = 0; k < 10; ++k)
    {
      const double r = k % 2 == 0 ? 1.0 : 0.382; // the regular five-point star's inner ratio
      const double a = (-90.0 + k * 36.0) * kPi / 180.0;
      xs[k]          = w / 2 + r * (w / 2) * std::cos(a);
      ys[k]          = h / 2 + r * (h / 2) * std::sin(a);
    }
    return insidePolygon(xs, ys, 10, lx, ly) ? layer.alpha : 0;
  }
  case ShapeKind::Image:
  {
    if (layer.patchW <= 0 || layer.patchH <= 0 ||
        layer.rgba.size() < std::size_t(layer.patchW) * layer.patchH * 4)
      return 0;
    const int  sx = std::min(layer.patchW - 1, int(lx * layer.patchW / w));
    const int  sy = std::min(layer.patchH - 1, int(ly * layer.patchH / h));
    const auto *p = &layer.rgba[(std::size_t(sy) * layer.patchW + sx) * 4];
    colour        = rgbToYuv709(p[0], p[1], p[2]);
    return layer.alpha * p[3] / 255;
  }
  }
  return 0;
}

std::uint8_t blend(int a, std::uint8_t src, std::uint8_t dst)
{
  return std::uint8_t((a * src + (255 - a) * dst + 127) / 255);
}

} // namespace

Yuv8 rgbToYuv709(int r, int g, int b)
{
  // BT.709, limited range: Y in 16..235, Cb/Cr in 16..240.
  const double y = 16 + (0.2126 * r + 0.7152 * g + 0.0722 * b) * 219.0 / 255.0;
  const double u = 128 + (-0.1146 * r - 0.3854 * g + 0.5 * b) * 224.0 / 255.0;
  const double v = 128 + (0.5 * r - 0.4542 * g - 0.0458 * b) * 224.0 / 255.0;
  return {clip8(y), clip8(u), clip8(v)};
}

LayerState layerStateAt(const Layer &layer, const Scene &scene, int frame)
{
  const auto &m    = layer.motion;
  const int   last = m.endFrame < 0 ? scene.frames - 1 : m.endFrame;
  const int   t    = std::max(0, std::min(frame, last) - m.startFrame);

  const double s = std::pow(m.scalePerFrame, t);
  LayerState   st;
  st.w        = layer.w * s;
  st.h        = layer.h * s;
  st.rotation = layer.rotation + m.rotationPerFrame * t;
  // Scaling keeps the box centre; motion moves it.
  const double x0 = layer.x + (layer.w - st.w) / 2, y0 = layer.y + (layer.h - st.h) / 2;
  st.cx = edgePosition(x0, m.vx * t, st.w, scene.width, m.edge) + st.w / 2;
  st.cy = edgePosition(y0, m.vy * t, st.h, scene.height, m.edge) + st.h / 2;
  return st;
}

Frame420 renderFrame(const Scene &scene, int frame)
{
  Frame420 f;
  f.width  = scene.width;
  f.height = scene.height;
  const int cw = scene.width / 2, ch = scene.height / 2;
  f.y.assign(std::size_t(scene.width) * scene.height, scene.background.y);
  f.u.assign(std::size_t(cw) * ch, scene.background.u);
  f.v.assign(std::size_t(cw) * ch, scene.background.v);

  for (const auto &layer : scene.layers)
  {
    if (!layer.visible || layer.w <= 0 || layer.h <= 0)
      continue;
    const auto   st  = layerStateAt(layer, scene, frame);
    const double rad = st.rotation * kPi / 180.0, c = std::cos(rad), s = std::sin(rad);
    // Bounding box of the rotated box.
    const double ex = (std::abs(c) * st.w + std::abs(s) * st.h) / 2;
    const double ey = (std::abs(s) * st.w + std::abs(c) * st.h) / 2;
    const int    x0 = std::max(0, int(std::floor(st.cx - ex))), x1 = std::min(scene.width - 1, int(std::ceil(st.cx + ex)));
    const int    y0 = std::max(0, int(std::floor(st.cy - ey))), y1 = std::min(scene.height - 1, int(std::ceil(st.cy + ey)));

    for (int py = y0; py <= y1; ++py)
      for (int px = x0; px <= x1; ++px)
      {
        // Pixel centre into the layer's box, undoing rotation about the centre.
        const double dx = px + 0.5 - st.cx, dy = py + 0.5 - st.cy;
        const double lx = c * dx + s * dy + st.w / 2;
        const double ly = -s * dx + c * dy + st.h / 2;
        Yuv8         col;
        const int    a = sample(layer, st.w, st.h, lx, ly, col);
        if (a <= 0)
          continue;
        auto &yy = f.y[std::size_t(py) * scene.width + px];
        yy       = blend(a, col.y, yy);
        if ((px & 1) == 0 && (py & 1) == 0 && px / 2 < cw && py / 2 < ch)
        {
          const std::size_t ci = std::size_t(py / 2) * cw + px / 2;
          f.u[ci]              = blend(a, col.u, f.u[ci]);
          f.v[ci]              = blend(a, col.v, f.v[ci]);
        }
      }
  }
  return f;
}

std::string writeY4m(const Scene &scene, const std::string &path)
{
  if (scene.width <= 0 || scene.height <= 0 || scene.width % 2 || scene.height % 2)
    return "width and height must be positive and even for 4:2:0";
  if (scene.frames <= 0)
    return "the scene has no frames";
  std::unique_ptr<FILE, int (*)(FILE *)> file(std::fopen(path.c_str(), "wb"), &std::fclose);
  if (!file)
    return "cannot open " + path + " for writing";
  std::fprintf(file.get(), "YUV4MPEG2 W%d H%d F%d:%d Ip A1:1 C420jpeg\n", scene.width, scene.height,
               scene.fpsNum, scene.fpsDen);
  for (int k = 0; k < scene.frames; ++k)
  {
    const auto f = renderFrame(scene, k);
    std::fputs("FRAME\n", file.get());
    if (std::fwrite(f.y.data(), 1, f.y.size(), file.get()) != f.y.size() ||
        std::fwrite(f.u.data(), 1, f.u.size(), file.get()) != f.u.size() ||
        std::fwrite(f.v.data(), 1, f.v.size(), file.get()) != f.v.size())
      return "write failed (disk full?)";
  }
  return {};
}

} // namespace bda::generator
