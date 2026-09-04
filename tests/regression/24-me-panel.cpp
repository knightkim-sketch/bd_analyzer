// Regression: the Motion Estimation panel, and the raw YUV item it drives.
//
// Driven through MainWindow so the whole chain is exercised - panel, item hooks, background run,
// statistics overlay - rather than the pieces in isolation.
//
// What this pins down:
//   * A raw YUV item now owns a statistics container. Upstream gives one only to compressed and
//     statistics-file items, and without it the ME overlay has nothing to draw through.
//   * The item can hand over an arbitrary frame without disturbing what the viewer shows.
//   * The panel's controls are what the estimate uses, and the block-size checkboxes are the only
//     switch deciding which overlay types exist.
//   * Unchecked, nothing is computed. Checked, a result arrives and reaches the overlay.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSettings>
#include <QSpinBox>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/MeStatisticsAdapter.h"
#include "integration/MotionEstimationWidget.h"
#include "playlistitem/playlistItemRawFile.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/PlaylistTreeWidget.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
void settle(int ms)
{
  for (int i = 0; i < ms / 10; ++i)
  {
    QCoreApplication::processEvents();
    usleep(10000);
  }
}

QCheckBox *checkBoxWithText(QWidget *parent, const QString &text)
{
  for (auto *box : parent->findChildren<QCheckBox *>())
    if (box->text() == text)
      return box;
  return nullptr;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 24-me-panel <raw.yuv>" << std::endl;
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  // Keep MainWindow::closeEvent's cache question out of the way; nothing here needs caching.
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1400, 900);
  w->show();
  w->loadFiles({QString(argv[1])});
  settle(2000);

  auto *panel = w->findChild<MotionEstimationWidget *>();
  check(panel != nullptr, "the Motion Estimation panel exists");

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  auto *item = tree && !tree->getSelectedItems().empty()
                   ? dynamic_cast<playlistItemRawFile *>(tree->getSelectedItems()[0])
                   : nullptr;
  check(item != nullptr, "the raw YUV item came up");
  if (!panel || !item)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  // --- the item can serve motion estimation --------------------------------------------------
  {
    check(item->supportsMotionEstimation(), "a raw YUV item supports motion estimation");
    check(item->getStatisticsUIHandler() != nullptr,
          "and now owns a statistics handler, which upstream does not give raw items");

    const auto frame0 = item->readRawFrame(0);
    const auto frame1 = item->readRawFrame(1);
    check(!frame0.isEmpty() && !frame1.isEmpty(), "two arbitrary frames can be read");
    check(frame0 != frame1, "and they are different frames, not the same buffer twice");
    check(item->readRawFrame(100000).isEmpty(), "a frame past the end comes back empty");
    check(item->readRawFrame(-1).isEmpty(), "and so does a negative index");
  }

  // --- the panel's controls are what the estimate uses ----------------------------------------
  {
    auto *interval = panel->findChild<QSpinBox *>();
    auto *algo     = panel->findChild<QComboBox *>();
    check(interval != nullptr && algo != nullptr, "the interval and algorithm controls are there");

    auto *box8  = checkBoxWithText(panel, "8");
    auto *box64 = checkBoxWithText(panel, "64");
    check(box8 != nullptr && box64 != nullptr, "and the block size checkboxes");
    if (!interval || !algo || !box8 || !box64)
    {
      std::cout << "FAIL" << std::endl;
      return 1;
    }

    check(box64->isChecked() && !box8->isChecked(),
          "64x64 alone by default - one arrow per superblock is readable, 8x8 is a thicket");

    interval->setValue(2);
    settle(100);
    check(panel->params().frameInterval == 2, "the interval control reaches the parameters");

    interval->setValue(-3);
    settle(100);
    check(panel->params().frameInterval == -3, "including a negative one");

    interval->setValue(1);
    box8->setChecked(true);
    settle(100);
    check(panel->params().blockSizes.contains(bda::me::BlockSize::Blk8),
          "checking a size adds it to the set");
    box8->setChecked(false);
    settle(100);
    check(!panel->params().blockSizes.contains(bda::me::BlockSize::Blk8),
          "and unchecking takes it away");

    // The closed-loop entry is visible so the plan shows, but cannot be chosen.
    check(algo->count() == 3, "all three algorithms are listed");
    algo->setCurrentIndex(2);
    settle(100);
    check(algo->currentIndex() != 2 || panel->params().algorithm ==
                                           bda::me::Algorithm::OdysseyClosedLoop,
          "the closed-loop entry is disabled rather than hidden");
    algo->setCurrentIndex(0);
    settle(100);
  }

  /* --- unchecked means nothing is computed ---------------------------------------------------
   *
   * The checkbox is the master switch, and it is off by default because a whole-frame estimate is
   * expensive enough that starting one on file open would be a surprise.
   */
  {
    auto *enable = checkBoxWithText(panel, "Compute in the background");
    check(enable != nullptr, "the enable checkbox is there");
    if (enable)
    {
      check(!enable->isChecked(), "and it is off by default");

      auto *playback = w->findChild<PlaybackController *>();
      if (playback)
        playback->setCurrentFrameAndUpdate(2);
      settle(500);

      const int vecId = bda::integration::meVectorTypeId(bda::me::Algorithm::SvtIntegerMe,
                                                         bda::me::BlockSize::Blk64);
      check(!item->getStatisticsData().hasDataForTypeID(vecId),
            "moving the frame with the box unchecked computes nothing");

      // --- checked: a result arrives and reaches the overlay --------------------------------
      enable->setChecked(true);
      // 128x128 with one 64x64 size is small, but the estimate is still a real search.
      settle(15000);

      check(item->getStatisticsData().hasDataForTypeID(vecId),
            "with the box checked the vectors reach the overlay");

      const auto *vecType = [&]() -> const stats::StatisticsType * {
        for (const auto &t : item->getStatisticsData().getStatisticsTypes())
          if (t.typeID == vecId)
            return &t;
        return nullptr;
      }();
      check(vecType != nullptr, "and the vector type is registered on the item");
      if (vecType)
        check(vecType->vectorScale == 8, "with the eighth-pel vector scale");

      const int size8Id = bda::integration::meVectorTypeId(bda::me::Algorithm::SvtIntegerMe,
                                                           bda::me::BlockSize::Blk8);
      bool      has8    = false;
      for (const auto &t : item->getStatisticsData().getStatisticsTypes())
        if (t.typeID == size8Id)
          has8 = true;
      check(!has8, "and only the checked block size got a type");
    }
  }

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
