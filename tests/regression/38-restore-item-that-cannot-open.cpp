// Regression: restoring a playlist entry whose file cannot be opened crashed at startup.
//
// playlistItemCompressedVideo's constructor gives up on a file it cannot open - libavcodec
// refusing it, an unknown raw format, no frame size, no usable pixel format - and returns before
// creating its video handler. newPlaylistItemCompressedVideo() then called
//
//     newFile->video->loadPlaylist(root);
//
// straight through the null unique_ptr. Measured: SIGSEGV at newPlaylistItemCompressedVideo+0x26c,
// "segfault at 0".
//
// What made it worse than a failed open is where it happens. The item lives in the autosaved
// playlist, so the crash repeated on every start and the only way out was editing the settings by
// hand. An AV1 stream whose sequence header breaks conformance is exactly such a file - which is
// the kind of stream this tool exists to look at.
#include <QApplication>
#include <QDomDocument>
#include <QSettings>

#include <iostream>
#include <string>

#include "common/YUViewDomElement.h"
#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++failures;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 38-restore-item-that-cannot-open <file-that-cannot-be-opened>\n";
    return 2;
  }
  const QString path = QString::fromUtf8(argv[1]);

  QCoreApplication::setOrganizationName("bdAnalyzerRestoreGuard");
  QCoreApplication::setApplicationName("bdAnalyzerRestoreGuard");
  QSettings().clear();

  // The premise: this file really is one the constructor cannot finish. If it ever starts opening,
  // the test below stops proving anything, so assert the premise rather than assuming it.
  {
    playlistItemCompressedVideo item(path, 0, InputFormat::Libav, decoder::DecoderEngine::FFMpeg);
    check(item.getSize().width() <= 0, "the fixture is a file the constructor cannot open");
  }

  QDomDocument doc;
  auto         root = doc.createElement("playlistItemCompressedVideo");
  doc.appendChild(root);
  auto addChild = [&doc, &root](const QString &name, const QString &value) {
    auto e = doc.createElement(name);
    e.appendChild(doc.createTextNode(value));
    root.appendChild(e);
  };
  addChild("absolutePath", "file://" + path);
  addChild("relativePath", path);
  addChild("displayComponent", "0");
  addChild("inputFormat", "Libav");
  addChild("decoder", "FFMpeg");

  // Reaching the next line at all is most of the test: before the fix this dereferenced a null
  // unique_ptr and the process died here.
  auto *item = playlistItemCompressedVideo::newPlaylistItemCompressedVideo(YUViewDomElement(root),
                                                                          QString());
  check(item == nullptr, "restoring it yields no item instead of crashing");
  delete item;

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
  return failures == 0 ? 0 : 1;
}
