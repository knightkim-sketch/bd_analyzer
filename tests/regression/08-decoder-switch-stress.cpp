// Regression: switching the decoder engine repeatedly while frames are being decoded must not
// crash or hang.
//
// Drives the real production path — the actual "comboBoxDecoder" QComboBox, whose
// currentIndexChanged runs playlistItemCompressedVideo::decoderComboxBoxChanged() — from the main
// thread, while a worker thread keeps calling loadFrame() like the loading/caching threads do.
//
// Before the fixes this failed in three different ways, all observed:
//   * SIGSEGV in loadRawData(): allocateDecoder() reset the decoders with no synchronisation.
//   * SIGSEGV in decoderFFmpeg::pushAVPacket(): decoderEngine was updated before the decoders, so
//     a worker saw the new engine with the old decoder and dereferenced a failed dynamic_cast.
//   * Hang: loadRawData() spun forever on an exhausted input while holding the decoder mutex.
// The test deliberately references nothing that the patches add, so it also compiles (and fails)
// against pristine upstream.
#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QThread>
#include <atomic>
#include <iostream>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"

static playlistItemCompressedVideo *g_item = nullptr;
static std::atomic<bool>            g_stop{false};
static std::atomic<int>             g_loads{0};

class Worker : public QThread
{
  void run() override
  {
    int f = 0;
    while (!g_stop.load())
    {
      g_item->loadFrame(f % 26, false, true, false);
      g_loads++;
      f++;
    }
  }
};

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 08-decoder-switch-stress <av1 file>" << std::endl;
    return 2;
  }
  // Keep the user's real YUView configuration out of this.
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  playlistItemCompressedVideo item(argv[1], 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  g_item = &item;

  auto *combo = item.getPropertiesWidget()->findChild<QComboBox *>("comboBoxDecoder");
  if (!combo || combo->count() < 2)
  {
    std::cout << "comboBoxDecoder missing or has fewer than 2 entries" << std::endl;
    _exit(2);
  }
  std::cout << "combo entries  :";
  for (int i = 0; i < combo->count(); ++i)
    std::cout << " [" << i << "]" << combo->itemText(i).toStdString();
  std::cout << std::endl;

  Worker w;
  w.start();

  const int N = 40;
  for (int i = 0; i < N; ++i)
  {
    combo->setCurrentIndex(i % 2); // fires decoderComboxBoxChanged()
    QThread::msleep(20);
  }

  g_stop = true;
  w.wait();

  std::cout << "switches       : " << N << std::endl;
  std::cout << "worker loads   : " << g_loads.load() << std::endl;

  // Reaching this line at all is the test: before the fixes the process crashed or hung here.
  const bool pass = g_loads.load() > 0;
  if (!pass)
    std::cout << "FAIL: the worker never loaded a frame" << std::endl;
  std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  // Skip local destructors: tearing down two FFmpegVersionHandler owners is not what this test
  // covers, and the order here is not the one the application uses.
  _exit(pass ? 0 : 1);
}
