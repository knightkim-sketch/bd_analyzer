#include "GlobalMotionWindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <fstream>
#include <vector>

#include "integration/GmStatisticsAdapter.h"
#include "integration/StabilizedY4mExport.h"
#include "me/GmCsv.h"
#include "me/IMotionEstimator.h"
#include "me/YuvLumaReader.h"
#include "playlistitem/playlistItem.h"
#include "playlistitem/playlistItemWithVideo.h"
#include "statistics/StatisticUIHandler.h"

namespace bda::integration
{

namespace
{

QTableWidgetItem *cell(const QString &text)
{
  auto *c = new QTableWidgetItem(text);
  c->setFlags(c->flags() & ~Qt::ItemIsEditable);
  return c;
}

QTableWidgetItem *cell(long long v) { return cell(QString::number(v)); }

QString median(std::vector<int> v)
{
  if (v.empty())
    return "-";
  std::sort(v.begin(), v.end());
  return QString::number(v[v.size() / 2]);
}

} // namespace

GlobalMotionWindow::GlobalMotionWindow(QWidget *parent) : QDialog(parent)
{
  this->setWindowTitle("Global motion");
  this->resize(1200, 800);

  auto *layout = new QVBoxLayout(this);

  this->summary = new QLabel(this);
  this->summary->setWordWrap(true);
  this->summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(this->summary);

  // Parameters, defaulting to GmParams - the design's values with the changes recorded in §11.
  const me::GmParams defaults;
  auto *form = new QHBoxLayout();
  const auto spin = [this, form](const QString &label, int lo, int hi, int value) {
    form->addWidget(new QLabel(label, this));
    auto *s = new QSpinBox(this);
    s->setRange(lo, hi);
    s->setValue(value);
    form->addWidget(s);
    return s;
  };
  this->firstFrame = spin("Frames", 1, 1, 1);
  this->lastFrame  = spin("to", 1, 1, 1);
  this->range      = spin("R (px)", 4, 1024, defaults.rOrig);
  this->refine     = spin("Refine ±", 0, 64, defaults.refineRange);
  this->range->setToolTip("Search range in source pixels. Capped at half the smallest window.");
  this->refine->setToolTip("Re-search at source resolution around the downsampled answer. 0 "
                           "switches it off; it never runs when the picture is not downsampled.");
  form->addWidget(new QLabel("Smoothing", this));
  this->smoothing = new QComboBox(this);
  this->smoothing->addItems({"off", "3-tap", "5-tap"});
  this->smoothing->setCurrentIndex(int(defaults.smoothing));
  form->addWidget(this->smoothing);
  form->addWidget(new QLabel("Brightness", this));
  this->brightness = new QComboBox(this);
  this->brightness->addItems({"off", "DC", "DC + gain"});
  this->brightness->setCurrentIndex(int(defaults.brightness));
  form->addWidget(this->brightness);
  form->addWidget(new QLabel("Model", this));
  this->model = new QComboBox(this);
  this->model->addItems({"off", "translation", "rotzoom", "affine"});
  this->model->setCurrentIndex(2);
  this->model->setToolTip("A global model fitted with RANSAC to SVT integer ME 16x16 vectors of the same "
                          "frame pair: blocks that do not follow it (local motion, mismatches) are "
                          "outliers. rotzoom is what libaom tries.");
  form->addWidget(this->model);
  form->addStretch(1);
  layout->addLayout(form);

  auto *buttons = new QHBoxLayout();
  this->runButton    = new QPushButton("Run", this);
  this->stopButton   = new QPushButton("Stop", this);
  this->exportButton = new QPushButton("Export CSV…", this);
  this->exportModelButton = new QPushButton("Export model CSV…", this);
  this->progress     = new QLabel(this);
  buttons->addWidget(this->runButton);
  buttons->addWidget(this->stopButton);
  buttons->addWidget(this->exportButton);
  buttons->addWidget(this->exportModelButton);
  buttons->addWidget(this->progress, 1);
  layout->addLayout(buttons);

  this->frames = new QTableWidget(0, 12, this);
  this->frames->setHorizontalHeaderLabels({"poc", "ref", "n", "accepted x", "accepted y",
                                           "median dx (px)", "median dy (px)",
                                           "refine moved (px)", "model", "model MV at centre (px)",
                                           "scale / angle (deg)", "inliers"});
  this->frames->setSelectionBehavior(QAbstractItemView::SelectRows);
  this->frames->setSelectionMode(QAbstractItemView::SingleSelection);
  this->frames->verticalHeader()->setVisible(false);
  this->frames->setToolTip("Medians are over accepted axes only. Select a frame to show it in the "
                           "main window with the windows drawn over it.");
  layout->addWidget(this->frames, 2);

  this->windows = new QTableWidget(0, 17, this);
  this->windows->setHorizontalHeaderLabels(
      {"window (i, j)", "dx (px)", "dy (px)", "coarse dx", "coarse dy", "accept x", "accept y",
       "cost best x", "cost zero x", "cost best y", "cost zero y", "a_q x", "a_q y", "sad2d best",
       "sad2d zero", "sad2d no x", "sad2d no y"});
  this->windows->verticalHeader()->setVisible(false);
  this->windows->setToolTip("Vectors point from the current picture into the reference: content "
                            "that moved right has a negative dx. Gates are judged at the "
                            "downsampled resolution; the refinement only moves the value.");
  layout->addWidget(this->windows, 3);

  // ---- Stabilization (DIS): the models above -> camera trajectory -> smoothing -> correction warps.
  auto *stabBox    = new QGroupBox("Stabilization (DIS)", this);
  auto *stabLayout = new QHBoxLayout(stabBox);
  auto *stabLeft   = new QWidget(stabBox);
  this->stabForm   = new QFormLayout(stabLeft);
  this->stabMethod = new QComboBox(stabBox);
  this->stabMethod->setObjectName("stabMethod");
  for (const auto m : {filter::SmoothingMethod::MovingAverage, filter::SmoothingMethod::Gaussian,
                       filter::SmoothingMethod::KalmanCausal, filter::SmoothingMethod::KalmanRts})
    this->stabMethod->addItem(filter::smoothingMethodName(m), int(m));
  const filter::StabilizerParams stabDefaults;
  this->stabMethod->setCurrentIndex(this->stabMethod->findData(int(stabDefaults.method)));
  this->stabMethod->setToolTip("Moving average and Gaussian look at frames on both sides; Kalman causal "
                               "only at past frames, as a real-time stabiliser would; Kalman RTS is its "
                               "offline (forward + backward) smoother.");
  this->stabForm->addRow("Method", this->stabMethod);
  this->stabRadius = new QSpinBox(stabBox);
  this->stabRadius->setRange(1, 200);
  this->stabRadius->setValue(stabDefaults.radius);
  this->stabForm->addRow("Radius (frames)", this->stabRadius);
  const auto dspin = [stabBox](double lo, double hi, double v, double step, int dec) {
    auto *d = new QDoubleSpinBox(stabBox);
    d->setRange(lo, hi), d->setDecimals(dec), d->setSingleStep(step), d->setValue(v);
    return d;
  };
  this->stabSigma = dspin(0.5, 100, stabDefaults.sigma, 0.5, 1);
  this->stabForm->addRow("Sigma (frames)", this->stabSigma);
  this->stabQ = dspin(0.0001, 100, stabDefaults.kalmanQ, 0.01, 4);
  this->stabQ->setToolTip("Process noise: how fast the intended camera path may change. Smaller is smoother.");
  this->stabForm->addRow("Kalman Q", this->stabQ);
  this->stabR = dspin(0.001, 1000, stabDefaults.kalmanR, 0.5, 3);
  this->stabR->setToolTip("Measurement noise: how much shake to expect on the path. Larger is smoother.");
  this->stabForm->addRow("Kalman R", this->stabR);
  this->applyToView = new QCheckBox("Apply to view", stabBox);
  this->applyToView->setToolTip("Show the stabilised picture in the main window: view, cache, pixel values "
                                "and statistics. Motion estimation keeps reading the source, and its "
                                "overlays stay in source coordinates.");
  this->stabForm->addRow(this->applyToView);
  this->exportStabilizedButton = new QPushButton("Export stabilized…", stabBox);
  this->stabForm->addRow(this->exportStabilizedButton);
  this->stabStatus = new QLabel(stabBox);
  this->stabStatus->setWordWrap(true);
  this->stabForm->addRow(this->stabStatus);
  stabLayout->addWidget(stabLeft);
  this->trajectoryPlot = new TrajectoryPlot(stabBox);
  stabLayout->addWidget(this->trajectoryPlot, 1);
  layout->addWidget(stabBox, 3);

  connect(this->stabMethod, &QComboBox::currentIndexChanged, this, [this] {
    this->updateStabilizationControls();
    this->updateStabilization();
  });
  connect(this->stabRadius, &QSpinBox::valueChanged, this, [this] { this->updateStabilization(); });
  for (auto *d : {this->stabSigma, this->stabQ, this->stabR})
    connect(d, &QDoubleSpinBox::valueChanged, this, [this] { this->updateStabilization(); });
  connect(this->applyToView, &QCheckBox::toggled, this, [this](bool on) {
    QString why;
    if (!this->setApplyToView(on, &why))
    {
      const QSignalBlocker block(this->applyToView);
      this->applyToView->setChecked(false);
      this->stabStatus->setText(why);
    }
  });
  connect(this->exportStabilizedButton, &QPushButton::clicked, this, [this] {
    const auto out = QFileDialog::getSaveFileName(this, "Export stabilized Y4M",
                                                  QFileInfo(this->path).baseName() + "_stabilized.y4m",
                                                  "Y4M (*.y4m)");
    QString error;
    if (!out.isEmpty() && !this->exportStabilized(out, &error))
      this->stabStatus->setText(error);
  });
  this->updateStabilizationControls();
  this->updateStabilization();

  connect(this->runButton, &QPushButton::clicked, this, [this] { this->run(); });
  connect(this->stopButton, &QPushButton::clicked, this, &GlobalMotionWindow::stop);
  connect(this->exportButton, &QPushButton::clicked, this, [this] {
    const auto out = QFileDialog::getSaveFileName(this, "Export global motion CSV",
                                                  QFileInfo(this->path).baseName() + ".gm.csv",
                                                  "CSV (*.csv)");
    QString error;
    if (!out.isEmpty() && !this->exportCsv(out, &error))
      this->progress->setText(error);
  });
  connect(this->exportModelButton, &QPushButton::clicked, this, [this] {
    const auto out = QFileDialog::getSaveFileName(this, "Export global model CSV",
                                                  QFileInfo(this->path).baseName() + ".gm-model.csv",
                                                  "CSV (*.csv)");
    QString error;
    if (!out.isEmpty() && !this->exportModelCsv(out, &error))
      this->progress->setText(error);
  });
  connect(this->frames, &QTableWidget::itemSelectionChanged, this,
          &GlobalMotionWindow::frameRowSelected);
  connect(this, &GlobalMotionWindow::frameDone, this, &GlobalMotionWindow::addFinishedFrames,
          Qt::QueuedConnection);
  connect(this, &GlobalMotionWindow::runFinished, this, &GlobalMotionWindow::finishRun,
          Qt::QueuedConnection);

  this->stopButton->setEnabled(false);
  this->runButton->setEnabled(false);
  this->exportButton->setEnabled(false);
  this->exportModelButton->setEnabled(false);
}

GlobalMotionWindow::~GlobalMotionWindow()
{
  // The worker writes into this object; let it see the request and finish first.
  this->cancel = true;
  if (this->worker)
    this->worker->wait();
}

void GlobalMotionWindow::closeEvent(QCloseEvent *event)
{
  this->stop();
  QDialog::closeEvent(event);
}

StreamDiffResult GlobalMotionWindow::setTarget(const QList<playlistItem *> &selection)
{
  if (this->running)
    return {false, "A run is in progress in this window. Stop it first."};
  if (selection.size() != 1)
    return {false, QString("Select exactly one item. The selection has %1.").arg(selection.size())};

  auto *target = selection.front();
  if (target == nullptr || !target->supportsMotionEstimation() ||
      target->getMotionEstimationData() == nullptr)
    return {false, "Global motion searches the source pictures. Select a raw YUV file, or a "
                   "compressed stream with its original YUV attached (Properties)."};

  // A compressed item reads its attached original; a raw item is its own file.
  const auto original = target->getOriginalYUVSource();
  const auto file     = original.isEmpty() ? target->properties().name : original;
  const auto format   = target->getRawPixelFormat();
  const auto size     = target->getRawFrameSize();

  me::YuvLumaReader reader;
  std::string       error;
  const bool        y4m = file.endsWith(".y4m", Qt::CaseInsensitive);
  if (!y4m)
  {
    if (format.getSubsampling() != video::yuv::Subsampling::YUV_420 || !format.isPlanar() ||
        format.isUVInterleaved() || format.getPlaneOrder() != video::yuv::PlaneOrder::YUV ||
        (format.getBitsPerSample() != 8 && format.getBitsPerSample() != 10) ||
        format.isBigEndian())
      return {false, QString("%1 is %2. The estimator reads planar 4:2:0 at 8 or 10 bits.")
                         .arg(QFileInfo(file).fileName())
                         .arg(QString::fromStdString(format.getName()))};
  }
  const bool opened =
      y4m ? reader.openY4m(file.toStdString(), error)
          : reader.openRaw(file.toStdString(), int(size.width), int(size.height),
                           int(format.getBitsPerSample()), error);
  if (!opened)
    return {false, QString::fromStdString(error)};
  if (reader.frameCount() < 2)
    return {false, "The clip has a single frame; there is no reference to search."};

  // A new clip: the old item stops showing a trajectory that is not its own.
  {
    const QSignalBlocker block(this->applyToView);
    this->applyToView->setChecked(false);
  }
  this->setApplyToView(false);
  this->item       = target;
  this->path       = file;
  this->isY4m      = y4m;
  this->width      = reader.width();
  this->height     = reader.height();
  this->bitDepth   = reader.bitDepth();
  this->frameCount = reader.frameCount();
  {
    std::lock_guard<std::mutex> guard(this->lock);
    this->done.clear();
    this->modelDone.clear();
  }
  this->shownRows    = 0;
  this->overlayFrame = -1;
  this->frames->setRowCount(0);
  this->windows->setRowCount(0);

  // Frame 0 has no past reference, so the range starts at 1.
  this->firstFrame->setRange(1, this->frameCount - 1);
  this->lastFrame->setRange(1, this->frameCount - 1);
  this->firstFrame->setValue(1);
  this->lastFrame->setValue(this->frameCount - 1);

  const int n = me::gmDownsampleExponent(this->width);
  this->summary->setText(QString("%1\n%2x%3, %4-bit, %5 frames. Searched at 1/%6 (n = %7) against "
                                 "the previous frame in display order.")
                             .arg(this->path)
                             .arg(this->width)
                             .arg(this->height)
                             .arg(this->bitDepth)
                             .arg(this->frameCount)
                             .arg(1 << n)
                             .arg(n));
  this->runButton->setEnabled(true);
  this->exportButton->setEnabled(false);
  this->progress->clear();
  return {true, {}};
}

me::GmParams GlobalMotionWindow::params() const
{
  me::GmParams p;
  p.rOrig       = this->range->value();
  p.refineRange = this->refine->value();
  p.smoothing   = me::GmSmoothing(this->smoothing->currentIndex());
  p.brightness  = me::GmBrightness(this->brightness->currentIndex());
  return p;
}

bool GlobalMotionWindow::run()
{
  if (this->running || this->path.isEmpty() || !this->item)
    return false;

  {
    std::lock_guard<std::mutex> guard(this->lock);
    this->done.clear();
    this->modelDone.clear();
  }
  this->shownRows    = 0;
  this->overlayFrame = -1;
  this->frames->setRowCount(0);
  this->windows->setRowCount(0);
  this->workerError.clear();
  this->cancel  = false;
  this->running = true;
  this->updateStabilization(); // no models during a run: controls off until it finishes
  this->runButton->setEnabled(false);
  this->stopButton->setEnabled(true);
  this->exportButton->setEnabled(false);
  this->exportModelButton->setEnabled(false);

  /* Register the overlay types now, on the GUI thread and once per run, which is when the
   * statistics panel may rebuild its rows - never from a repaint (see MainWindow's ME path).
   */
  if (auto *data = this->item->getMotionEstimationData(); data && syncGmStatTypes(*data))
    if (auto *handler = this->item->getMotionEstimationUIHandler())
      handler->updateStatisticsHandlerControls();

  const auto p     = this->params();
  const int  first = this->firstFrame->value();
  const int  last  = std::max(first, this->lastFrame->value());
  const auto file  = this->path.toStdString();
  const bool y4m   = this->isY4m;
  const int  w = this->width, h = this->height, depth = this->bitDepth;
  const int  modelType = this->model->currentIndex() - 1; // -1: off
  this->progress->setText(QString("Estimating %1 frames …").arg(last - first + 1));

  this->worker = QThread::create([this, p, first, last, file, y4m, w, h, depth, modelType] {
    me::YuvLumaReader reader;
    std::string       error;
    if (!(y4m ? reader.openY4m(file, error) : reader.openRaw(file, w, h, depth, error)))
    {
      this->workerError = QString::fromStdString(error);
      emit this->runFinished();
      return;
    }
    /* Each frame is read once: this frame's current is the next one's reference. The block ME the
     * model is fitted to reads past the picture edge, so then the planes carry odyssey's full pad;
     * the window estimator only reads inside the picture and does not care.
     */
    const int   pad = modelType >= 0 ? me::kOdysseyPadFull : 0;
    me::MePlane ref = reader.readLuma(first - 1, pad, error);
    auto        estimator = me::makeEstimator(me::Algorithm::SvtIntegerMe);
    for (int poc = first; poc <= last && !ref.empty() && !this->cancel; ++poc)
    {
      me::MePlane cur = reader.readLuma(poc, pad, error);
      if (cur.empty())
        break;
      auto result = me::estimateGlobalMotion(cur, ref, p);
      std::optional<me::GmModelResult> model;
      if (modelType >= 0)
      {
        me::MePictureSet pictures;
        pictures.current     = cur;
        pictures.reference   = ref;
        pictures.frameIdx    = poc;
        pictures.refFrameIdx = poc - 1;
        me::MeParams meParams;
        meParams.blockSizes = me::BlockSizeSet();
        meParams.blockSizes.add(me::BlockSize::Blk16);
        const auto        blocks = estimator->estimateFrame(pictures, meParams);
        me::GmModelParams mp;
        mp.type = me::GmModelType(modelType);
        model   = me::fitGlobalModel(blocks, w, h, mp);
      }
      {
        std::lock_guard<std::mutex> guard(this->lock);
        this->done[poc] = std::move(result);
        if (model)
          this->modelDone[poc] = std::move(*model);
      }
      emit this->frameDone();
      ref = std::move(cur);
    }
    if (!error.empty())
      this->workerError = QString::fromStdString(error);
    emit this->runFinished();
  });
  connect(this->worker, &QThread::finished, this->worker, &QObject::deleteLater);
  this->worker->start();
  return true;
}

void GlobalMotionWindow::stop()
{
  if (this->running)
    this->cancel = true;
}

playlistItem *GlobalMotionWindow::target() const { return this->item; }

std::map<int, me::GmFrameResult> GlobalMotionWindow::results() const
{
  std::lock_guard<std::mutex> guard(this->lock);
  return this->done;
}

std::map<int, me::GmModelResult> GlobalMotionWindow::modelResults() const
{
  std::lock_guard<std::mutex> guard(this->lock);
  return this->modelDone;
}

void GlobalMotionWindow::addFinishedFrames()
{
  const auto all    = this->results();
  const auto models = this->modelResults();
  int        row    = 0;
  for (const auto &[poc, r] : all)
  {
    if (row++ < this->shownRows)
      continue;
    std::vector<int> dxs, dys;
    int              ax = 0, ay = 0, moved = 0;
    for (const auto &w : r.windows)
    {
      ax += w.acceptX;
      ay += w.acceptY;
      if (w.acceptX)
        dxs.push_back(w.x.dFull);
      if (w.acceptY)
        dys.push_back(w.y.dFull);
      moved = std::max({moved, std::abs(w.x.dFull - (w.x.d << r.n)),
                        std::abs(w.y.dFull - (w.y.d << r.n))});
    }
    const int at = this->frames->rowCount();
    this->frames->insertRow(at);
    this->frames->setItem(at, 0, cell(poc));
    this->frames->setItem(at, 1, cell(poc - 1));
    this->frames->setItem(at, 2, cell(r.ok() ? QString::number(r.n) : QString::fromStdString(r.error)));
    this->frames->setItem(at, 3, cell(QString("%1 / 16").arg(ax)));
    this->frames->setItem(at, 4, cell(QString("%1 / 16").arg(ay)));
    this->frames->setItem(at, 5, cell(median(dxs)));
    this->frames->setItem(at, 6, cell(median(dys)));
    this->frames->setItem(at, 7, cell(moved));
    if (const auto m = models.find(poc); m != models.end())
    {
      const auto &model = m->second;
      if (model.ok())
      {
        const auto mv = me::gmModelMvAt(model, this->width / 2.0, this->height / 2.0);
        this->frames->setItem(at, 8, cell(me::gmModelTypeName(model.type)));
        this->frames->setItem(at, 9, cell(QString("(%1, %2)").arg(mv.x / 8.0).arg(mv.y / 8.0)));
        this->frames->setItem(
            at, 10, cell(QString("%1 / %2").arg(model.scale(), 0, 'f', 4).arg(model.angleDegrees(), 0, 'f', 3)));
      }
      else
        this->frames->setItem(at, 8, cell(QString::fromStdString(model.error)));
      this->frames->setItem(at, 11, cell(QString("%1 / %2").arg(model.inliers).arg(model.total)));
    }
    ++this->shownRows;
  }
  this->progress->setText(QString("%1 of %2 frames")
                              .arg(this->shownRows)
                              .arg(this->lastFrame->value() - this->firstFrame->value() + 1));
}

void GlobalMotionWindow::finishRun()
{
  this->addFinishedFrames();
  this->running = false;
  this->updateStabilization();
  this->runButton->setEnabled(true);
  this->stopButton->setEnabled(false);
  this->exportButton->setEnabled(this->shownRows > 0);
  this->exportModelButton->setEnabled(!this->modelResults().empty());

  long accepted = 0, axes = 0;
  for (const auto &[poc, r] : this->results())
    for (const auto &w : r.windows)
    {
      accepted += int(w.acceptX) + int(w.acceptY);
      axes += 2;
    }
  QString text = QString("%1 frames, %2 of %3 axes accepted").arg(this->shownRows).arg(accepted).arg(axes);
  if (this->cancel)
    text += " (stopped)";
  if (!this->workerError.isEmpty())
    text += " - " + this->workerError;
  this->progress->setText(text);
}

bool GlobalMotionWindow::exportCsv(const QString &out, QString *error) const
{
  std::ofstream file(out.toStdString());
  if (!file)
  {
    if (error)
      *error = "Cannot write " + out;
    return false;
  }
  me::writeGmCsvHeader(file);
  for (const auto &[poc, r] : this->results())
    me::writeGmCsvRows(file, poc, poc - 1, r);
  return bool(file);
}

bool GlobalMotionWindow::exportModelCsv(const QString &out, QString *error) const
{
  std::ofstream file(out.toStdString());
  if (!file)
  {
    if (error)
      *error = "Cannot write " + out;
    return false;
  }
  // p0..p5 map a current-picture position to the reference: x' = p0 + p2 x + p3 y, y' = p1 + p4 x + p5 y.
  file << "poc,ref_poc,type,ok,p0,p1,p2,p3,p4,p5,inliers,total,rms\n";
  file.precision(9);
  for (const auto &[poc, m] : this->modelResults())
  {
    file << poc << ',' << poc - 1 << ',' << me::gmModelTypeName(m.type) << ',' << int(m.ok());
    for (double v : m.p)
      file << ',' << v;
    file << ',' << m.inliers << ',' << m.total << ',' << m.rms << '\n';
  }
  return bool(file);
}

void GlobalMotionWindow::selectFrame(int frameIdx)
{
  for (int row = 0; row < this->frames->rowCount(); ++row)
    if (this->frames->item(row, 0)->text().toInt() == frameIdx)
    {
      this->frames->selectRow(row);
      return;
    }
}

void GlobalMotionWindow::frameRowSelected()
{
  const auto rows = this->frames->selectionModel()->selectedRows();
  if (rows.isEmpty())
    return;
  const int poc = this->frames->item(rows.front().row(), 0)->text().toInt();
  this->showWindows(poc);
  this->drawOverlay(poc);
  if (this->item)
    emit this->showFrameRequested(this->item, poc);
}

void GlobalMotionWindow::showWindows(int frameIdx)
{
  const auto all = this->results();
  const auto it  = all.find(frameIdx);
  this->windows->setRowCount(0);
  if (it == all.end())
    return;
  const auto &r = it->second;
  for (const auto &w : r.windows)
  {
    const int row = this->windows->rowCount();
    this->windows->insertRow(row);
    int c = 0;
    this->windows->setItem(row, c++, cell(QString("(%1, %2)").arg(w.winI).arg(w.winJ)));
    this->windows->setItem(row, c++, cell(w.x.dFull));
    this->windows->setItem(row, c++, cell(w.y.dFull));
    this->windows->setItem(row, c++, cell(w.x.d << r.n));
    this->windows->setItem(row, c++, cell(w.y.d << r.n));
    this->windows->setItem(row, c++, cell(w.acceptX ? "yes" : "no"));
    this->windows->setItem(row, c++, cell(w.acceptY ? "yes" : "no"));
    this->windows->setItem(row, c++, cell(w.x.costBest));
    this->windows->setItem(row, c++, cell(w.x.costZero));
    this->windows->setItem(row, c++, cell(w.y.costBest));
    this->windows->setItem(row, c++, cell(w.y.costZero));
    this->windows->setItem(row, c++, cell(w.x.aQ));
    this->windows->setItem(row, c++, cell(w.y.aQ));
    this->windows->setItem(row, c++, cell(w.sad2dBest));
    this->windows->setItem(row, c++, cell(w.sad2dZero));
    this->windows->setItem(row, c++, cell(w.sad2dNoX));
    this->windows->setItem(row, c++, cell(w.sad2dNoY));
  }
  this->windows->resizeColumnsToContents();
}

void GlobalMotionWindow::drawOverlay(int frameIdx)
{
  if (!this->item || frameIdx == this->overlayFrame)
    return;
  auto *data = this->item->getMotionEstimationData();
  if (data == nullptr)
    return;

  const auto all = this->results();
  const auto it  = all.find(frameIdx);
  if (it == all.end())
  {
    // Not estimated: take the windows away rather than leave another frame's on screen. The data
    // only - the types stay, the statistics panel holds rows for them.
    for (int id = kGmAcceptedTypeId; id <= kGmLastTypeId; ++id)
      data->eraseDataForTypeID(id);
  }
  else
  {
    /* setFrameIndex() first: it clears the cache when the index moves, so filling before it
     * would be thrown away. It also locks the data itself - no lock may be held here.
     */
    data->setFrameIndex(frameIdx);
    fillGmStatistics(*data, it->second, this->width, this->height);
    const auto models = this->modelResults();
    const auto m      = models.find(frameIdx);
    fillGmModelStatistics(*data, m == models.end() ? nullptr : &m->second, this->width, this->height);
  }
  this->overlayFrame = frameIdx;
  emit this->overlayChanged();
}

void GlobalMotionWindow::setCurrentFrame(playlistItem *shown, int frameIdx)
{
  // Called on every repaint as well; drawOverlay() does nothing unless the frame really moved.
  if (shown == nullptr || shown != this->item.data() || frameIdx == this->overlayFrame)
    return;
  this->drawOverlay(frameIdx);
  this->showWindows(frameIdx);
  this->trajectoryPlot->setCurrentFrame(frameIdx);
}

filter::StabilizerParams GlobalMotionWindow::stabilizerParams() const
{
  filter::StabilizerParams p;
  p.method  = filter::SmoothingMethod(this->stabMethod->currentData().toInt());
  p.radius  = this->stabRadius->value();
  p.sigma   = this->stabSigma->value();
  p.kalmanQ = this->stabQ->value();
  p.kalmanR = this->stabR->value();
  return p;
}

void GlobalMotionWindow::updateStabilizationControls()
{
  const auto method = filter::SmoothingMethod(this->stabMethod->currentData().toInt());
  const bool kalman = method == filter::SmoothingMethod::KalmanCausal || method == filter::SmoothingMethod::KalmanRts;
  this->stabForm->setRowVisible(this->stabRadius, method == filter::SmoothingMethod::MovingAverage);
  this->stabForm->setRowVisible(this->stabSigma, method == filter::SmoothingMethod::Gaussian);
  this->stabForm->setRowVisible(this->stabQ, kalman);
  this->stabForm->setRowVisible(this->stabR, kalman);
}

std::map<int, filter::AffineParams> GlobalMotionWindow::modelsByFrame() const
{
  // A frame whose fit failed is left out: the stabiliser takes it as no motion.
  std::map<int, filter::AffineParams> out;
  for (const auto &[poc, m] : this->modelResults())
    if (m.ok())
      out[poc] = m.p;
  return out;
}

void GlobalMotionWindow::updateStabilization()
{
  const auto models = this->modelResults();
  const bool have   = !models.empty() && !this->running;
  this->applyToView->setEnabled(have);
  this->exportStabilizedButton->setEnabled(have);
  if (!have)
  {
    this->trajectory.clear();
    this->trajectoryPlot->setTrajectory(0, {});
    this->stabStatus->setText(this->running ? "Estimating …"
                                            : "Run with a Model (not off) to get the camera trajectory.");
    return;
  }
  // The run's frames and the reference of its first one.
  this->trajectoryFirst = models.begin()->first - 1;
  const int last        = models.rbegin()->first;
  this->trajectory = filter::stabilizeByFrame(this->modelsByFrame(), this->trajectoryFirst, last, this->width,
                                              this->height, this->stabilizerParams());
  this->trajectoryPlot->setTrajectory(this->trajectoryFirst, this->trajectory);

  double maxCorrection = 0;
  for (const auto &f : this->trajectory)
    maxCorrection = std::max(maxCorrection, std::hypot(f.correction.dx, f.correction.dy));
  int failed = 0;
  for (const auto &[poc, m] : models)
    failed += !m.ok();
  this->stabStatus->setText(QString("Frames %1-%2, %3. Largest correction %4 px%5.")
                                .arg(this->trajectoryFirst)
                                .arg(last)
                                .arg(filter::smoothingMethodName(this->stabilizerParams().method))
                                .arg(maxCorrection, 0, 'f', 1)
                                .arg(failed ? QString(", %1 frames without a model taken as still").arg(failed)
                                            : QString()));
  if (this->applyToView->isChecked())
  {
    QString why;
    if (!this->setApplyToView(true, &why))
      this->stabStatus->setText(why);
  }
}

bool GlobalMotionWindow::setApplyToView(bool on, QString *why)
{
  // Switching off, or moving to another item: the item that shows the warp gets it taken away.
  if (this->stabilisedItem && (!on || this->stabilisedItem.data() != this->item.data()))
  {
    if (auto *video = dynamic_cast<playlistItemWithVideo *>(this->stabilisedItem.data()))
      video->setStabilization(nullptr);
    this->stabilisedItem = nullptr;
  }
  if (on && this->applyToView->isChecked() != on)
  {
    const QSignalBlocker block(this->applyToView);
    this->applyToView->setChecked(true);
  }
  if (!on)
  {
    emit this->overlayChanged();
    return true;
  }
  auto *video = dynamic_cast<playlistItemWithVideo *>(this->item.data());
  if (video == nullptr || this->trajectory.empty())
  {
    if (why)
      *why = video == nullptr ? "The item cannot show a stabilised picture." : "There is no trajectory yet.";
    return false;
  }
  // A new generation for every table, from one counter shared by all windows, so a cache never
  // mistakes one trajectory for another.
  static std::atomic<std::uint64_t> generations{0};
  auto table        = std::make_shared<StabilizationTable>();
  table->generation = ++generations;
  for (std::size_t i = 0; i < this->trajectory.size(); ++i)
    table->warps[this->trajectoryFirst + int(i)] = this->trajectory[i].warp;
  if (!video->setStabilization(table, why))
    return false;
  this->stabilisedItem = this->item;
  emit this->overlayChanged();
  return true;
}

bool GlobalMotionWindow::exportStabilized(const QString &out, QString *error)
{
  if (!this->item || this->modelResults().empty())
  {
    if (error)
      *error = "Run with a Model first.";
    return false;
  }
  QProgressDialog progress("Writing the stabilized Y4M …", "Cancel", 0, 0, this);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(500);
  const auto result = exportStabilizedY4m(this->item, out, this->modelsByFrame(), this->stabilizerParams(),
                                          [&progress](int done, int total) {
                                            progress.setMaximum(total);
                                            progress.setValue(done);
                                            QCoreApplication::processEvents();
                                            return !progress.wasCanceled();
                                          });
  if (!result.ok())
  {
    if (error)
      *error = result.cancelled ? QString("Cancelled - %1 is incomplete.").arg(out) : result.error;
    return false;
  }
  this->stabStatus->setText(QString("Wrote %1 frames to %2 (trajectory in %3).")
                                .arg(result.framesWritten)
                                .arg(out)
                                .arg(result.csvPath));
  return true;
}

} // namespace bda::integration
