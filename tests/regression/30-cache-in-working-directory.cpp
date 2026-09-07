// Regression: the derived-data cache goes in the working directory, and nowhere else.
//
// Decoded YUV and per-block statistics run to gigabytes, so where they land is not a detail. This
// used to be the executable's directory with a fallback to the user cache directory, and both are
// wrong for how the analyzer is deployed: from an RPM the binary sits under a root-owned /opt, the
// write probe there always failed, and every run silently ended up in ~/.cache - a path nobody
// looks at, on a partition that is small on these machines.
//
// So it follows the user: start the analyzer on the volume that has the room and the cache is
// there, next to the material being analysed.
//
// What this pins down:
//   * The root is <cwd>/.bd_analyzer, and it moves when the working directory does.
//   * A working directory that cannot be written to means no caching - not a guess at some other
//     location. Callers already treat an empty root that way.
//   * The "BDCache/directory" setting still overrides it, which is how a machine with one big
//     volume can pin the cache there regardless of where the app is started.
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <iostream>
#include <string>
#include <cstdio>
#include <unistd.h>

#include "common/BDCachePaths.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}
} // namespace

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  /* BDCachePaths::root() resolves once per process and keeps the answer, so one process can only
   * observe one outcome. The cases are therefore driven as child processes, each with its own
   * working directory - which is also how the real thing is used.
   */
  if (argc > 1 && std::string(argv[1]) == "--print-root")
  {
    if (argc > 2)
      QSettings().setValue("BDCache/directory", QString(argv[2]));
    std::cout << BDCachePaths::root().toStdString() << std::endl;
    return 0;
  }

  const QString self = QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath();
  const auto    rootIn = [&self](const QString &workingDir, const QString &configured = {}) {
    QString out;
    // popen rather than QProcess: this test links the library, not the Qt process machinery, and
    // a shell is the shortest way to say "run this over there".
    QString cmd = "cd '" + workingDir + "' && '" + self + "' --print-root";
    if (!configured.isEmpty())
      cmd += " '" + configured + "'";
    cmd += " 2>/dev/null";
    if (auto *pipe = popen(cmd.toLocal8Bit().constData(), "r"))
    {
      char buffer[4096]{};
      if (std::fgets(buffer, sizeof(buffer), pipe))
        out = QString::fromLocal8Bit(buffer).trimmed();
      pclose(pipe);
    }
    return out;
  };

  const auto base = QDir::tempPath() + "/bd-cache-cwd-test";
  QDir().mkpath(base + "/one");
  QDir().mkpath(base + "/two");

  // --- it follows the working directory --------------------------------------------------------
  {
    const auto one = rootIn(base + "/one");
    const auto two = rootIn(base + "/two");
    check(one == base + "/one/.bd_analyzer", "started in one/: " + one.toStdString());
    check(two == base + "/two/.bd_analyzer", "started in two/: " + two.toStdString());
    check(one != two, "so two runs from two places do not share a cache");
    check(QFileInfo(one).isDir(), "and the directory is created");
  }

  /* --- and not from the binary's location ------------------------------------------------------
   *
   * The regression itself: the executable is somewhere else entirely, and nothing may appear next
   * to it.
   */
  {
    const auto exeDir = QFileInfo(self).absolutePath();
    check(!QFileInfo(exeDir + "/.bd_analyzer").exists(),
          "nothing is created next to the executable");
  }

  // --- an unwritable working directory means no caching ----------------------------------------
  {
    // /proc is mounted read-only for mkdir, which is exactly the shape of a read-only install.
    const auto none = rootIn("/proc");
    check(none.isEmpty(), "an unwritable working directory gives an empty root, not a fallback");
  }

  // --- the setting still wins -------------------------------------------------------------------
  {
    const auto pinned = base + "/pinned";
    QDir().mkpath(pinned);
    const auto root = rootIn(base + "/one", pinned);
    check(root == pinned, "BDCache/directory overrides the working directory: " +
                              root.toStdString());
  }

  QDir(base).removeRecursively();
  QSettings().clear();
  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
