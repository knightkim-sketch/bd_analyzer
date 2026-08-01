// Drive the REAL production path: create the properties widget, find the actual
// "comboBoxDecoder" QComboBox and change its index. That emits currentIndexChanged, which runs
// playlistItemCompressedVideo::decoderComboxBoxChanged() exactly as the GUI does — while a worker
// thread keeps loading frames, like the loading/caching threads do.
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
  QCoreApplication::setOrganizationName("bdProbeOrg");
  QCoreApplication::setApplicationName("bdProbeApp");
  QSettings().clear();

  playlistItemCompressedVideo item(argv[1], 0, InputFormat::Libav,
                                   decoder::DecoderEngine::Invalid);
  g_item = &item;

  auto *props = item.getPropertiesWidget();
  auto *combo = props->findChild<QComboBox *>("comboBoxDecoder");
  if (!combo) { std::cout << "comboBoxDecoder not found" << std::endl; _exit(2); }

  std::cout << "combo entries  :";
  for (int i = 0; i < combo->count(); ++i)
    std::cout << " [" << i << "]" << combo->itemText(i).toStdString();
  std::cout << std::endl;
  std::cout << "current index  : " << combo->currentIndex() << " ("
            << combo->currentText().toStdString() << ")" << std::endl;

  Worker w;
  w.start();
  QThread::msleep(800);

  const int target = combo->findText("FFMpeg");
  std::cout << "switching combo to index " << target << " (FFMpeg) ..." << std::endl;
  std::cout.flush();
  combo->setCurrentIndex(target);          // <-- fires the real slot
  QThread::msleep(2500);

  g_stop = true;
  w.wait();
  const auto after = combo->currentText().toStdString();
  std::cout << "combo now      : " << after << std::endl;
  std::cout << "worker loads   : " << g_loads.load() << std::endl;

  bool pass = true;
  if (after != "FFMpeg")
  {
    std::cout << "FAIL: the decoder did not switch" << std::endl;
    pass = false;
  }
  if (g_loads.load() == 0)
  {
    std::cout << "FAIL: the worker never loaded a frame" << std::endl;
    pass = false;
  }
  std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(pass ? 0 : 1);
}
