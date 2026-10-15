#include "FilteredY4mExport.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>

#include <cmath>
#include <string>

#include <playlistitem/playlistItem.h>

#include "MeFrameSource.h"
#include "filter/Y4mWriter.h"

namespace bda::integration
{

void exportFilteredY4m(playlistItem *item, QWidget *parent)
{
  const QString title = QObject::tr("Export filtered Y4M");
  const auto    tell  = [&](const QString &text) { QMessageBox::information(parent, title, text); };

  if (!item || !item->supportsMotionEstimation())
  {
    tell(QObject::tr("Select an item with a raw source: a raw YUV file, or a stream with its "
                     "original YUV attached."));
    return;
  }

  me::YuvFrameLayout layout;
  QString            why;
  if (!yuvLayoutFromRawFormat(item->getRawPixelFormat(), item->getRawFrameSize(), layout, &why))
  {
    tell(why);
    return;
  }
  const bool mono = layout.subX == 0;
  if (layout.bigEndian || layout.interleavedUV || (!mono && (layout.subX != 2 || layout.subY != 2)) ||
      (layout.bitDepth != 8 && layout.bitDepth != 10))
  {
    tell(QObject::tr("Y4M export takes planar 4:2:0 or 4:0:0, 8 or 10 bit, little endian. This "
                     "item's format is %1.")
             .arg(QString::fromStdString(item->getRawPixelFormat().getName())));
    return;
  }

  const auto range = item->properties().startEndRange;
  if (range.first < 0 || range.second < range.first)
  {
    tell(QObject::tr("The item has no frames to export."));
    return;
  }

  const auto suggested = QFileInfo(item->properties().name).completeBaseName() + "_filtered.y4m";
  const auto path      = QFileDialog::getSaveFileName(
      parent, title, suggested, QObject::tr("Y4M video (*.y4m)"));
  if (path.isEmpty())
    return;

  filter::Y4mFormat format;
  format.width    = layout.width;
  format.height   = layout.height;
  format.bitDepth = layout.bitDepth;
  format.mono     = mono;
  // Y4M wants a ratio; a thousandth of a frame per second is finer than any rate in use.
  const auto fps        = item->properties().frameRate;
  format.fpsNumerator   = fps > 0 ? int(std::lround(fps * 1000)) : 30000;
  format.fpsDenominator = 1000;

  filter::Y4mWriter writer;
  std::string       error;
  if (!writer.open(path.toStdString(), format, error))
  {
    tell(QString::fromStdString(error));
    return;
  }

  const int       count = range.second - range.first + 1;
  QProgressDialog progress(QObject::tr("Writing filtered frames..."), QObject::tr("Cancel"), 0, count, parent);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(500);

  const auto lumaEnd   = layout.offset[0] + format.lumaBytes();
  const auto chromaEnd = std::max(layout.offset[1], layout.offset[2]) + format.chromaBytes();
  const auto needed    = mono ? lumaEnd : std::max(lumaEnd, chromaEnd);
  bool       failed    = false;
  for (int i = 0; i < count && !progress.wasCanceled(); i++)
  {
    const auto frame = item->readFilteredRawFrame(range.first + i);
    if (frame.size() < needed)
    {
      error  = "Could not read frame " + std::to_string(range.first + i);
      failed = true;
      break;
    }
    const auto *data = reinterpret_cast<const std::uint8_t *>(frame.constData());
    if (!writer.writeFrame(
            data + layout.offset[0], data + layout.offset[1], data + layout.offset[2], error))
    {
      failed = true;
      break;
    }
    progress.setValue(i + 1);
  }
  progress.reset();

  const auto written = writer.framesWritten();
  if (failed)
    tell(QObject::tr("%1. %2 frames were written to %3.")
             .arg(QString::fromStdString(error))
             .arg(written)
             .arg(path));
  else if (written < count)
    tell(QObject::tr("Cancelled after %1 of %2 frames. The file holds the frames written so far: %3")
             .arg(written)
             .arg(count)
             .arg(path));
  else
    tell(QObject::tr("Wrote %1 filtered frames to %2.").arg(written).arg(path));
}

} // namespace bda::integration
