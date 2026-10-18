// Regression: "base + enhancement" input (File > Open LCEVC, patch 0076).
//
// The hardware encoder writes its LCEVC output as a base stream plus enh.nal (the LCEVC NAL units, one
// per picture in output order) and enh.nal.sizes. The same enhancement muxed into one stream is
// hw_encoder_h264.lvc, so its LTM reference MD5s apply here too:
//   * the base as Annex-B H.264 with enh.nal attached decodes, under LTM and LCEVCdec, to the
//     reference, at the enhanced size;
//   * the base in MP4 - FFmpeg demuxes and decodes it, packets go through the LCEVC decoder to the
//     FFmpeg base decoder - gives the same;
//   * a saved playlist entry reopens with the enhancement and the engine it had;
//   * an enhancement file without LCEVC data is refused and leaves the base as it was.
//
// args: <base .h264> <enh.nal> <md5 list> <base .mp4 or "-">
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QSettings>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "video/videoHandler.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << std::endl;
  if (!condition)
    ++failures;
}

std::vector<std::string> readEnhancedMd5(const std::string &path)
{
  std::vector<std::string> md5s;
  std::ifstream            f(path);
  std::string              line;
  while (std::getline(f, line))
    if (!line.empty() && line[0] != '#')
    {
      std::istringstream s(line);
      int                idx;
      std::string        e;
      s >> idx >> e;
      md5s.push_back(e);
    }
  return md5s;
}

std::string decodedMd5(playlistItemCompressedVideo &item, int frame)
{
  QMetaObject::invokeMethod(&item, "loadRawData", Qt::DirectConnection, Q_ARG(int, frame), Q_ARG(bool, false));
  auto *video = dynamic_cast<video::videoHandler *>(item.getFrameHandler());
  if (!video || video->rawData_frameIndex != frame)
    return "-";
  return QCryptographicHash::hash(video->rawData, QCryptographicHash::Md5).toHex().toStdString();
}

int matching(playlistItemCompressedVideo &item, const std::vector<std::string> &ref)
{
  int same = 0;
  for (int f = 0; f < int(ref.size()); ++f)
    same += decodedMd5(item, f) == ref[std::size_t(f)];
  return same;
}

std::string engineOf(const playlistItemCompressedVideo &item)
{
  QDomDocument doc;
  auto         root = doc.createElement("root");
  item.savePlaylist(root, QDir("/"));
  return root.firstChildElement().firstChildElement("decoder").text().toStdString();
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear();
  if (argc < 5)
  {
    std::cerr << "usage: 56-lcevc-separate-files <base.h264> <enh.nal> <md5 list> <base.mp4|->\n";
    return 2;
  }
  const QString base = argv[1], enhancement = argv[2];
  const auto    ref  = readEnhancedMd5(argv[3]);
  const int     n    = int(ref.size());

  {
    std::cout << "== Annex-B base + enh.nal ==" << std::endl;
    playlistItemCompressedVideo item(base);
    const auto baseSize = item.getFrameHandler()->getFrameSize();
    QString    error;
    const bool attached = item.attachLcevcEnhancement(enhancement, error);
    check(attached && error.isEmpty(), "enh.nal attaches, no warning (" + error.toStdString() + ")");
    const auto size = item.getFrameHandler()->getFrameSize();
    check(baseSize.width == 256 && size.width == 512 && size.height == 288, "the item grows from 256x144 to 512x288");
    check(engineOf(item) == "LCEVC (LTM, analysis)", "and decodes under LTM");
    const auto ltm = matching(item, ref);
    check(ltm == n, "LTM: every frame equals ModelDecoder (" + std::to_string(ltm) + "/" + std::to_string(n) + ")");
    check(item.getFrameInfo(0).items.size() > 5, "the Frame tab has the picture configuration");

    // Saved and reopened - with LCEVCdec, to see that the engine travels with it.
    {
      playlistItemCompressedVideo second(base);
      second.attachLcevcEnhancement(enhancement, error, decoder::DecoderEngine::LcevcDec);
      QDomDocument doc;
      auto         root = doc.createElement("playlist");
      second.savePlaylist(root, QDir("/"));
      std::unique_ptr<playlistItemCompressedVideo> reopened(playlistItemCompressedVideo::newPlaylistItemCompressedVideo(
          YUViewDomElement(root.firstChildElement()), "/playlist.yuvplaylist"));
      check(reopened && engineOf(*reopened) == "LCEVC (LCEVCdec)", "a saved playlist entry reopens under LCEVCdec");
      const auto dec = reopened ? matching(*reopened, ref) : 0;
      check(dec == n, "LCEVCdec after reopening: every frame equals ModelDecoder (" + std::to_string(dec) + "/" +
                          std::to_string(n) + ")");
    }
  }

  if (std::string(argv[4]) != "-")
  {
    std::cout << "== MP4 base (FFmpeg demux + decode) + enh.nal ==" << std::endl;
    playlistItemCompressedVideo item(argv[4]);
    QString                     error;
    check(item.attachLcevcEnhancement(enhancement, error), "enh.nal attaches to the MP4 base");
    const auto same = matching(item, ref);
    check(same == n, "LTM over FFmpeg packets: every frame equals ModelDecoder (" + std::to_string(same) + "/" +
                         std::to_string(n) + ")");
    const auto again = decodedMd5(item, 0) == ref[0] && decodedMd5(item, n - 1) == ref[std::size_t(n - 1)];
    check(again, "and after seeking back to the start");
  }
  else
    std::cout << "  skip MP4 base (no ffmpeg to make it)" << std::endl;

  {
    std::cout << "== not an enhancement ==" << std::endl;
    playlistItemCompressedVideo item(base);
    QString                     error;
    const bool attached = item.attachLcevcEnhancement(base, error);
    check(!attached && !error.isEmpty(), "the base stream itself is refused as enhancement (" + error.toStdString() + ")");
    check(item.getFrameHandler()->getFrameSize().width == 256 && engineOf(item) == "FFMpeg",
          "and the item stays the base under FFmpeg");
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
