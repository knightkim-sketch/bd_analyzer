// Headless per-superblock BD-rate export.
//
// Drives the same production classes the GUI's "SB BD-rate" button uses (BdRateGroups ->
// BdRateSequenceSweeper -> bdrate::bdRate) with no window, and writes one CSV row per superblock.
// Exists because the feature is GUI-only and batch attribution needs the numbers on disk.
//
//   sb-bdrate-export <org.y4m> <out.csv> [--raw raw.csv] --a <anchor...> --b <test...>
//
// --raw additionally dumps every (frame, superblock, group, point) sample, so the aggregation
// level is a choice made downstream rather than baked in here. Frames are walked in increasing
// order only: each stream then advances one frame at a time and never seeks backwards.
//
// The sweep accumulates every frame, so each superblock's curve is a sequence curve, not one frame.
#include <QApplication>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "integration/BdRateCollector.h"
#include "integration/BdRateGroups.h"
#include "ui/Mainwindow.h"
#include "ui/PlaybackController.h"
#include "ui/widgets/PlaylistTreeWidget.h"
#include "playlistitem/playlistItem.h"

namespace
{
void settle(int ms)
{
  for (int i = 0; i < ms / 10; ++i)
  {
    QCoreApplication::processEvents();
    usleep(10000);
  }
}

// Select exactly the playlist items whose path is in `want`, deselect everything else.
int selectOnly(PlaylistTreeWidget *tree, const QStringList &want)
{
  int hit = 0;
  for (int i = 0; i < tree->topLevelItemCount(); ++i)
  {
    auto *item = dynamic_cast<playlistItem *>(tree->topLevelItem(i));
    if (!item)
      continue;
    const auto name = item->properties().name;
    bool       on   = false;
    for (const auto &w : want)
      if (QFileInfo(name).canonicalFilePath() == QFileInfo(w).canonicalFilePath())
        on = true;
    item->setSelected(on);
    if (on)
      ++hit;
  }
  return hit;
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);

  QString     org, out, raw;
  QStringList a, b;
  {
    QStringList *bucket = nullptr;
    std::vector<QString> positional;
    for (int i = 1; i < argc; ++i)
    {
      const QString arg = argv[i];
      if (arg == "--raw")    { bucket = nullptr; if (i + 1 < argc) raw = argv[++i]; continue; }
      if (arg == "--a")      { bucket = &a; continue; }
      if (arg == "--b")      { bucket = &b; continue; }
      if (bucket)            { *bucket << arg; continue; }
      positional.push_back(arg);
    }
    if (positional.size() < 2 || a.size() < 2 || b.size() < 2)
    {
      std::cerr << "usage: sb-bdrate-export <org.y4m> <out.csv> --a <anchor...> --b <test...>\n";
      return 2;
    }
    org = positional[0];
    out = positional[1];
  }

  QCoreApplication::setOrganizationName("bdAnalyzerSbExport");
  QCoreApplication::setApplicationName("bdAnalyzerSbExport");
  QSettings().clear();
  QSettings().remove("Autosaveplaylist");

  auto *w = new MainWindow(false); // never deleted - see regression 21
  w->resize(1200, 800);
  w->show();
  settle(500);
  w->loadFiles(QStringList(a) << b << org);
  settle(8000);

  auto *tree = w->findChild<PlaylistTreeWidget *>();
  if (!tree) { std::cerr << "no playlist\n"; return 1; }

  std::vector<bda::integration::BdRateGroup> groups;
  for (auto *files : {&a, &b})
  {
    QStringList want = *files;
    want << org;
    const int hit = selectOnly(tree, want);
    settle(300);
    std::cout << "selected " << hit << " of " << want.size() << " items" << std::endl;
    const auto r = bda::integration::makeBdRateGroup(tree->getAllSelectedItems(), groups);
    if (!r.ok())
    {
      std::cerr << "group refused: " << r.message.toStdString() << std::endl;
      return 1;
    }
    std::cout << "group '" << r.group.name.toStdString() << "'  points=" << r.group.points.size()
              << "  grid=" << r.group.frameSize.width << "x" << r.group.frameSize.height
              << "  sb=" << r.group.superblockSize << std::endl;
    groups.push_back(r.group);
  }

