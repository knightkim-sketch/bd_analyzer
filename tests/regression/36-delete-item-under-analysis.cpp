// Regression: deleting a playlist item that something else is still holding on to.
//
// Two crashes, both from the same gesture - select a stream, press delete:
//
//   * The bitstream analysis widget tears its parser down when the selection empties, but the
//     parser's streamInfoUpdated is emitted from its background thread and therefore arrives
//     queued. The call that was already in the event queue then ran against a parser that no longer
//     existed: updateStreamInfo() dereferenced a null unique_ptr where every other slot on that
//     signal checked first. SIGSEGV on deleting the item being analysed.
//
//   * A BD-rate group held its streams by raw pointer while its own comment said "the user can
//     delete it while the plot window is open ... every use re-checks it". A raw pointer cannot be
//     re-checked: the item is destroyed (deleteLater, so it takes an event loop iteration), the
//     pointer stays non-null, and the next collection - which happens on every frame change - read
//     freed memory.
//
// The test processes DeferredDelete explicitly, because a run without QEventLoop::exec() never
// destroys the item and the use-after-free hides.
#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QSettings>
#include <iostream>
#include <string>
#include <unistd.h>

#include "integration/BdRateCollector.h"
#include "integration/BdRateGroups.h"
#include "playlistitem/playlistItemCompressedVideo.h"
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
    // What QEventLoop::exec() does and processEvents() does not: run deleteLater().
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    usleep(10000);
  }
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 4)
  {
    std::cout << "SKIP: needs <org.y4m> <a.ivf> <b.ivf> [c.ivf]" << std::endl;
    std::cout.flush();
    _exit(0);
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().remove("Autosaveplaylist");

  const QString org = argv[1];
  QStringList   streams;
  for (int i = 2; i < argc; ++i)
    streams << argv[i];

  auto *w = new MainWindow(false); // never deleted - see 21-playlist-saved-across-sessions
  w->resize(1200, 800);
  w->show();
  settle(400);
  w->loadFiles(QStringList(streams) << org);
  settle(6000);

  auto *tree     = w->findChild<PlaylistTreeWidget *>();
  auto *playback = w->findChild<PlaybackController *>();
  check(tree != nullptr, "the playlist came up");
  if (!tree)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  QList<playlistItem *> selection;
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
    selection << dynamic_cast<playlistItem *>(tree->topLevelItem(i));

  std::vector<bda::integration::BdRateGroup> groups;
  const auto result = bda::integration::makeBdRateGroup(selection, groups);
  check(result.ok(), "the streams become a group: " + result.message.toStdString());
  if (!result.ok())
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }
  groups.push_back(result.group);

  if (playback)
    playback->setCurrentFrameAndUpdate(2);
  settle(1500);

  // The victim is the item the analysis widget is parsing, so both paths are exercised at once.
  auto *victim = groups.front().points[1].item.data();
  QPointer<playlistItem> watch(victim);
  tree->clearSelection();
  victim->setSelected(true);
  tree->setCurrentItem(victim);
  w->currentSelectedItemsChanged(victim, nullptr);
  settle(1500); // let the background parser start and emit

  tree->deletePlaylistItems(false);
  settle(3000); // the queued stream info update lands in here, and so does the deleteLater

  check(watch.isNull(), "the item was destroyed");
  check(groups.front().points[1].item.isNull(),
        "the group's pointer to it went null with it, rather than dangling");

  // Would be a use-after-free if the pointer had stayed.
  const auto data = bda::integration::collectBdRateFrame(groups, 2);
  std::cout << "  collect says: \"" << data.error.toStdString() << "\"" << std::endl;
  check(!data.error.isEmpty(),
        "and collecting reports the missing stream instead of decoding through it");

  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
