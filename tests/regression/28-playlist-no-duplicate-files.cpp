// Regression: the same file must not sit in the playlist twice.
//
// Easy to arrange and never useful: the playlist is restored on start (see
// 21-playlist-saved-across-sessions) and the command line then names a file that is already in it,
// so every start added another copy of whatever you opened last.
//
// What this pins down:
//   * Loading through the window drops the copy and selects the original - opening a file that is
//     already open should take you to it, not list it again.
//   * Two spellings of one path are one file. canonicalFilePath() resolves them; a file that no
//     longer exists has no canonical path and must not collapse onto every other missing file.
//   * The snapshot taken on quit is deduplicated, so a session that collected copies some other way
//     does not hand them to the next session, which would restore them and collect more.
//   * Different files are left alone. This is not "one item per playlist".
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <iostream>
#include <string>
#include <unistd.h>

#include "ui/Mainwindow.h"
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
void checkEq(long long got, long long want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (got " << got << ", want " << want << ")";
  std::cout << std::endl;
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

// Any raw YUV will do; the name carries the resolution so the format is guessed without a dialog.
QString writeClip(const QString &name, int w, int h, int frames, char fill)
{
  const QString path = QDir::tempPath() + "/" + name;
  QFile         f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return {};
  const QByteArray frame(w * h + 2 * (w / 2) * (h / 2), fill);
  for (int n = 0; n < frames; ++n)
    f.write(frame);
  f.close();
  return path;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();
  QSettings().setValue("BDCache/directory", "/proc/bd-analyzer-no-such-cache");
  QSettings().remove("Autosaveplaylist");

  const auto clipA = writeClip("bd-dup-a_64x64_yuv420p.yuv", 64, 64, 4, '\x40');
  const auto clipB = writeClip("bd-dup-b_64x64_yuv420p.yuv", 64, 64, 4, '\x60');
  check(!clipA.isEmpty() && !clipB.isEmpty(), "the two clips were written");
  if (clipA.isEmpty() || clipB.isEmpty())
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }

  // The same file named a second way: through a directory that resolves back to the same place.
  const auto clipAIndirect = QDir::tempPath() + "/./" + QFileInfo(clipA).fileName();

  /* --- the load path ---------------------------------------------------------------------------
   *
   * This is the startup path: YUViewApplication hands the command line to MainWindow::loadFiles
   * after the playlist has been restored.
   */
  auto *w = new MainWindow(false);
  w->show();
  settle(500);
  w->loadFiles({clipA, clipA, clipB, clipAIndirect});
  settle(2500);

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  check(tree != nullptr, "the playlist tree is there");
  if (!tree)
  {
    std::cout << "FAIL" << std::endl;
    return 1;
  }
  checkEq(tree->topLevelItemCount(), 2, "one item per distinct file, spellings folded together");

  {
    const auto items = tree->getAllPlaylistItems();
    check(items.size() == 2 && items[0]->properties().name == clipA,
          "and the first copy is the one that survived");
    check(items.size() == 2 && items[1]->properties().name == clipB,
          "with the other file untouched - this is not one item per playlist");
  }

  // Opening a file that is already open selects it rather than adding it.
  w->loadFiles({clipB});
  settle(1500);
  checkEq(tree->topLevelItemCount(), 2, "re-opening an open file adds nothing");
  {
    const auto selected = tree->getSelectedItems();
    check(selected[0] != nullptr && selected[0]->properties().name == clipB,
          "and lands the selection on the file that was asked for");
  }

  /* --- the snapshot ----------------------------------------------------------------------------
   *
   * Loaded straight into the widget, which is the path that does not deduplicate - it stands in for
   * a session that collected copies some other way. Quitting must not hand them to the next start.
   */
  tree->loadFiles({clipA});
  settle(1500);
  checkEq(tree->topLevelItemCount(), 3, "the widget's own load path still adds the copy");

  w->close();
  settle(500);
  check(QSettings().contains("Autosaveplaylist"), "closing takes the snapshot");

  {
    // See 21-playlist-saved-across-sessions on why the window that holds items is the one closed.
    auto *w2 = new MainWindow(false);
    w2->show();
    settle(2500);
    auto *tree2 = w2->findChild<PlaylistTreeWidget *>();
    checkEq(tree2 ? tree2->topLevelItemCount() : -1,
            2,
            "and the next start comes up without the copy");
  }

  QFile::remove(clipA);
  QFile::remove(clipB);
  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
