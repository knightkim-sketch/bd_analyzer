// Reproduce: restoring a playlist entry whose file cannot be opened.
//
// playlistItemCompressedVideo's constructor has five early returns - libavcodec refusing the file,
// an unknown raw format, no frame size, no pixel format - and `video` (a unique_ptr) is only
// created after all of them. newPlaylistItemCompressedVideo() then calls
//
//     newFile->video->loadPlaylist(root);
//
// with no check, so any of those failures turns a restore into a null dereference at startup. That
// is fatal in a way an open failure is not: the item is in the autosaved playlist, so the app
// crashes every time it starts until the playlist is cleared by hand.
#include <QApplication>
#include <QDomDocument>
#include <QSettings>

#include <iostream>

#include "common/YUViewDomElement.h"
#include "playlistitem/playlistItemCompressedVideo.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: repro-restore-null-video <stream-that-cannot-be-opened>\n";
    return 2;
  }
  const QString path = QString::fromUtf8(argv[1]);

  QCoreApplication::setOrganizationName("bdAnalyzerReproRestore");
  QCoreApplication::setApplicationName("bdAnalyzerReproRestore");
  QSettings().clear();

  // First: does the constructor actually leave the item without a video handler?
  {
    playlistItemCompressedVideo item(path, 0, InputFormat::Libav, decoder::DecoderEngine::FFMpeg);
    const auto size = item.getSize();
    std::cout << "direct construction: size " << size.width() << "x" << size.height() << "\n";
    std::cout << "                     " << (size.width() <= 0 ? "no usable video handler"
                                                               : "opened fine")
              << std::endl;
  }

  // Then the restore path itself, with the smallest element it accepts.
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

  std::cout << "calling newPlaylistItemCompressedVideo() - this is where startup dies"
            << std::endl;
  auto *item = playlistItemCompressedVideo::newPlaylistItemCompressedVideo(
      YUViewDomElement(root), QString());
  std::cout << "returned " << (item ? "an item" : "nullptr") << " - no crash" << std::endl;
  std::cout.flush();
  _exit(0);
}
