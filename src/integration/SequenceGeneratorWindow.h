// The Y4M sequence generator window: draw shapes or paste image patches on a canvas, one object
// per layer, give each a motion, preview it frame by frame and write the composited sequence as a
// y4m, which MainWindow then opens in the playlist.
//
// The scene and its rendering are Qt-free (src/generator/Scene); this file is the editor around it.
// Design: docs/ai/30-designs/y4m-sequence-generator-design.md
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

#include "generator/Scene.h"

namespace bda::integration
{

//!< The preview: the composited frame scaled to fit, with mouse create / select / move / resize.
class GeneratorCanvas : public QWidget
{
  Q_OBJECT

public:
  explicit GeneratorCanvas(generator::Scene &scene, QWidget *parent = nullptr);

  void setFrame(int frame);
  int  frame() const { return this->frameIdx; }
  void setSelected(int layer);
  int  selected() const { return this->selectedLayer; }
  //!< The shape a drag on an empty area creates.
  void setCreateKind(generator::ShapeKind kind) { this->createKind = kind; }

signals:
  void selectionChanged(int layer);
  void sceneEdited();

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  double  scale() const;
  QPointF origin() const;
  QPointF toScene(const QPointF &widgetPos) const;
  QRectF  layerBox(int layer) const; //!< Axis-aligned box at the shown frame, scene pixels.
  int     layerAt(const QPointF &scenePos) const;

  generator::Scene    &scene;
  int                  frameIdx{0};
  int                  selectedLayer{-1};
  generator::ShapeKind createKind{generator::ShapeKind::Rect};

  enum class Drag
  {
    None,
    Create,
    Move,
    Resize,
  } drag{Drag::None};
  QPointF pressScene, lastScene;
  QRectF  pressBox; //!< The selected layer's frame-0 box when the drag started.
};

class SequenceGeneratorWindow : public QDialog
{
  Q_OBJECT

public:
  explicit SequenceGeneratorWindow(QWidget *parent = nullptr);

  generator::Scene       &scene() { return this->sceneData; }
  const generator::Scene &scene() const { return this->sceneData; }
  //!< After changing scene() directly, refresh the controls and the preview.
  void sceneChanged();

  /* Write the sequence to `path`; emits generated(path) on success. For tests and the button. */
  bool generate(const QString &path, QString *error = nullptr);

  bool saveScene(const QString &path, QString *error = nullptr) const;
  bool loadScene(const QString &path, QString *error = nullptr);

signals:
  void generated(const QString &path);

private slots:
  void addShape();
  void addImageFromFile();
  void addImageFromClipboard();
  void moveLayer(int delta);
  void deleteLayer();
  void layerRowChanged(int row);
  void layerItemChanged(QListWidgetItem *item);
  void propertiesEdited();
  void sequenceEdited();
  void frameChanged(int frame);
  void togglePlay();
  void generateClicked();
  void saveClicked();
  void loadClicked();

private:
  void addImage(const QImage &image, const QString &name);
  void refreshLayerList();
  void refreshProperties();
  void pickColour(bool background);

  generator::Scene sceneData;
  GeneratorCanvas *canvas{};
  bool             updating{}; //!< Set while controls are filled from the scene, so edits are not echoed back.

  QSpinBox    *width{}, *height{}, *frames{}, *fps{};
  QPushButton *backgroundButton{};
  QComboBox   *shapeKind{};
  QListWidget *layers{};
  QSpinBox    *x{}, *y{}, *w{}, *h{}, *alpha{};
  QDoubleSpinBox *rotation{};
  QPushButton    *colourButton{};
  QDoubleSpinBox *vx{}, *vy{}; //!< Fractional pixels per frame (sub-pixel motion)
  QSpinBox       *startFrame{}, *endFrame{};
  QComboBox      *edge{};
  QDoubleSpinBox *rotationPerFrame{}, *scalePerFrame{};
  QSlider        *frameSlider{};
  QLabel         *frameLabel{}, *status{};
  QPushButton    *playButton{};
  QTimer          playTimer;
};

} // namespace bda::integration
