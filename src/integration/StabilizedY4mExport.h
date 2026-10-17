// File > Export stabilized Y4M: digital image stabilisation of the selected item, written as
// YUV4MPEG2 with its camera trajectory as CSV.
//
// Part of the src/integration boundary, modelled on FilteredY4mExport (same format checks, Y4M
// writer, cancellable progress). Two passes over the item's raw frames:
//   1. for each frame k >= 1: SVT integer ME of frame k against k-1 (16x16 points), then a RANSAC
//      global model (bda::me::fitGlobalModel, Translation or RotZoom); a failed fit counts as no
//      motion. bda::filter::stabilize turns the models into a smoothed trajectory and per-frame
//      correction warps.
//   2. each frame warped (bilinear, border replicate, same size, no crop) and written; luma and
//      chroma with the same correction, chroma taken as centre-sited (C420jpeg, what the writer
//      declares - see bda::filter::ChromaSiting).
// The trajectory (raw, smoothed, correction) goes to <output>.trajectory.csv.
// Design: docs/ai/30-designs/ransac-global-model-dis-design.md section 4.
#pragma once

#include <QString>

#include <functional>
#include <vector>

#include "filter/Stabilizer.h"
#include "me/GlobalModel.h"

class QWidget;
class playlistItem;

namespace bda::integration
{

struct StabilizedExportOptions
{
  me::GmModelType model  = me::GmModelType::RotZoom; //!< Translation or RotZoom
  int             radius = 15;                       //!< Moving-average radius, frames
};

struct StabilizedExportResult
{
  QString error; //!< Empty on success (and on cancel)
  bool    cancelled{};
  int     framesTotal{};
  int     framesWritten{};
  int     failedFits{}; //!< Frames whose model could not be fitted and were taken as no motion
  QString csvPath;
  std::vector<filter::StabilizerFrame> trajectory; //!< One entry per frame of the range
  std::vector<me::GmModelResult>       models;     //!< Index k: frame k -> k-1 (index 0 unused)

  bool ok() const { return this->error.isEmpty() && !this->cancelled; }
};

/* The whole export without any dialog, for tests and scripts: writes `path` and
 * `path`.trajectory.csv. `progress(done, total)` is called after every step of both passes and
 * cancels the export by returning false.
 */
StabilizedExportResult exportStabilizedY4m(playlistItem                            *item,
                                           const QString                           &path,
                                           const StabilizedExportOptions           &options,
                                           const std::function<bool(int, int)>     &progress = {});

/* The menu entry: checks the item (message box when it cannot be exported, as the filtered
 * export does), asks for the options and a file name, runs with a progress dialog and reports.
 */
void exportStabilizedY4m(playlistItem *item, QWidget *parent);

} // namespace bda::integration
