// Regression: a raw AV1 stream (no container) must open and behave like the same stream in IVF.
//
// libavformat's "obu" demuxer reads these files and the item decodes them fine, but ".av1"/".obu"
// were missing from playlistItemCompressedVideo::getSupportedFileExtensions(). That list is a hard
// gate in guessFileTypeFromFileAndCreatePlaylistItem(), so dropping such a file on YUView did
// nothing at all - no item, no error.
#include <QApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <iostream>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

bool extensionIsSupported(const QString &extension)
{
  QStringList allExtensions, filters;
  playlistItemCompressedVideo::getSupportedFileExtensions(allExtensions, filters);
  return allExtensions.contains(extension);
}

// Hash the decoded pixels of every frame, so two containers of the same stream can be compared.
QByteArray pixelHash(const QString &path, int &frames)
{
  playlistItemCompressedVideo item(path, 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);
  const auto                  last = item.properties().startEndRange.second;
  const auto                  size = item.getSize();
  QCryptographicHash          hash(QCryptographicHash::Md5);
  frames = 0;
  for (int f = 0; f <= last; ++f)
  {
    item.loadFrame(f, false, true, false);
    for (int y = 0; y < size.height(); y += 5)
      for (int x = 0; x < size.width(); x += 5)
        for (const auto &set : item.getPixelValues(QPoint(x, y), f))
          for (const auto &pair : set.second)
            hash.addData(pair.second.toUtf8());
    ++frames;
  }
  return hash.result().toHex();
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3)
  {
    std::cerr << "usage: 15-raw-av1-extension <ivf file> <raw av1/obu file>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  /* This is the list guessFileTypeFromFileAndCreatePlaylistItem() matches the file suffix against.
   * Calling that function here is not an option: when it finds no match it falls back to a modal
   * "what kind of file is this" dialog, which would hang a headless run.
   */
  check(extensionIsSupported("ivf"), "\"ivf\" is a supported extension");
  check(extensionIsSupported("av1"), "\"av1\" is a supported extension");
  check(extensionIsSupported("obu"), "\"obu\" is a supported extension");

  int  ivfFrames = 0, rawFrames = 0;
  const auto ivfHash = pixelHash(argv[1], ivfFrames);
  const auto rawHash = pixelHash(argv[2], rawFrames);
  std::cout << "  ivf : " << ivfFrames << " frames, " << ivfHash.toStdString() << std::endl;
  std::cout << "  raw : " << rawFrames << " frames, " << rawHash.toStdString() << std::endl;

  check(rawFrames == ivfFrames, "the raw stream reports the same frame count");
  check(rawHash == ivfHash, "the raw stream decodes to the same pixels as the IVF");

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
