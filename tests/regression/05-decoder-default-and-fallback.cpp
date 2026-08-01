// Which decoder does playlistItemCompressedVideo's constructor actually pick, and does the item
// end up usable? Uses an isolated QSettings scope so the user's YUView.conf is untouched.
#include <QApplication>
#include <QSettings>
#include <iostream>
#include <unistd.h>
#include "playlistitem/playlistItemCompressedVideo.h"

class Probe : public playlistItemCompressedVideo
{
public:
  using playlistItemCompressedVideo::playlistItemCompressedVideo;
  decoder::DecoderEngine engine() const { return this->decoderEngine; }
  size_t                 nrStatTypes() { return this->statisticsData.getStatisticsTypes().size(); }
};

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear(); // no DefaultDecoderAV1 pinned -> exercise the code default

  // argv[2] (optional) is the decoder engine name this run must select.
  const std::string expected = argc > 2 ? argv[2] : "";

  Probe item(argv[1], 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);

  std::cout << "engine        : "
            << std::string(decoder::DecoderEngineMapper.getName(item.engine())) << std::endl;
  std::cout << "startEndRange : (" << item.properties().startEndRange.first << ", "
            << item.properties().startEndRange.second << ")" << std::endl;
  std::cout << "nr stat types : " << item.nrStatTypes() << std::endl;

  const std::string engine(decoder::DecoderEngineMapper.getName(item.engine()));
  bool              pass  = item.properties().startEndRange.second > 0;
  if (!pass)
    std::cout << "FAIL: no decodable frame range" << std::endl;
  if (!expected.empty() && engine != expected)
  {
    std::cout << "FAIL: expected engine " << expected << " but got " << engine << std::endl;
    pass = false;
  }
  std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(pass ? 0 : 1);
}
