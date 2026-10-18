#include "LcevcSeparateOpen.h"

#include <playlistitem/playlistItemCompressedVideo.h>

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>

namespace bda::integration
{

playlistItemCompressedVideo *openLcevcSeparate(QWidget *parent)
{
  QSettings  settings;
  const auto lastDir = settings.value("lastFilePath").toString();
  const auto base    = QFileDialog::getOpenFileName(
      parent,
      "Open LCEVC: base stream",
      lastDir,
      "Compressed video (*.h264 *.264 *.avc *.hevc *.h265 *.265 *.vvc *.266 *.ivf *.obu *.mp4 *.mkv *.ts);;"
      "All files (*)");
  if (base.isEmpty())
    return nullptr;
  const auto enhancement = QFileDialog::getOpenFileName(parent,
                                                        "Open LCEVC: enhancement (LCEVC NAL units)",
                                                        QFileInfo(base).absolutePath(),
                                                        "LCEVC enhancement (*.nal *.lvc);;All files (*)");
  if (enhancement.isEmpty())
    return nullptr;
  settings.setValue("lastFilePath", QFileInfo(base).absolutePath());

  auto *item = new playlistItemCompressedVideo(base);
  if (!item->getFrameHandler() || item->properties().startEndRange.second < 0)
  {
    QMessageBox::warning(parent, "Open LCEVC", "The base stream could not be opened:\n" + base);
    delete item;
    return nullptr;
  }
  QString error;
  if (!item->attachLcevcEnhancement(enhancement, error))
    QMessageBox::warning(parent, "Open LCEVC", "The enhancement was not attached; the base is shown alone.\n" + error);
  else if (!error.isEmpty())
    QMessageBox::information(parent, "Open LCEVC", error);
  return item;
}

} // namespace bda::integration
