#include "GlobalMotionWindow.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <vector>

#include "integration/GmStatisticsAdapter.h"
#include "me/GmCsv.h"
#include "me/YuvLumaReader.h"
#include "playlistitem/playlistItem.h"
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
  form->addStretch(1);
  layout->addLayout(form);

  auto *buttons = new QHBoxLayout();
  this->runButton    = new QPushButton("Run", this);
  this->stopButton   = new QPushButton("Stop", this);
  this->exportButton = new QPushButton("Export CSV…", this);
  this->progress     = new QLabel(this);
  buttons->addWidget(this->runButton);
  buttons->addWidget(this->stopButton);
  buttons->addWidget(this->exportButton);
  buttons->addWidget(this->progress, 1);
  layout->addLayout(buttons);

  this->frames = new QTableWidget(0, 8, this);
  this->frames->setHorizontalHeaderLabels({"poc", "ref", "n", "accepted x", "accepted y",
                                           "median dx (px)", "median dy (px)",
                                           "refine moved (px)"});
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
  connect(this->frames, &QTableWidget::itemSelectionChanged, this,
          &GlobalMotionWindow::frameRowSelected);
  connect(this, &GlobalMotionWindow::frameDone, this, &GlobalMotionWindow::addFinishedFrames,
          Qt::QueuedConnection);
  connect(this, &GlobalMotionWindow::runFinished, this, &GlobalMotionWindow::finishRun,
          Qt::QueuedConnection);

  this->stopButton->setEnabled(false);
  this->runButton->setEnabled(false);
  this->exportButton->setEnabled(false);
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
  }
  this->shownRows    = 0;
  this->overlayFrame = -1;
  this->frames->setRowCount(0);
  this->windows->setRowCount(0);
  this->workerError.clear();
  this->cancel  = false;
  this->running = true;
  this->runButton->setEnabled(false);
  this->stopButton->setEnabled(true);
  this->exportButton->setEnabled(false);

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
  this->progress->setText(QString("Estimating %1 frames …").arg(last - first + 1));

  this->worker = QThread::create([this, p, first, last, file, y4m, w, h, depth] {
    me::YuvLumaReader reader;
    std::string       error;
    if (!(y4m ? reader.openY4m(file, error) : reader.openRaw(file, w, h, depth, error)))
    {
      this->workerError = QString::fromStdString(error);
      emit this->runFinished();
      return;
    }
    // Each frame is read once: this frame's current is the next one's reference.
    me::MePlane ref = reader.readLuma(first - 1, 0, error);
    for (int poc = first; poc <= last && !ref.empty() && !this->cancel; ++poc)
    {
      me::MePlane cur = reader.readLuma(poc, 0, error);
      if (cur.empty())
        break;
      auto result = me::estimateGlobalMotion(cur, ref, p);
      {
        std::lock_guard<std::mutex> guard(this->lock);
        this->done[poc] = std::move(result);
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

std::map<int, me::GmFrameResult> GlobalMotionWindow::results() const
{
  std::lock_guard<std::mutex> guard(this->lock);
  return this->done;
}

void GlobalMotionWindow::addFinishedFrames()
{
  const auto all = this->results();
  int        row = 0;
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
  this->runButton->setEnabled(true);
  this->stopButton->setEnabled(false);
  this->exportButton->setEnabled(this->shownRows > 0);

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
    for (int id = kGmAcceptedTypeId; id <= kGmWindowTypeId; ++id)
      data->eraseDataForTypeID(id);
  }
  else
  {
    /* setFrameIndex() first: it clears the cache when the index moves, so filling before it
     * would be thrown away. It also locks the data itself - no lock may be held here.
     */
    data->setFrameIndex(frameIdx);
    fillGmStatistics(*data, it->second, this->width, this->height);
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
}

} // namespace bda::integration