  if (!raw.isEmpty())
  {
    auto *playback = w->findChild<PlaybackController *>();
    int   nFrames  = 0;
    for (const auto &g : groups)
      for (const auto &p : g.points)
        if (p.item)
          nFrames = std::max(nFrames, int(p.item->properties().startEndRange.second) + 1);
    std::cout << "raw dump over " << nFrames << " frames" << std::endl;

    std::ofstream rf(raw.toStdString());
    rf << "frame,sb_x,sb_y,group,point,bits,sse,samples\n";
    for (int fi = 0; fi < nFrames; ++fi)
    {
      if (playback)
        playback->setCurrentFrameAndUpdate(fi);
      bda::integration::BdRateFrameData d;
      for (int attempt = 0; attempt < 120; ++attempt)
      {
        d = bda::integration::collectBdRateFrame(groups, fi);
        if (!d.error.isEmpty() || !d.pending)
          break;
        settle(100);
      }
      if (!d.error.isEmpty())
      {
        std::cerr << "frame " << fi << ": " << d.error.toStdString() << std::endl;
        return 1;
      }
      for (const auto &[key, cell] : d.perSuperblock)
        for (std::size_t g = 0; g < cell.size(); ++g)
          for (std::size_t p = 0; p < cell[g].size(); ++p)
          {
            const auto &s = cell[g][p];
            if (s.sampleCount <= 0)
              continue;
            rf << fi << ',' << key.first << ',' << key.second << ',' << g << ',' << p << ','
               << s.bits << ',' << s.sse << ',' << s.sampleCount << '\n';
          }
      if (fi % 10 == 0)
        std::cout << "  raw frame " << fi << "/" << nFrames << std::endl;
    }
    rf.close();
    std::cout << "wrote " << raw.toStdString() << std::endl;
  }

  bda::integration::BdRateSequenceSweeper sweeper;
  bool finished = false;
  QObject::connect(&sweeper, &bda::integration::BdRateSequenceSweeper::finished,
                   [&finished]() { finished = true; });
  sweeper.start(groups);
  for (int i = 0; i < 36000 && !finished; ++i)
  {
    settle(100);
    if (i % 100 == 0)
      std::cout << "  sweep " << int(sweeper.progress() * 100) << "%  "
                << sweeper.statusText().toStdString() << std::endl;
  }
  if (!finished) { std::cerr << "sweep did not finish\n"; return 1; }

  const auto &seq = sweeper.data();
  if (!seq.usable()) { std::cerr << "sweep unusable: " << seq.error.toStdString() << "\n"; return 1; }
  std::cout << "frames " << seq.firstFrame << ".." << seq.lastFrame
            << "  superblocks " << seq.perSuperblock.size() << std::endl;

  // Sequence totals, as a cross-check against the clip-level BD-rate measured elsewhere.
  {
    const auto ca = bda::integration::curveFor(seq.totals.at(0));
    const auto cb = bda::integration::curveFor(seq.totals.at(1));
    const auto r  = bda::bdrate::bdRate(ca, cb);
    std::cout << "SEQUENCE BD-rate = " << r.percent << " %  (status " << int(r.status)
              << ", degree " << r.degree << ", overlap " << r.psnrOverlapLow << ".."
              << r.psnrOverlapHigh << " dB)" << std::endl;
  }

  std::ofstream f(out.toStdString());
  f << "sb_x,sb_y,status,bdrate,degree,psnr_lo,psnr_hi,n_a,n_b,"
       "bits_a_max,bits_b_max,psnr_a_max,psnr_b_max,samples\n";
  int ok = 0, bad = 0;
  for (const auto &[key, cell] : seq.perSuperblock)
  {
    if (cell.size() < 2) continue;
    const auto ca = bda::integration::curveFor(cell.at(0));
    const auto cb = bda::integration::curveFor(cell.at(1));
    const auto r  = bda::bdrate::bdRate(ca, cb);
    auto maxRate = [](const std::vector<bda::bdrate::RatePoint> &c) {
      double m = 0; for (const auto &p : c) m = std::max(m, p.rate); return m; };
    auto maxPsnr = [](const std::vector<bda::bdrate::RatePoint> &c) {
      double m = 0; for (const auto &p : c) m = std::max(m, p.psnr); return m; };
    std::int64_t samples = 0;
    for (const auto &s : cell.at(0)) if (s.sampleCount > samples) samples = s.sampleCount;
    f << key.first << ',' << key.second << ',' << int(r.status) << ','
      << (r.ok() ? r.percent : 0.0) << ',' << r.degree << ','
      << r.psnrOverlapLow << ',' << r.psnrOverlapHigh << ','
      << ca.size() << ',' << cb.size() << ','
      << maxRate(ca) << ',' << maxRate(cb) << ','
      << maxPsnr(ca) << ',' << maxPsnr(cb) << ',' << samples << '\n';
    r.ok() ? ++ok : ++bad;
  }
  f.close();
  std::cout << "wrote " << out.toStdString() << "  ok=" << ok << " unusable=" << bad << std::endl;
  std::cout.flush();
  _exit(0);
}
