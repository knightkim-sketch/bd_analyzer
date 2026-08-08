// Regression: the statistics overlay list in the properties panel shows one row per *group* of
// statistics types, not one per type.
//
// Three types answer "how is this block predicted" (the prediction mode plus the luma and chroma
// intra modes) and two pairs are per reference list (motion vectors, reference indices). They are
// separate types on purpose - each keeps its own colour map, and the block syntax table lists them
// all - but a row each made the list too long to take in. One row now drives every type of its
// group, which is what this pins down: a group must switch all of its members and nothing else.
//
// Also checks the order: the superblock level values come first, because they are what one looks at
// to find where the bitrate went before drilling into per block modes.
#include <QApplication>
#include <QCheckBox>
#include <QSettings>
#include <iostream>
#include <unistd.h>

#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

class Probe : public playlistItemCompressedVideo
{
public:
  using playlistItemCompressedVideo::playlistItemCompressedVideo;
  stats::StatisticsTypesVec &types() { return this->statisticsData.getStatisticsTypes(); }
  stats::StatisticUIHandler &handler() { return this->statisticsUIHandler; }
};
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 2)
  {
    std::cerr << "usage: 16-statistics-ui-grouping <av1 file in a container>" << std::endl;
    return 2;
  }
  QCoreApplication::setOrganizationName("bdAnalyzerRegressionTest");
  QCoreApplication::setApplicationName("bdAnalyzerRegressionTest");
  QSettings().clear();

  Probe item(argv[1], 0, InputFormat::Libav, decoder::DecoderEngine::Invalid);

  auto *layout = item.handler().createStatisticsHandlerControls();
  if (!layout)
  {
    std::cout << "SKIP: no statistics controls (no analyzer decoder?)" << std::endl;
    std::cout.flush();
    _exit(0);
  }
  auto *host = new QWidget;
  host->setLayout(layout);

  const auto boxes = host->findChildren<QCheckBox *>();
  std::cout << "check boxes: " << boxes.size() << " for " << item.types().size() << " types"
            << std::endl;
  QStringList labels;
  for (auto *b : boxes)
    labels << b->text();
  std::cout << "  order: " << labels.join(", ").toStdString() << std::endl;

  check(!boxes.isEmpty(), "the controls were created");
  check(boxes.size() < int(item.types().size()),
        "there are fewer rows than types (grouping is in effect)");
  check(labels.value(0) == "sb_qindex", "the first row is sb_qindex");
  check(labels.value(1) == "sb_bitcount", "the second row is sb_bitcount");
  // The grouped rows are labelled with the group, so the per member names must be gone.
  check(!labels.contains("intra pred mode (Y)") && !labels.contains("intra pred mode (UV)"),
        "the intra prediction modes do not get rows of their own");
  check(labels.contains("Pred Mode"), "the prediction group has a row");
  check(!labels.contains("Motion Vector 0") && labels.contains("Motion Vector"),
        "the motion vectors share one row");
  check(!labels.contains("ref frame index 0") && labels.contains("ref frame index"),
        "the reference indices share one row");

  const auto setChecked = [&boxes](const QString &label, bool on) {
    for (auto *b : boxes)
      if (b->text() == label)
      {
        b->setChecked(on);
        return true;
      }
    return false;
  };
  const auto renderOf = [&item](const QString &name) {
    for (const auto &t : item.types())
      if (t.typeName == name)
        return t.render ? 1 : 0;
    return -1;
  };
  const auto allRender = [&renderOf](std::initializer_list<const char *> names, int expected) {
    for (const auto *n : names)
      if (renderOf(QString(n)) != expected)
        return false;
    return true;
  };

  // Everything starts off, so a group switching on can only come from its row.
  check(allRender({"Pred Mode", "intra pred mode (Y)", "intra pred mode (UV)"}, 0),
        "the prediction types start off");

  setChecked("Pred Mode", true);
  check(allRender({"Pred Mode", "intra pred mode (Y)", "intra pred mode (UV)"}, 1),
        "one row switches on all three prediction types");
  check(allRender({"Motion Vector 0", "Motion Vector 1", "sb_qindex"}, 0),
        "and leaves the other groups alone");

  setChecked("Motion Vector", true);
  check(allRender({"Motion Vector 0", "Motion Vector 1"}, 1),
        "one row switches on both motion vector types");

  setChecked("ref frame index", true);
  check(allRender({"ref frame index 0", "ref frame index 1"}, 1),
        "one row switches on both reference index types");

  setChecked("Pred Mode", false);
  check(allRender({"Pred Mode", "intra pred mode (Y)", "intra pred mode (UV)"}, 0),
        "switching the row off switches all three back off");
  check(allRender({"Motion Vector 0", "Motion Vector 1"}, 1),
        "without disturbing the other groups");

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  std::cout.flush();
  _exit(g_failures == 0 ? 0 : 1);
}
