#include "StabilizedY4mExport.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSpinBox>

#include <cmath>
#include <fstream>
#include <string>

#include <playlistitem/playlistItem.h>

#include "MeFrameSource.h"
#include "filter/AffineWarp.h"
#include "filter/Y4mWriter.h"
#include "me/IMotionEstimator.h"

namespace bda::integration
{
namespace
{

/* The same acceptance rules as the filtered export. Empty when the item can be exported. */
QString checkItem(playlistItem *item, me::YuvFrameLayout &layout)
{
  if (!item || !item->supportsMotionEstimation())
    return QObject::tr("Select an item with a raw source: a raw YUV file, or a stream with its "
                       "original YUV attached.");
  QString why;
  if (!yuvLayoutFromRawFormat(item->getRawPixelFormat(), item->getRawFrameSize(), layout, &why))
    return why;
  const bool mono = layout.subX == 0;
  if (layout.bigEndian || layout.interleavedUV || (!mono && (layout.subX != 2 || layout.subY != 2)) ||
      (layout.bitDepth != 8 && layout.bitDepth != 10))
    return QObject::tr("Y4M export takes planar 4:2:0 or 4:0:0, 8 or 10 bit, little endian. This "
                       "item's format is %1.")
        .arg(QString::fromStdString(item->getRawPixelFormat().getName()));
  const auto range = item->properties().startEndRange;
  if (range.first < 0 || range.second < range.first)
    return QObject::tr("The item has no frames to export.");
  return {};
}

std::vector<std::uint16_t> readPlane(const QByteArray &frame, std::int64_t offset, int w, int h, int bitDepth)
{
  std::vector<std::uint16_t> plane(std::size_t(w) * h);
  const auto *data = reinterpret_cast<const std::uint8_t *>(frame.constData()) + offset;
  if (bitDepth > 8)
    for (std::size_t i = 0; i < plane.size(); ++i)
      plane[i] = std::uint16_t(data[2 * i] | (data[2 * i + 1] << 8));
  else
    for (std::size_t i = 0; i < plane.size(); ++i)
      plane[i] = data[i];
  return plane;
}

std::vector<std::uint8_t> planeBytes(const std::vector<std::uint16_t> &plane, int bitDepth)
{
  std::vector<std::uint8_t> bytes(plane.size() * (bitDepth > 8 ? 2 : 1));
  if (bitDepth > 8)
    for (std::size_t i = 0; i < plane.size(); ++i)
      bytes[2 * i] = std::uint8_t(plane[i] & 0xff), bytes[2 * i + 1] = std::uint8_t(plane[i] >> 8);
  else
    for (std::size_t i = 0; i < plane.size(); ++i)
      bytes[i] = std::uint8_t(plane[i]);
  return bytes;
}

/* Pass 2: warps every frame of the item's range with result.trajectory[k].warp, writes `path` and
 * `path`.trajectory.csv, and fills framesWritten, csvPath, error / cancelled. `step(done)` is called
 * after each frame written (done = 1..count) and cancels by returning false.
 */
void writeStabilized(playlistItem                    *item,
                     const QString                   &path,
                     const me::YuvFrameLayout        &layout,
                     StabilizedExportResult          &result,
                     const std::function<bool(int)> &step)
{
  const auto range = item->properties().startEndRange;
  const int  count = range.second - range.first + 1;
  const bool mono  = layout.subX == 0;
  const int  w = layout.width, h = layout.height, bd = layout.bitDepth;

  filter::Y4mFormat y4m;
  y4m.width    = w;
  y4m.height   = h;
  y4m.bitDepth = bd;
  y4m.mono     = mono;
  const auto fps     = item->properties().frameRate;
  y4m.fpsNumerator   = fps > 0 ? int(std::lround(fps * 1000)) : 30000;
  y4m.fpsDenominator = 1000;
  const auto lumaEnd   = layout.offset[0] + y4m.lumaBytes();
  const auto chromaEnd = std::max(layout.offset[1], layout.offset[2]) + y4m.chromaBytes();
  const auto needed    = mono ? lumaEnd : std::max(lumaEnd, chromaEnd);

  // Pass 2: warp and write.
  filter::Y4mWriter writer;
  std::string       error;
  if (!writer.open(path.toStdString(), y4m, error))
  {
    result.error = QString::fromStdString(error);
    return;
  }
  for (int k = 0; k < count; ++k)
  {
    const auto frame = item->readRawFrame(range.first + k);
    if (frame.size() < needed)
    {
      result.error = QObject::tr("Could not read frame %1").arg(range.first + k);
      break;
    }
    filter::Frame420u16 in;
    in.width  = w;
    in.height = h;
    in.y      = readPlane(frame, layout.offset[0], w, h, bd);
    if (!mono)
    {
      in.u = readPlane(frame, layout.offset[1], w / 2, h / 2, bd);
      in.v = readPlane(frame, layout.offset[2], w / 2, h / 2, bd);
    }
    const auto out = filter::warpFrame420(in, result.trajectory[std::size_t(k)].warp, bd, filter::ChromaSiting::Centre);
    const auto y = planeBytes(out.y, bd), u = planeBytes(out.u, bd), v = planeBytes(out.v, bd);
    if (!writer.writeFrame(y.data(), mono ? nullptr : u.data(), mono ? nullptr : v.data(), error))
    {
      result.error = QString::fromStdString(error);
      break;
    }
    result.framesWritten = writer.framesWritten();
    if (!step(k + 1))
    {
      result.cancelled = true;
      break;
    }
  }

  result.csvPath = path + ".trajectory.csv";
  std::ofstream csv(result.csvPath.toStdString(), std::ios::binary | std::ios::trunc);
  csv << filter::trajectoryCsv(result.trajectory);
  if (!csv && result.error.isEmpty())
    result.error = QObject::tr("Could not write %1").arg(result.csvPath);
}

} // namespace

StabilizedExportResult exportStabilizedY4m(playlistItem                        *item,
                                           const QString                       &path,
                                           const StabilizedExportOptions       &options,
                                           const std::function<bool(int, int)> &progress)
{
  StabilizedExportResult result;
  me::YuvFrameLayout     layout;
  result.error = checkItem(item, layout);
  if (!result.error.isEmpty())
    return result;

  const auto range  = item->properties().startEndRange;
  const int  count  = range.second - range.first + 1;
  const int  w = layout.width, h = layout.height;
  const auto format = item->getRawPixelFormat();
  const auto size   = item->getRawFrameSize();
  result.framesTotal = count;

  const int  steps = 2 * count;
  const auto step  = [&](int done) { return !progress || progress(done, steps); };

  // Pass 1: inter-frame models.
  auto estimator = me::makeEstimator(me::Algorithm::SvtIntegerMe);
  if (!estimator)
  {
    result.error = QObject::tr("SVT integer ME is not available.");
    return result;
  }
  me::MeParams meParams;
  meParams.algorithm  = me::Algorithm::SvtIntegerMe;
  meParams.blockSizes = me::BlockSizeSet();
  meParams.blockSizes.add(me::BlockSize::Blk16);
  /* No static-block early exit. fitGlobalModel does count bypassed blocks as (0, 0), so a still
   * pair would fit either way; the search is kept on so that every point is a measured vector and
   * the result does not depend on SVT's bypass threshold (and matches the GM window's points).
   */
  meParams.staticBypass = false;
  me::GmModelParams fit;
  fit.type    = options.model;
  fit.fitSize = me::BlockSize::Blk16;

  result.models.assign(std::size_t(count), me::GmModelResult{});
  std::vector<filter::AffineParams> models(std::size_t(count), filter::kAffineIdentity);
  me::MePlane                       previous;
  for (int k = 0; k < count; ++k)
  {
    const auto raw = item->readRawFrame(range.first + k);
    auto       cur = lumaPlaneFromRawYuv(raw, format, size, me::kOdysseyPadFull);
    if (cur.empty())
    {
      result.error = QObject::tr("Could not read frame %1").arg(range.first + k);
      return result;
    }
    if (k > 0)
    {
      me::MePictureSet pictures;
      pictures.current     = cur;
      pictures.reference   = previous;
      pictures.frameIdx    = range.first + k;
      pictures.refFrameIdx = range.first + k - 1;
      pictures.refDistance = 1;
      const auto me        = estimator->estimateFrame(pictures, meParams);
      auto       model     = me.ok() ? me::fitGlobalModel(me, w, h, fit) : me::GmModelResult{};
      if (!me.ok() && model.error.empty())
        model.error = me.error.empty() ? "ME did not finish" : me.error;
      if (model.ok())
        models[std::size_t(k)] = model.p;
      else
        ++result.failedFits; // taken as no motion
      result.models[std::size_t(k)] = std::move(model);
    }
    previous = std::move(cur);
    if (!step(k + 1))
    {
      result.cancelled = true;
      return result;
    }
  }

  result.trajectory = filter::stabilize(models, w, h, options.smoothing);
  writeStabilized(item, path, layout, result, [&](int done) { return step(count + done); });
  return result;
}

StabilizedExportResult exportStabilizedY4m(playlistItem                              *item,
                                           const QString                             &path,
                                           const std::map<int, filter::AffineParams> &modelsByFrame,
                                           const filter::StabilizerParams            &smoothing,
                                           const std::function<bool(int, int)>       &progress)
{
  StabilizedExportResult result;
  me::YuvFrameLayout     layout;
  result.error = checkItem(item, layout);
  if (!result.error.isEmpty())
    return result;
  const auto range   = item->properties().startEndRange;
  const int  count   = range.second - range.first + 1;
  result.framesTotal = count;
  result.trajectory  = filter::stabilizeByFrame(modelsByFrame, range.first, range.second, layout.width,
                                                layout.height, smoothing);
  writeStabilized(item, path, layout, result, [&](int done) { return !progress || progress(done, count); });
  return result;
}

void exportStabilizedY4m(playlistItem *item, QWidget *parent)
{
  const QString title = QObject::tr("Export stabilized Y4M");
  const auto    tell  = [&](const QString &text) { QMessageBox::information(parent, title, text); };

  me::YuvFrameLayout layout;
  if (const auto why = checkItem(item, layout); !why.isEmpty())
  {
    tell(why);
    return;
  }

  StabilizedExportOptions options;
  {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto *form  = new QFormLayout(&dialog);
    auto *model = new QComboBox(&dialog);
    model->setObjectName("stabilizeModel");
    model->addItem(QObject::tr("Translation"), int(me::GmModelType::Translation));
    model->addItem(QObject::tr("Rotation + zoom"), int(me::GmModelType::RotZoom));
    model->setCurrentIndex(1);
    auto *method = new QComboBox(&dialog);
    method->setObjectName("stabilizeMethod");
    for (const auto m : {filter::SmoothingMethod::MovingAverage, filter::SmoothingMethod::Gaussian,
                         filter::SmoothingMethod::KalmanCausal, filter::SmoothingMethod::KalmanRts})
      method->addItem(QObject::tr(filter::smoothingMethodName(m)), int(m));
    method->setCurrentIndex(method->findData(int(options.smoothing.method)));
    method->setToolTip(QObject::tr("How the camera trajectory is smoothed. Moving average and Gaussian "
                                   "look ahead and behind; Kalman causal uses past frames only (as a "
                                   "real-time stabiliser would); Kalman RTS adds a backward pass."));
    auto *radius = new QSpinBox(&dialog);
    radius->setObjectName("stabilizeRadius");
    radius->setRange(0, 300);
    radius->setValue(options.smoothing.radius);
    radius->setSuffix(QObject::tr(" frames"));
    radius->setToolTip(QObject::tr("Moving-average radius of the camera trajectory: the window is "
                                   "2R + 1 frames. Larger keeps less of the camera motion."));
    auto *sigma = new QDoubleSpinBox(&dialog);
    sigma->setObjectName("stabilizeSigma");
    sigma->setRange(0.5, 100);
    sigma->setDecimals(1);
    sigma->setSingleStep(0.5);
    sigma->setValue(options.smoothing.sigma);
    sigma->setSuffix(QObject::tr(" frames"));
    sigma->setToolTip(QObject::tr("Standard deviation of the Gaussian window (it spans +-3 sigma). "
                                  "Larger keeps less of the camera motion."));
    auto *kalmanQ = new QDoubleSpinBox(&dialog);
    kalmanQ->setObjectName("stabilizeKalmanQ");
    kalmanQ->setRange(0.0001, 1000);
    kalmanQ->setDecimals(4);
    kalmanQ->setSingleStep(0.01);
    kalmanQ->setValue(options.smoothing.kalmanQ);
    kalmanQ->setToolTip(QObject::tr("Process noise: variance of the camera acceleration per frame. "
                                    "Smaller follows the camera less closely (smoother)."));
    auto *kalmanR = new QDoubleSpinBox(&dialog);
    kalmanR->setObjectName("stabilizeKalmanR");
    kalmanR->setRange(0.0001, 10000);
    kalmanR->setDecimals(4);
    kalmanR->setValue(options.smoothing.kalmanR);
    kalmanR->setToolTip(QObject::tr("Measurement noise: variance of the measured trajectory (px^2, "
                                    "deg^2 for the angle). Larger is smoother; only Q / R matters."));
    form->addRow(QObject::tr("Motion model"), model);
    form->addRow(QObject::tr("Smoothing"), method);
    form->addRow(QObject::tr("Smoothing radius"), radius);
    form->addRow(QObject::tr("Gaussian sigma"), sigma);
    form->addRow(QObject::tr("Kalman Q"), kalmanQ);
    form->addRow(QObject::tr("Kalman R"), kalmanR);
    const auto showRows = [=] {
      const auto m      = filter::SmoothingMethod(method->currentData().toInt());
      const bool kalman = m == filter::SmoothingMethod::KalmanCausal || m == filter::SmoothingMethod::KalmanRts;
      form->setRowVisible(radius, m == filter::SmoothingMethod::MovingAverage);
      form->setRowVisible(sigma, m == filter::SmoothingMethod::Gaussian);
      form->setRowVisible(kalmanQ, kalman);
      form->setRowVisible(kalmanR, kalman);
    };
    showRows();
    QObject::connect(method, &QComboBox::currentIndexChanged, &dialog, showRows);
    form->addRow(new QLabel(QObject::tr("SVT integer ME 16x16 + RANSAC global model per frame pair. "
                                        "Same size as the input, edges replicated, no crop."),
                            &dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
      return;
    options.model             = me::GmModelType(model->currentData().toInt());
    options.smoothing.method  = filter::SmoothingMethod(method->currentData().toInt());
    options.smoothing.radius  = radius->value();
    options.smoothing.sigma   = sigma->value();
    options.smoothing.kalmanQ = kalmanQ->value();
    options.smoothing.kalmanR = kalmanR->value();
  }

  const auto suggested = QFileInfo(item->properties().name).completeBaseName() + "_stabilized.y4m";
  const auto path =
      QFileDialog::getSaveFileName(parent, title, suggested, QObject::tr("Y4M video (*.y4m)"));
  if (path.isEmpty())
    return;

  const auto      range = item->properties().startEndRange;
  const int       count = range.second - range.first + 1;
  QProgressDialog progress(QObject::tr("Estimating camera motion, then writing stabilized frames..."),
                           QObject::tr("Cancel"), 0, 2 * count, parent);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(500);
  const auto result = exportStabilizedY4m(item, path, options, [&progress](int done, int) {
    progress.setValue(done);
    return !progress.wasCanceled();
  });
  progress.reset();

  const auto fits = result.failedFits > 0
                        ? QObject::tr(" %1 frame pairs had no model and were taken as still.").arg(result.failedFits)
                        : QString();
  if (!result.error.isEmpty())
    tell(QObject::tr("%1. %2 frames were written to %3.").arg(result.error).arg(result.framesWritten).arg(path));
  else if (result.cancelled)
    tell(QObject::tr("Cancelled. %1 of %2 frames were written to %3.")
             .arg(result.framesWritten)
             .arg(count)
             .arg(path));
  else
    tell(QObject::tr("Wrote %1 stabilized frames to %2 and the trajectory to %3.%4")
             .arg(result.framesWritten)
             .arg(path)
             .arg(result.csvPath)
             .arg(fits));
}

} // namespace bda::integration
