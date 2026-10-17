#include "SequenceGeneratorWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace bda::integration
{
namespace
{

using generator::EdgeMode;
using generator::Layer;
using generator::ShapeKind;
using generator::Yuv8;

const char *kindName(ShapeKind k)
{
  switch (k)
  {
  case ShapeKind::Rect:
    return "Rectangle";
  case ShapeKind::Circle:
    return "Circle";
  case ShapeKind::Triangle:
    return "Triangle";
  case ShapeKind::Star:
    return "Star";
  case ShapeKind::Image:
    return "Image";
  }
  return "?";
}

std::uint8_t clip8(double v) { return std::uint8_t(std::clamp(std::lround(v), 0L, 255L)); }

// BT.709 limited range back to RGB, for the preview and the colour buttons.
QColor yuvToColour(const Yuv8 &c)
{
  const double y = (c.y - 16) * 255.0 / 219.0, u = (c.u - 128) * 255.0 / 224.0, v = (c.v - 128) * 255.0 / 224.0;
  return QColor(clip8(y + 1.5748 * v), clip8(y - 0.1873 * u - 0.4681 * v), clip8(y + 1.8556 * u));
}

QImage frameToImage(const generator::Frame420 &f)
{
  QImage img(f.width, f.height, QImage::Format_RGB888);
  for (int y = 0; y < f.height; ++y)
  {
    auto *row = img.scanLine(y);
    for (int x = 0; x < f.width; ++x)
    {
      const std::size_t ci = std::size_t(y / 2) * (f.width / 2) + x / 2;
      const auto c = yuvToColour({f.y[std::size_t(y) * f.width + x], f.u[ci], f.v[ci]});
      row[x * 3] = std::uint8_t(c.red()), row[x * 3 + 1] = std::uint8_t(c.green()), row[x * 3 + 2] = std::uint8_t(c.blue());
    }
  }
  return img;
}

void setButtonColour(QPushButton *button, const Yuv8 &c)
{
  const auto q = yuvToColour(c);
  button->setText(QString("Y %1  U %2  V %3").arg(c.y).arg(c.u).arg(c.v));
  button->setStyleSheet(QString("background-color: %1; color: %2").arg(q.name(), q.lightness() > 128 ? "black" : "white"));
}

QSpinBox *spin(int lo, int hi, int value)
{
  auto *s = new QSpinBox;
  s->setRange(lo, hi);
  s->setValue(value);
  return s;
}

QDoubleSpinBox *dspin(double lo, double hi, double value, double step, int decimals)
{
  auto *s = new QDoubleSpinBox;
  s->setRange(lo, hi);
  s->setDecimals(decimals);
  s->setSingleStep(step);
  s->setValue(value);
  return s;
}

} // namespace

// ---- canvas ---------------------------------------------------------------------------------

GeneratorCanvas::GeneratorCanvas(generator::Scene &scene, QWidget *parent) : QWidget(parent), scene(scene)
{
  this->setMinimumSize(480, 270);
  this->setMouseTracking(false);
}

void GeneratorCanvas::setFrame(int frame)
{
  this->frameIdx = std::clamp(frame, 0, std::max(0, this->scene.frames - 1));
  this->update();
}

void GeneratorCanvas::setGrid(bool on, int spacing)
{
  this->gridOn      = on;
  this->gridSpacing = std::max(1, spacing);
  this->update();
}

void GeneratorCanvas::setSelected(int layer)
{
  this->selectedLayer = layer;
  this->update();
}

double GeneratorCanvas::scale() const
{
  if (this->scene.width <= 0 || this->scene.height <= 0)
    return 1;
  return std::min(double(this->width()) / this->scene.width, double(this->height()) / this->scene.height);
}

QPointF GeneratorCanvas::origin() const
{
  const double s = this->scale();
  return {(this->width() - this->scene.width * s) / 2, (this->height() - this->scene.height * s) / 2};
}

// Hit-testing and edits work in scene coordinates, so a position on the flipped picture is mirrored
// back first; drags then follow the mouse on screen.
QPointF GeneratorCanvas::toScene(const QPointF &p) const
{
  return this->cameraInverse(this->mirror((p - this->origin()) / this->scale()));
}

// The forward camera of generator::CameraMotion: p = c + R(a) (q - c) + d.
QPointF GeneratorCanvas::cameraForward(const QPointF &q) const
{
  const auto cam = generator::cameraAt(this->scene, this->frameIdx);
  const double r = cam.angle * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
  const double cx = this->scene.width / 2.0, cy = this->scene.height / 2.0, ux = q.x() - cx, uy = q.y() - cy;
  return {cx + c * ux - s * uy + cam.dx, cy + s * ux + c * uy + cam.dy};
}

QPointF GeneratorCanvas::cameraInverse(const QPointF &p) const
{
  const auto cam = generator::cameraAt(this->scene, this->frameIdx);
  const double r = cam.angle * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
  const double cx = this->scene.width / 2.0, cy = this->scene.height / 2.0;
  const double ux = p.x() - cx - cam.dx, uy = p.y() - cy - cam.dy;
  return {cx + c * ux + s * uy, cy - s * ux + c * uy};
}

QPointF GeneratorCanvas::mirror(const QPointF &p) const
{
  return {this->scene.flipH ? this->scene.width - p.x() : p.x(), this->scene.flipV ? this->scene.height - p.y() : p.y()};
}

QRectF GeneratorCanvas::layerBox(int layer) const
{
  const auto st = generator::layerStateAt(this->scene.layers[std::size_t(layer)], this->scene, this->frameIdx);
  return {st.cx - st.w / 2, st.cy - st.h / 2, st.w, st.h};
}

int GeneratorCanvas::layerAt(const QPointF &p) const
{
  for (int i = int(this->scene.layers.size()) - 1; i >= 0; --i) // topmost first
    if (this->scene.layers[std::size_t(i)].visible && this->layerBox(i).contains(p))
      return i;
  return -1;
}

void GeneratorCanvas::paintEvent(QPaintEvent *)
{
  QPainter painter(this);
  painter.fillRect(this->rect(), this->palette().color(QPalette::Dark));
  if (this->scene.width <= 0 || this->scene.height <= 0 || this->scene.width % 2 || this->scene.height % 2)
    return;
  const double s = this->scale();
  const auto   o = this->origin();
  painter.drawImage(QRectF(o, QSizeF(this->scene.width * s, this->scene.height * s)),
                    frameToImage(generator::renderFrame(this->scene, this->frameIdx)));

  // Grid in picture coordinates (x = k * spacing from the picture origin): drawn here only.
  if (this->gridOn)
  {
    painter.setPen(QPen(QColor(255, 255, 255, 70), 0));
    for (int gx = this->gridSpacing; gx < this->scene.width; gx += this->gridSpacing)
      painter.drawLine(QPointF(o.x() + gx * s, o.y()), QPointF(o.x() + gx * s, o.y() + this->scene.height * s));
    for (int gy = this->gridSpacing; gy < this->scene.height; gy += this->gridSpacing)
      painter.drawLine(QPointF(o.x(), o.y() + gy * s), QPointF(o.x() + this->scene.width * s, o.y() + gy * s));
  }

  if (this->selectedLayer >= 0 && this->selectedLayer < int(this->scene.layers.size()))
  {
    // The box and its resize corner where they appear on the (possibly shaken and flipped)
    // picture; under a rotating camera the box is drawn as the quadrilateral it becomes.
    const auto box    = this->layerBox(this->selectedLayer);
    const auto toView = [&](const QPointF &q) { return o + this->mirror(this->cameraForward(q)) * s; };
    const QPolygonF quad{toView(box.topLeft()), toView(box.topRight()), toView(box.bottomRight()), toView(box.bottomLeft())};
    const QRectF  wbox   = quad.boundingRect();
    const QPointF handle = toView(box.bottomRight());
    painter.setPen(QPen(Qt::yellow, 1, Qt::DashLine));
    painter.drawPolygon(quad);
    painter.fillRect(QRectF(handle - QPointF(4, 4), QSizeF(8, 8)), Qt::yellow); // resize handle
    // Where the object is one frame later: its motion vector as seen on the picture, MV(x, y).
    const auto &m  = this->scene.layers[std::size_t(this->selectedLayer)].motion;
    const double mx = this->scene.flipH ? 0.0 - m.vx : m.vx, my = this->scene.flipV ? 0.0 - m.vy : m.vy; // 0.0 - v: no "-0"
    if (mx != 0 || my != 0)
    {
      const QPointF c = wbox.center(), d = c + QPointF(mx, my) * s * 4;
      painter.setPen(QPen(Qt::cyan, 2));
      painter.drawLine(c, d);
      painter.drawText(d + QPointF(4, -4), QString("MV(%1, %2)/frame").arg(mx).arg(my));
    }
  }
}

void GeneratorCanvas::mousePressEvent(QMouseEvent *event)
{
  if (event->button() != Qt::LeftButton)
    return;
  const auto p      = this->toScene(event->position());
  this->pressScene  = this->lastScene = p;
  const auto keepBox = [this](int i) {
    const auto &l  = this->scene.layers[std::size_t(i)];
    this->pressBox = QRectF(l.x, l.y, l.w, l.h);
  };
  if (this->selectedLayer >= 0 && this->selectedLayer < int(this->scene.layers.size()))
  {
    const auto box = this->layerBox(this->selectedLayer);
    if (QLineF(p, box.bottomRight()).length() * this->scale() <= 8)
    {
      keepBox(this->selectedLayer);
      this->drag = Drag::Resize;
      return;
    }
  }
  const int hit = this->layerAt(p);
  if (hit >= 0)
  {
    this->drag = Drag::Move;
    keepBox(hit);
    if (hit != this->selectedLayer)
    {
      this->setSelected(hit);
      emit selectionChanged(hit);
    }
    return;
  }
  this->drag = Drag::Create;
}

void GeneratorCanvas::mouseMoveEvent(QMouseEvent *event)
{
  const auto p = this->toScene(event->position());
  if ((this->drag == Drag::Move || this->drag == Drag::Resize) && this->selectedLayer >= 0)
  {
    auto      &l = this->scene.layers[std::size_t(this->selectedLayer)];
    const auto d = p - this->pressScene;
    // Edits change the frame-0 box, in whole pixels; the motion then carries it.
    if (this->drag == Drag::Move)
      l.x = std::round(this->pressBox.x() + d.x()), l.y = std::round(this->pressBox.y() + d.y());
    else
      l.w = std::max(2.0, std::round(this->pressBox.width() + d.x())),
      l.h = std::max(2.0, std::round(this->pressBox.height() + d.y()));
    this->update();
    emit sceneEdited();
  }
  else if (this->drag == Drag::Create)
  {
    this->lastScene = p;
    this->update();
  }
}

void GeneratorCanvas::mouseReleaseEvent(QMouseEvent *event)
{
  if (this->drag == Drag::Create)
  {
    const auto   p = this->toScene(event->position());
    const QRectF r = QRectF(this->pressScene, p).normalized();
    if (r.width() >= 4 && r.height() >= 4)
    {
      Layer l;
      l.kind = this->createKind;
      l.name = std::string(kindName(this->createKind)) + " " + std::to_string(this->scene.layers.size() + 1);
      l.x = std::round(r.x()), l.y = std::round(r.y()), l.w = std::round(r.width()), l.h = std::round(r.height());
      // A distinct colour per layer so overlaps read at a glance.
      static const Yuv8 palette[] = {{180, 60, 200}, {150, 200, 70}, {200, 80, 120}, {120, 170, 190}, {210, 128, 128}};
      l.colour = palette[this->scene.layers.size() % 5];
      this->scene.layers.push_back(l);
      this->setSelected(int(this->scene.layers.size()) - 1);
      emit sceneEdited();
      emit selectionChanged(this->selectedLayer);
    }
  }
  this->drag = Drag::None;
  this->update();
}

// ---- window ---------------------------------------------------------------------------------

SequenceGeneratorWindow::SequenceGeneratorWindow(QWidget *parent) : QDialog(parent)
{
  this->setWindowTitle("Y4M sequence generator");
  this->resize(1400, 820);

  auto *root = new QHBoxLayout(this);
  auto *left = new QVBoxLayout();
  this->canvas = new GeneratorCanvas(this->sceneData, this);
  left->addWidget(this->canvas, 1);

  auto *preview = new QHBoxLayout();
  this->playButton  = new QPushButton("Play", this);
  this->frameSlider = new QSlider(Qt::Horizontal, this);
  this->frameLabel  = new QLabel(this);
  preview->addWidget(this->playButton);
  preview->addWidget(this->frameSlider, 1);
  preview->addWidget(this->frameLabel);
  this->gridCheck   = new QCheckBox("Grid", this);
  this->gridSpacing = new QComboBox(this);
  for (int g : {8, 16, 32, 64, 128})
    this->gridSpacing->addItem(QString::number(g), g);
  this->gridCheck->setToolTip("Preview only: lines on block boundaries, not written to the y4m.");
  {
    QSettings settings;
    this->gridCheck->setChecked(settings.value("SequenceGenerator/grid", false).toBool());
    const int i = this->gridSpacing->findData(settings.value("SequenceGenerator/gridSpacing", 64).toInt());
    this->gridSpacing->setCurrentIndex(i >= 0 ? i : this->gridSpacing->findData(64));
  }
  this->canvas->setGrid(this->gridCheck->isChecked(), this->gridSpacing->currentData().toInt());
  preview->addWidget(this->gridCheck);
  preview->addWidget(this->gridSpacing);
  left->addLayout(preview);
  this->status = new QLabel("Drag on the canvas to draw the selected shape. Drag a shape to move it, "
                            "its yellow corner to resize it.", this);
  this->status->setWordWrap(true);
  left->addWidget(this->status);
  root->addLayout(left, 1);

  auto *right = new QVBoxLayout();
  root->addLayout(right);

  // Sequence
  auto *seqBox  = new QGroupBox("Sequence", this);
  auto *seqForm = new QFormLayout(seqBox);
  this->width   = spin(16, 7680, this->sceneData.width);
  this->height  = spin(16, 4320, this->sceneData.height);
  this->frames  = spin(1, 10000, this->sceneData.frames);
  this->fps     = spin(1, 240, this->sceneData.fpsNum);
  this->width->setSingleStep(2), this->height->setSingleStep(2);
  this->backgroundButton = new QPushButton(this);
  auto *size = new QHBoxLayout();
  size->addWidget(this->width), size->addWidget(new QLabel("x", this)), size->addWidget(this->height);
  seqForm->addRow("Size (even)", size);
  seqForm->addRow("Frames", this->frames);
  seqForm->addRow("fps", this->fps);
  seqForm->addRow("Background", this->backgroundButton);
  this->flipH = new QCheckBox("Flip horizontal", this);
  this->flipV = new QCheckBox("Flip vertical", this);
  for (auto *c : {this->flipH, this->flipV})
    c->setToolTip("Mirrors the whole picture, in the preview and the y4m. Motion is mirrored too: "
                  "a horizontal flip negates MV x, a vertical flip MV y.");
  auto *flip = new QHBoxLayout();
  flip->addWidget(this->flipH), flip->addWidget(this->flipV);
  seqForm->addRow("Flip", flip);
  // Camera shake: the whole picture moves under a camera, before the flips (generator::CameraMotion).
  this->cameraMode = new QComboBox(this);
  this->cameraMode->setObjectName("cameraMode");
  this->cameraMode->addItems({"Off", "Random walk", "Sine"});
  this->cameraMode->setToolTip("Moves the whole picture as a shaking camera would. Generate also writes "
                               "the camera trajectory (as seen after the flips) to <y4m>.camera.csv.");
  this->cameraAmpX   = dspin(0, 256, this->sceneData.camera.ampX, 0.5, 2);
  this->cameraAmpY   = dspin(0, 256, this->sceneData.camera.ampY, 0.5, 2);
  this->cameraAmpDeg = dspin(0, 45, this->sceneData.camera.ampDeg, 0.1, 2);
  this->cameraPeriod = spin(1, 10000, this->sceneData.camera.period);
  this->cameraSeed   = spin(0, 2147483647, int(this->sceneData.camera.seed));
  this->cameraAmpX->setObjectName("cameraAmpX"), this->cameraAmpY->setObjectName("cameraAmpY");
  this->cameraAmpDeg->setObjectName("cameraAmpDeg"), this->cameraPeriod->setObjectName("cameraPeriod");
  this->cameraSeed->setObjectName("cameraSeed");
  auto *amp = new QHBoxLayout();
  amp->addWidget(this->cameraAmpX), amp->addWidget(this->cameraAmpY), amp->addWidget(this->cameraAmpDeg);
  auto *camMisc = new QHBoxLayout();
  camMisc->addWidget(new QLabel("period", this)), camMisc->addWidget(this->cameraPeriod);
  camMisc->addWidget(new QLabel("seed", this)), camMisc->addWidget(this->cameraSeed);
  seqForm->addRow("Camera shake", this->cameraMode);
  seqForm->addRow("Amplitude x, y px, deg", amp);
  seqForm->addRow("", camMisc);
  right->addWidget(seqBox);

  // Layers
  auto *layerBox = new QGroupBox("Layers (top of the list is drawn last, on top)", this);
  auto *lv       = new QVBoxLayout(layerBox);
  auto *addRow   = new QHBoxLayout();
  this->shapeKind = new QComboBox(this);
  for (auto k : {ShapeKind::Rect, ShapeKind::Circle, ShapeKind::Triangle, ShapeKind::Star})
    this->shapeKind->addItem(kindName(k), int(k));
  auto *addButton   = new QPushButton("Add", this);
  auto *imageButton = new QPushButton("Image...", this);
  auto *pasteButton = new QPushButton("Paste", this);
  addRow->addWidget(this->shapeKind), addRow->addWidget(addButton), addRow->addWidget(imageButton), addRow->addWidget(pasteButton);
  lv->addLayout(addRow);
  this->layers = new QListWidget(this);
  lv->addWidget(this->layers);
  auto *order   = new QHBoxLayout();
  auto *up      = new QPushButton("Up", this);
  auto *down    = new QPushButton("Down", this);
  auto *remove  = new QPushButton("Delete", this);
  order->addWidget(up), order->addWidget(down), order->addWidget(remove);
  lv->addLayout(order);
  right->addWidget(layerBox);

  // Properties of the selected layer, at frame 0
  auto *propBox  = new QGroupBox("Object (frame 0)", this);
  auto *propForm = new QFormLayout(propBox);
  this->x = spin(-8192, 8192, 0), this->y = spin(-8192, 8192, 0);
  this->w = spin(1, 8192, 16), this->h = spin(1, 8192, 16);
  this->rotation     = dspin(-360, 360, 0, 5, 1);
  this->alpha        = spin(0, 255, 255);
  this->colourButton = new QPushButton(this);
  auto *pos = new QHBoxLayout();
  pos->addWidget(this->x), pos->addWidget(this->y);
  auto *dim = new QHBoxLayout();
  dim->addWidget(this->w), dim->addWidget(this->h);
  propForm->addRow("Position x, y", pos);
  propForm->addRow("Size w, h", dim);
  propForm->addRow("Rotation (deg)", this->rotation);
  propForm->addRow("Colour", this->colourButton);
  propForm->addRow("Alpha", this->alpha);
  right->addWidget(propBox);

  // Motion
  auto *motionBox  = new QGroupBox("Motion", this);
  auto *motionForm = new QFormLayout(motionBox);
  this->vx = dspin(-512, 512, 0, 0.25, 3), this->vy = dspin(-512, 512, 0, 0.25, 3);
  this->startFrame = spin(0, 10000, 0), this->endFrame = spin(-1, 10000, -1);
  this->endFrame->setSpecialValueText("last");
  this->edge = new QComboBox(this);
  this->edge->addItems({"Leave the picture", "Wrap around", "Bounce"});
  this->rotationPerFrame = dspin(-180, 180, 0, 1, 2);
  this->scalePerFrame    = dspin(0.5, 2.0, 1.0, 0.01, 3);
  auto *mv = new QHBoxLayout();
  mv->addWidget(this->vx), mv->addWidget(this->vy);
  auto *range = new QHBoxLayout();
  range->addWidget(this->startFrame), range->addWidget(new QLabel("to", this)), range->addWidget(this->endFrame);
  motionForm->addRow("MV(x, y) px/frame", mv);
  motionForm->addRow("Frames", range);
  motionForm->addRow("At the edge", this->edge);
  motionForm->addRow("Rotation/frame", this->rotationPerFrame);
  motionForm->addRow("Scale/frame", this->scalePerFrame);
  right->addWidget(motionBox);

  auto *actions  = new QHBoxLayout();
  auto *save     = new QPushButton("Save scene...", this);
  auto *load     = new QPushButton("Load scene...", this);
  auto *generate = new QPushButton("Generate Y4M...", this);
  generate->setDefault(true);
  actions->addWidget(save), actions->addWidget(load), actions->addWidget(generate);
  right->addLayout(actions);
  right->addStretch();

  connect(this->canvas, &GeneratorCanvas::selectionChanged, this, [this](int l) {
    this->updating = true;
    this->layers->setCurrentRow(l);
    this->updating = false;
    this->refreshProperties();
  });
  connect(this->canvas, &GeneratorCanvas::sceneEdited, this, [this] {
    this->refreshLayerList();
    this->refreshProperties();
  });
  connect(this->shapeKind, &QComboBox::currentIndexChanged, this,
          [this] { this->canvas->setCreateKind(ShapeKind(this->shapeKind->currentData().toInt())); });
  connect(addButton, &QPushButton::clicked, this, &SequenceGeneratorWindow::addShape);
  connect(imageButton, &QPushButton::clicked, this, &SequenceGeneratorWindow::addImageFromFile);
  connect(pasteButton, &QPushButton::clicked, this, &SequenceGeneratorWindow::addImageFromClipboard);
  connect(up, &QPushButton::clicked, this, [this] { this->moveLayer(+1); });
  connect(down, &QPushButton::clicked, this, [this] { this->moveLayer(-1); });
  connect(remove, &QPushButton::clicked, this, &SequenceGeneratorWindow::deleteLayer);
  connect(this->layers, &QListWidget::currentRowChanged, this, &SequenceGeneratorWindow::layerRowChanged);
  connect(this->layers, &QListWidget::itemChanged, this, &SequenceGeneratorWindow::layerItemChanged);
  for (auto *s : {this->x, this->y, this->w, this->h, this->alpha, this->startFrame, this->endFrame})
    connect(s, &QSpinBox::valueChanged, this, &SequenceGeneratorWindow::propertiesEdited);
  for (auto *s : {this->rotation, this->vx, this->vy, this->rotationPerFrame, this->scalePerFrame})
    connect(s, &QDoubleSpinBox::valueChanged, this, &SequenceGeneratorWindow::propertiesEdited);
  connect(this->edge, &QComboBox::currentIndexChanged, this, &SequenceGeneratorWindow::propertiesEdited);
  connect(this->colourButton, &QPushButton::clicked, this, [this] { this->pickColour(false); });
  connect(this->backgroundButton, &QPushButton::clicked, this, [this] { this->pickColour(true); });
  for (auto *s : {this->width, this->height, this->frames, this->fps})
    connect(s, &QSpinBox::valueChanged, this, &SequenceGeneratorWindow::sequenceEdited);
  for (auto *c : {this->flipH, this->flipV})
    connect(c, &QCheckBox::toggled, this, &SequenceGeneratorWindow::sequenceEdited);
  connect(this->cameraMode, &QComboBox::currentIndexChanged, this, &SequenceGeneratorWindow::sequenceEdited);
  for (auto *d : {this->cameraAmpX, this->cameraAmpY, this->cameraAmpDeg})
    connect(d, &QDoubleSpinBox::valueChanged, this, &SequenceGeneratorWindow::sequenceEdited);
  for (auto *sp : {this->cameraPeriod, this->cameraSeed})
    connect(sp, &QSpinBox::valueChanged, this, &SequenceGeneratorWindow::sequenceEdited);
  const auto gridEdited = [this] {
    const int g = this->gridSpacing->currentData().toInt();
    this->canvas->setGrid(this->gridCheck->isChecked(), g);
    QSettings settings;
    settings.setValue("SequenceGenerator/grid", this->gridCheck->isChecked());
    settings.setValue("SequenceGenerator/gridSpacing", g);
  };
  connect(this->gridCheck, &QCheckBox::toggled, this, gridEdited);
  connect(this->gridSpacing, &QComboBox::currentIndexChanged, this, gridEdited);
  connect(this->frameSlider, &QSlider::valueChanged, this, &SequenceGeneratorWindow::frameChanged);
  connect(this->playButton, &QPushButton::clicked, this, &SequenceGeneratorWindow::togglePlay);
  connect(&this->playTimer, &QTimer::timeout, this, [this] {
    const int n = std::max(1, this->sceneData.frames);
    this->frameSlider->setValue((this->frameSlider->value() + 1) % n);
  });
  connect(generate, &QPushButton::clicked, this, &SequenceGeneratorWindow::generateClicked);
  connect(save, &QPushButton::clicked, this, &SequenceGeneratorWindow::saveClicked);
  connect(load, &QPushButton::clicked, this, &SequenceGeneratorWindow::loadClicked);

  this->sceneChanged();
}

void SequenceGeneratorWindow::sceneChanged()
{
  this->updating = true;
  this->width->setValue(this->sceneData.width);
  this->height->setValue(this->sceneData.height);
  this->frames->setValue(this->sceneData.frames);
  this->fps->setValue(this->sceneData.fpsNum);
  setButtonColour(this->backgroundButton, this->sceneData.background);
  this->flipH->setChecked(this->sceneData.flipH);
  this->flipV->setChecked(this->sceneData.flipV);
  this->cameraMode->setCurrentIndex(int(this->sceneData.camera.mode));
  this->cameraAmpX->setValue(this->sceneData.camera.ampX);
  this->cameraAmpY->setValue(this->sceneData.camera.ampY);
  this->cameraAmpDeg->setValue(this->sceneData.camera.ampDeg);
  this->cameraPeriod->setValue(this->sceneData.camera.period);
  this->cameraSeed->setValue(int(this->sceneData.camera.seed));
  this->frameSlider->setRange(0, std::max(0, this->sceneData.frames - 1));
  this->updating = false;
  this->refreshLayerList();
  this->refreshProperties();
  this->frameChanged(this->frameSlider->value());
}

void SequenceGeneratorWindow::refreshLayerList()
{
  this->updating = true;
  const int keep = this->canvas->selected();
  this->layers->clear();
  // Shown top layer first, so the list reads like a stack.
  for (int i = int(this->sceneData.layers.size()) - 1; i >= 0; --i)
  {
    const auto &l    = this->sceneData.layers[std::size_t(i)];
    auto       *item = new QListWidgetItem(QString::fromStdString(l.name.empty() ? kindName(l.kind) : l.name));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(l.visible ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, i);
    this->layers->addItem(item);
  }
  if (keep >= 0 && keep < int(this->sceneData.layers.size()))
    this->layers->setCurrentRow(int(this->sceneData.layers.size()) - 1 - keep);
  this->updating = false;
}

void SequenceGeneratorWindow::refreshProperties()
{
  const int i = this->canvas->selected();
  const bool have = i >= 0 && i < int(this->sceneData.layers.size());
  for (QWidget *wdg : std::initializer_list<QWidget *>{this->x, this->y, this->w, this->h, this->rotation, this->colourButton,
                                                       this->alpha, this->vx, this->vy, this->startFrame, this->endFrame,
                                                       this->edge, this->rotationPerFrame, this->scalePerFrame})
    wdg->setEnabled(have);
  if (!have)
    return;
  const auto &l = this->sceneData.layers[std::size_t(i)];
  this->updating = true;
  this->x->setValue(int(std::lround(l.x))), this->y->setValue(int(std::lround(l.y)));
  this->w->setValue(int(std::lround(l.w))), this->h->setValue(int(std::lround(l.h)));
  this->rotation->setValue(l.rotation);
  this->alpha->setValue(l.alpha);
  setButtonColour(this->colourButton, l.colour);
  this->colourButton->setEnabled(l.kind != ShapeKind::Image);
  this->vx->setValue(l.motion.vx), this->vy->setValue(l.motion.vy);
  this->startFrame->setValue(l.motion.startFrame), this->endFrame->setValue(l.motion.endFrame);
  this->edge->setCurrentIndex(int(l.motion.edge));
  this->rotationPerFrame->setValue(l.motion.rotationPerFrame);
  this->scalePerFrame->setValue(l.motion.scalePerFrame);
  this->updating = false;
}

void SequenceGeneratorWindow::propertiesEdited()
{
  const int i = this->canvas->selected();
  if (this->updating || i < 0 || i >= int(this->sceneData.layers.size()))
    return;
  auto &l             = this->sceneData.layers[std::size_t(i)];
  l.x = this->x->value(), l.y = this->y->value(), l.w = this->w->value(), l.h = this->h->value();
  l.rotation          = this->rotation->value();
  l.alpha             = std::uint8_t(this->alpha->value());
  l.motion.vx = this->vx->value(), l.motion.vy = this->vy->value();
  l.motion.startFrame = this->startFrame->value(), l.motion.endFrame = this->endFrame->value();
  l.motion.edge       = EdgeMode(this->edge->currentIndex());
  l.motion.rotationPerFrame = this->rotationPerFrame->value();
  l.motion.scalePerFrame    = this->scalePerFrame->value();
  this->canvas->update();
}

void SequenceGeneratorWindow::sequenceEdited()
{
  if (this->updating)
    return;
  this->sceneData.width  = this->width->value() & ~1; // 4:2:0 needs even sizes
  this->sceneData.height = this->height->value() & ~1;
  this->sceneData.frames = this->frames->value();
  this->sceneData.fpsNum = this->fps->value();
  this->sceneData.flipH  = this->flipH->isChecked();
  this->sceneData.flipV  = this->flipV->isChecked();
  auto &cam              = this->sceneData.camera;
  cam.mode               = generator::CameraMode(this->cameraMode->currentIndex());
  cam.ampX               = this->cameraAmpX->value();
  cam.ampY               = this->cameraAmpY->value();
  cam.ampDeg             = this->cameraAmpDeg->value();
  cam.period             = this->cameraPeriod->value();
  cam.seed               = std::uint32_t(this->cameraSeed->value());
  this->frameSlider->setRange(0, std::max(0, this->sceneData.frames - 1));
  this->canvas->setFrame(this->frameSlider->value());
}

void SequenceGeneratorWindow::frameChanged(int frame)
{
  this->canvas->setFrame(frame);
  this->frameLabel->setText(QString("frame %1 / %2").arg(frame).arg(std::max(0, this->sceneData.frames - 1)));
}

void SequenceGeneratorWindow::togglePlay()
{
  if (this->playTimer.isActive())
  {
    this->playTimer.stop();
    this->playButton->setText("Play");
  }
  else
  {
    this->playTimer.start(std::max(10, 1000 / std::max(1, this->sceneData.fpsNum)));
    this->playButton->setText("Stop");
  }
}

void SequenceGeneratorWindow::layerRowChanged(int row)
{
  if (this->updating || row < 0)
    return;
  this->canvas->setSelected(this->layers->item(row)->data(Qt::UserRole).toInt());
  this->refreshProperties();
}

void SequenceGeneratorWindow::layerItemChanged(QListWidgetItem *item)
{
  if (this->updating || !item)
    return;
  const int i = item->data(Qt::UserRole).toInt();
  if (i >= 0 && i < int(this->sceneData.layers.size()))
    this->sceneData.layers[std::size_t(i)].visible = item->checkState() == Qt::Checked;
  this->canvas->update();
}

void SequenceGeneratorWindow::addShape()
{
  Layer l;
  l.kind = ShapeKind(this->shapeKind->currentData().toInt());
  l.name = std::string(kindName(l.kind)) + " " + std::to_string(this->sceneData.layers.size() + 1);
  l.w = std::max(16, this->sceneData.width / 8), l.h = l.w;
  l.x = (this->sceneData.width - l.w) / 2, l.y = (this->sceneData.height - l.h) / 2;
  l.colour = {180, 60, 200};
  this->sceneData.layers.push_back(l);
  this->canvas->setSelected(int(this->sceneData.layers.size()) - 1);
  this->refreshLayerList();
  this->refreshProperties();
  this->canvas->update();
}

void SequenceGeneratorWindow::addImage(const QImage &image, const QString &name)
{
  if (image.isNull())
  {
    this->status->setText("No image to add.");
    return;
  }
  const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
  Layer      l;
  l.kind   = ShapeKind::Image;
  l.name   = name.toStdString();
  l.patchW = rgba.width(), l.patchH = rgba.height();
  l.rgba.resize(std::size_t(l.patchW) * l.patchH * 4);
  for (int y = 0; y < l.patchH; ++y)
    std::copy_n(rgba.constScanLine(y), l.patchW * 4, l.rgba.begin() + std::ptrdiff_t(y) * l.patchW * 4);
  // Native size, but never wider than half the picture.
  const double fit = std::min(1.0, std::min(this->sceneData.width / 2.0 / l.patchW, this->sceneData.height / 2.0 / l.patchH));
  l.w = std::max(2.0, std::round(l.patchW * fit)), l.h = std::max(2.0, std::round(l.patchH * fit));
  l.x = std::round((this->sceneData.width - l.w) / 2), l.y = std::round((this->sceneData.height - l.h) / 2);
  this->sceneData.layers.push_back(l);
  this->canvas->setSelected(int(this->sceneData.layers.size()) - 1);
  this->refreshLayerList();
  this->refreshProperties();
  this->canvas->update();
}

void SequenceGeneratorWindow::addImageFromFile()
{
  const auto path = QFileDialog::getOpenFileName(this, "Image patch", QString(), "Images (*.png *.jpg *.jpeg *.bmp)");
  if (!path.isEmpty())
    this->addImage(QImage(path), QFileInfo(path).fileName());
}

void SequenceGeneratorWindow::addImageFromClipboard()
{
  this->addImage(QApplication::clipboard()->image(), "Pasted image");
}

void SequenceGeneratorWindow::moveLayer(int delta)
{
  const int i = this->canvas->selected(), j = i + delta;
  if (i < 0 || j < 0 || j >= int(this->sceneData.layers.size()))
    return;
  std::swap(this->sceneData.layers[std::size_t(i)], this->sceneData.layers[std::size_t(j)]);
  this->canvas->setSelected(j);
  this->refreshLayerList();
  this->canvas->update();
}

void SequenceGeneratorWindow::deleteLayer()
{
  const int i = this->canvas->selected();
  if (i < 0 || i >= int(this->sceneData.layers.size()))
    return;
  this->sceneData.layers.erase(this->sceneData.layers.begin() + i);
  this->canvas->setSelected(std::min(i, int(this->sceneData.layers.size()) - 1));
  this->refreshLayerList();
  this->refreshProperties();
}

void SequenceGeneratorWindow::pickColour(bool background)
{
  const int i = this->canvas->selected();
  Yuv8 &target = background ? this->sceneData.background : this->sceneData.layers[std::size_t(std::max(0, i))].colour;
  if (!background && (i < 0 || i >= int(this->sceneData.layers.size())))
    return;
  const QColor c = QColorDialog::getColor(yuvToColour(target), this, background ? "Background" : "Object colour");
  if (!c.isValid())
    return;
  target = generator::rgbToYuv709(c.red(), c.green(), c.blue());
  setButtonColour(background ? this->backgroundButton : this->colourButton, target);
  this->canvas->update();
}

bool SequenceGeneratorWindow::generate(const QString &path, QString *error)
{
  const auto why = generator::writeY4m(this->sceneData, path.toStdString());
  if (!why.empty())
  {
    if (error)
      *error = QString::fromStdString(why);
    this->status->setText("Not written: " + QString::fromStdString(why));
    return false;
  }
  if (this->sceneData.camera.mode != generator::CameraMode::Off)
  {
    const auto csv = generator::writeTrajectoryCsv(this->sceneData, (path + ".camera.csv").toStdString());
    if (!csv.empty())
    {
      if (error)
        *error = QString::fromStdString(csv);
      this->status->setText("Camera trajectory not written: " + QString::fromStdString(csv));
      return false;
    }
  }
  this->status->setText(QString("Wrote %1 (%2 frames, %3x%4) and opened it in the playlist.")
                            .arg(path)
                            .arg(this->sceneData.frames)
                            .arg(this->sceneData.width)
                            .arg(this->sceneData.height));
  emit generated(path);
  return true;
}

void SequenceGeneratorWindow::generateClicked()
{
  QSettings  settings;
  const auto dir  = settings.value("SequenceGenerator/lastDir", QDir::homePath()).toString();
  const auto name = QString("generated_%1x%2_%3f.y4m").arg(this->sceneData.width).arg(this->sceneData.height).arg(this->sceneData.frames);
  const auto path = QFileDialog::getSaveFileName(this, "Generate Y4M", dir + "/" + name, "Y4M (*.y4m)");
  if (path.isEmpty())
    return;
  settings.setValue("SequenceGenerator/lastDir", QFileInfo(path).absolutePath());
  QApplication::setOverrideCursor(Qt::WaitCursor);
  this->generate(path);
  QApplication::restoreOverrideCursor();
}

bool SequenceGeneratorWindow::saveScene(const QString &path, QString *error) const
{
  const auto colour = [](const Yuv8 &c) { return QJsonArray{c.y, c.u, c.v}; };
  QJsonArray list;
  for (const auto &l : this->sceneData.layers)
  {
    QJsonObject o{{"name", QString::fromStdString(l.name)}, {"visible", l.visible}, {"kind", int(l.kind)},
                  {"x", l.x}, {"y", l.y}, {"w", l.w}, {"h", l.h}, {"rotation", l.rotation},
                  {"colour", colour(l.colour)}, {"alpha", l.alpha},
                  {"vx", l.motion.vx}, {"vy", l.motion.vy}, {"start", l.motion.startFrame}, {"end", l.motion.endFrame},
                  {"rotationPerFrame", l.motion.rotationPerFrame}, {"scalePerFrame", l.motion.scalePerFrame},
                  {"edge", int(l.motion.edge)}};
    if (l.kind == ShapeKind::Image)
    {
      o["patchW"] = l.patchW, o["patchH"] = l.patchH;
      o["rgba"]   = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(l.rgba.data()), qsizetype(l.rgba.size())).toBase64());
    }
    list.append(o);
  }
  const QJsonObject rootBase{{"width", this->sceneData.width}, {"height", this->sceneData.height},
                         {"frames", this->sceneData.frames}, {"fps", this->sceneData.fpsNum},
                         {"background", colour(this->sceneData.background)},
                         {"flipH", this->sceneData.flipH}, {"flipV", this->sceneData.flipV}, {"layers", list}};
  QJsonObject root = rootBase;
  if (this->sceneData.camera.mode != generator::CameraMode::Off)
  {
    const auto &cam = this->sceneData.camera;
    root["camera"]  = QJsonObject{{"mode", int(cam.mode)}, {"ampX", cam.ampX}, {"ampY", cam.ampY},
                                  {"ampDeg", cam.ampDeg}, {"period", cam.period}, {"seed", double(cam.seed)}};
  }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = "cannot write " + path;
    return false;
  }
  file.write(QJsonDocument(root).toJson());
  return true;
}

