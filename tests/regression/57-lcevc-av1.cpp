// Regression: AV1 base + LCEVC in metadata OBUs (ITU-T T.35), the stream lcevc-av1-mux writes.
//
// tests/data/lcevc/testsrc2_av1_lcevc.ivf: 512x288 testsrc2, 8 frames. AV1 base 256x144 by libaom;
// the enhancement by LTM's ModelEncoder against the dav1d decode of that base (H.264 carrier recipe);
// lcevc-av1-mux put one metadata OBU per temporal unit. The reference is LTM's ModelDecoder over the
// same external base, dithering off.
//   * the bitstream analysis shows every metadata OBU as "Metadata (LCEVC)" with its LCEVC syntax;
//   * the item offers the LCEVC engines over dav1d, opens at 512x288 and every frame equals the
//     reference under LTM; backward seeks too. LCEVCdec gives every picture with LTM's luma (its
//     chroma differs by design, see below);
//   * the Base signal is the dav1d picture.
//
// args: <ivf> <md5 list>
#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QSettings>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"
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

void countNames(const QAbstractItemModel &model, const QModelIndex &index, int &lcevcObus, int &globalConfigs)
{
  const auto name = model.data(model.index(index.row(), 0, index.parent())).toString().toStdString();
  // The OBU rows only: a packet row lists the names of its OBUs too.
  lcevcObus += name.rfind("OBU", 0) == 0 && name.find("Metadata (LCEVC)") != std::string::npos;
  globalConfigs += name == "global_config";
  for (int row = 0; row < model.rowCount(index); ++row)
    countNames(model, model.index(row, 0, index), lcevcObus, globalConfigs);
}

QByteArray decodeFrame(playlistItemCompressedVideo &item, int frame)
{
  QMetaObject::invokeMethod(&item, "loadRawData", Qt::DirectConnection, Q_ARG(int, frame), Q_ARG(bool, false));
  auto *video = dynamic_cast<video::videoHandler *>(item.getFrameHandler());
  return video && video->rawData_frameIndex == frame ? video->rawData : QByteArray();
}

std::string md5(const QByteArray &data)
{
  return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex().toStdString();
}

QByteArray halve420(const QByteArray &frame, int width, int height)
{
  QByteArray out;
  const auto *p = reinterpret_cast<const unsigned char *>(frame.constData());
  for (int plane = 0; plane < 3; ++plane)
  {
    const int w = plane ? width / 2 : width, h = plane ? height / 2 : height;
    for (int y = 0; y < h; y += 2)
      for (int x = 0; x < w; x += 2)
        out.append(char(p[y * w + x]));
    p += w * h;
  }
  return out;
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear();
  if (argc < 3)
  {
    std::cerr << "usage: 57-lcevc-av1 <ivf> <md5 list>\n";
    return 2;
  }
  std::vector<std::string> enhanced, base;
  {
    std::ifstream f(argv[2]);
    std::string   line;
    while (std::getline(f, line))
      if (!line.empty() && line[0] != '#')
      {
        std::istringstream s(line);
        int                idx;
        std::string        e, b;
        s >> idx >> e >> b;
        enhanced.push_back(e);
        base.push_back(b);
      }
  }
  const int n = int(enhanced.size());

  {
    FFmpeg::FFmpegVersionHandler ff;
    ff.loadFFmpegLibraries();
    parser::ParserAVFormat parser;
    parser.enableModel();
    check(parser.runParsingOfFile(std::filesystem::path(argv[1])), "the bitstream analysis parses the IVF");
    parser.updateNumberModelItems();
    int   lcevcObus = 0, globalConfigs = 0;
    auto *model     = parser.getPacketItemModel();
    for (int row = 0; model && row < model->rowCount(); ++row)
      countNames(*model, model->index(row, 0), lcevcObus, globalConfigs);
    check(lcevcObus == n && globalConfigs >= 1,
          "every temporal unit has a Metadata (LCEVC) OBU (" + std::to_string(lcevcObus) + "/" + std::to_string(n) +
              "), with its global_config");
  }

  playlistItemCompressedVideo item(argv[1]);
  auto *props   = item.getPropertiesWidget();
  auto *decoder = props->findChild<QComboBox *>("comboBoxDecoder");
  auto *signal  = props->findChild<QComboBox *>("comboBoxDisplaySignal");
  check(decoder && decoder->findText("Dav1d") >= 0 && decoder->findText("LCEVC (LTM, analysis)") >= 0 &&
            decoder->currentText() == "LCEVC (LTM, analysis)",
        "dav1d and the LCEVC engines are offered, LTM is selected");
  const auto size = item.getFrameHandler()->getFrameSize();
  check(size.width == 512 && size.height == 288, "the picture has the enhanced size 512x288");

  auto countMatching = [&](const std::vector<std::string> &ref, bool halve) {
    int same = 0;
    for (int f = 0; f < n; ++f)
    {
      const auto frame = decodeFrame(item, f);
      same += md5(halve ? halve420(frame, 512, 288) : frame) == ref[std::size_t(f)];
    }
    return same;
  };
  auto same = countMatching(enhanced, false);
  check(same == n, "LTM over dav1d: every frame equals ModelDecoder (" + std::to_string(same) + "/" + std::to_string(n) + ")");
  same = 0;
  for (const int f : {n - 1, 2, 0})
    same += md5(decodeFrame(item, f)) == enhanced[std::size_t(f)];
  check(same == 3, "and out of order");

  signal->setCurrentIndex(signal->findText("Base"));
  same = countMatching(base, true);
  check(same == n, "the Base signal is the dav1d picture (" + std::to_string(same) + "/" + std::to_string(n) + ")");
  signal->setCurrentIndex(signal->findText("Enhanced"));

  /* LCEVCdec: every picture comes out (it used to stall at picture 6 reading LTM's temporal chunk as
   * bitstream version 2), and luma equals LTM. Chroma does not: LCEVCdec clamps each upscaling pass
   * to S15 and testsrc2's colour bars overshoot there, LTM (and the DIS) do not clamp.
   */
  std::vector<QByteArray> ltmLuma;
  decoder->setCurrentIndex(decoder->findText("LCEVC (LTM, analysis)"));
  for (int f = 0; f < n; ++f)
    ltmLuma.push_back(decodeFrame(item, f).left(512 * 288));
  decoder->setCurrentIndex(decoder->findText("LCEVC (LCEVCdec)"));
  int delivered = 0, lumaSame = 0;
  for (int f = 0; f < n; ++f)
  {
    const auto frame = decodeFrame(item, f);
    delivered += frame.size() == 512 * 288 * 3 / 2;
    lumaSame += !frame.isEmpty() && frame.left(512 * 288) == ltmLuma[std::size_t(f)];
  }
  check(delivered == n && lumaSame == n, "LCEVCdec over dav1d: every picture, luma equal to LTM (" +
                                             std::to_string(lumaSame) + "/" + std::to_string(n) + ")");

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
