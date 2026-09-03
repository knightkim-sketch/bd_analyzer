// Regression: opening a raw YUV file whose name carries no resolution and whose size matches no
// guess. The frame size stays invalid, so FrameHandler::getFormatAsString() returns nothing - and
// videoHandlerYUV::getFormatAsString() used to dereference that empty optional. That built a
// std::string from a garbage length and aborted with std::bad_alloc inside the
// playlistItemRawFile constructor, before the item was ever shown:
//
//   operator new(unsigned long)                 <- bad_alloc
//   videoHandlerYUV::getFormatAsString() const
//   playlistItemRawFile::playlistItemRawFile(...)
//   playlistItems::createPlaylistItemFromFile(...)
//   PlaylistTreeWidget::loadFiles(...)
//
// videoHandlerRGB::getFormatAsString() already guarded the same optional; only the YUV override
// was missing the check.
//
// What this pins down:
//   * A handler with no frame size reports "no format" rather than aborting or returning garbage.
//   * Building a raw file item with an invalid size survives, which is the path that crashed.
//   * Not crashing is not enough: once a size is known the format string has to appear and be
//     well formed, so the user can still set the resolution and use the file.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSize>
#include <iostream>
#include <string>

#include "playlistitem/playlistItemRawFile.h"
#include "video/yuv/videoHandlerYUV.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

// video is protected on playlistItemWithVideo.
class Probe : public playlistItemRawFile
{
public:
  using playlistItemRawFile::playlistItemRawFile;
  video::videoHandler *handler() { return this->video.get(); }
};
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  // A size that matches no plausible YUV geometry, under a name that carries no hints. This is
  // exactly what reached the crash: 176x144 4:2:0 needs 38016 bytes per frame.
  const QString path = QDir::tempPath() + "/bd-raw-yuv-unknown-format.yuv";
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      std::cout << "  FAIL  could not create " << path.toStdString() << std::endl;
      return 1;
    }
    f.write(QByteArray(1000, '\x80'));
    f.close();
  }

  // --- the handler on its own ---------------------------------------------------------------
  {
    video::yuv::videoHandlerYUV handler;
    check(!handler.isFormatValid(), "a fresh YUV handler has no valid frame size");
    check(!handler.getFormatAsString().has_value(),
          "and reports no format string rather than dereferencing an empty optional");
  }

  // --- the path that aborted ----------------------------------------------------------------
  {
    Probe item(path, QSize(), "YUV 4:2:0 8-bit");
    check(true, "constructing a raw YUV item with an invalid size does not abort");

    auto *handler = item.handler();
    check(handler != nullptr, "the item has a video handler");
    check(handler && !handler->isFormatValid(), "the handler still reports the format as invalid");
    check(handler && !handler->getFormatAsString().has_value(),
          "and the item reports no format string");
  }

  // --- still usable once the size is known --------------------------------------------------
  {
    Probe item(path, QSize(176, 144), "YUV 4:2:0 8-bit");
    auto *handler = item.handler();
    check(handler && handler->isFormatValid(), "a sized item reports a valid format");

    const auto format = handler ? handler->getFormatAsString() : std::nullopt;
    check(format.has_value(), "a sized item reports a format string");
    check(format && format->rfind("176;144;YUV;", 0) == 0,
          "the format string carries the size and the raw format: " + format.value_or("<none>"));
  }

  QFile::remove(path);
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