bool SequenceGeneratorWindow::loadScene(const QString &path, QString *error)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = "cannot read " + path;
    return false;
  }
  const auto doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject())
  {
    if (error)
      *error = path + " is not a scene file";
    return false;
  }
  const auto root   = doc.object();
  const auto colour = [](const QJsonValue &v) {
    const auto a = v.toArray();
    return Yuv8{std::uint8_t(a[0].toInt(128)), std::uint8_t(a[1].toInt(128)), std::uint8_t(a[2].toInt(128))};
  };
  generator::Scene s;
  s.width      = root["width"].toInt(s.width) & ~1;
  s.height     = root["height"].toInt(s.height) & ~1;
  s.frames     = root["frames"].toInt(s.frames);
  s.fpsNum     = root["fps"].toInt(s.fpsNum);
  s.background = colour(root["background"]);
  s.flipH      = root["flipH"].toBool(false); // absent in scenes saved before the flips
  s.flipV      = root["flipV"].toBool(false);
  if (root["camera"].isObject()) // absent in scenes saved before camera shake: Off
  {
    const auto c    = root["camera"].toObject();
    s.camera.mode   = generator::CameraMode(std::clamp(c["mode"].toInt(), 0, 2));
    s.camera.ampX   = c["ampX"].toDouble(s.camera.ampX);
    s.camera.ampY   = c["ampY"].toDouble(s.camera.ampY);
    s.camera.ampDeg = c["ampDeg"].toDouble(s.camera.ampDeg);
    s.camera.period = std::max(1, c["period"].toInt(s.camera.period));
    s.camera.seed   = std::uint32_t(c["seed"].toDouble(s.camera.seed));
  }
  for (const auto &v : root["layers"].toArray())
  {
    const auto o = v.toObject();
    Layer      l;
    l.name    = o["name"].toString().toStdString();
    l.visible = o["visible"].toBool(true);
    l.kind    = ShapeKind(o["kind"].toInt());
    l.x = o["x"].toDouble(), l.y = o["y"].toDouble(), l.w = o["w"].toDouble(16), l.h = o["h"].toDouble(16);
    l.rotation = o["rotation"].toDouble();
    l.colour   = colour(o["colour"]);
    l.alpha    = std::uint8_t(o["alpha"].toInt(255));
    l.motion.vx = o["vx"].toDouble(), l.motion.vy = o["vy"].toDouble();
    l.motion.startFrame       = o["start"].toInt();
    l.motion.endFrame         = o["end"].toInt(-1);
    l.motion.rotationPerFrame = o["rotationPerFrame"].toDouble();
    l.motion.scalePerFrame    = o["scalePerFrame"].toDouble(1.0);
    l.motion.edge             = EdgeMode(o["edge"].toInt());
    if (l.kind == ShapeKind::Image)
    {
      l.patchW = o["patchW"].toInt(), l.patchH = o["patchH"].toInt();
      const auto bytes = QByteArray::fromBase64(o["rgba"].toString().toLatin1());
      l.rgba.assign(bytes.begin(), bytes.end());
    }
    s.layers.push_back(l);
  }
  this->sceneData = s;
  this->canvas->setSelected(s.layers.empty() ? -1 : int(s.layers.size()) - 1);
  this->sceneChanged();
  return true;
}

void SequenceGeneratorWindow::saveClicked()
{
  const auto path = QFileDialog::getSaveFileName(this, "Save scene", QString(), "Generator scene (*.json)");
  QString    error;
  if (!path.isEmpty() && !this->saveScene(path, &error))
    this->status->setText(error);
}

void SequenceGeneratorWindow::loadClicked()
{
  const auto path = QFileDialog::getOpenFileName(this, "Load scene", QString(), "Generator scene (*.json)");
  QString    error;
  if (!path.isEmpty() && !this->loadScene(path, &error))
    this->status->setText(error);
}

} // namespace bda::integration
