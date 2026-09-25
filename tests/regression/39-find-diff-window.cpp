// Regression: the Find diff window compares two selected streams without a display.
//
// The window is layer A of the Find diff design - sequence and frame header syntax, compared OBU
// by OBU. Everything it does apart from the comparison is drawing, so this drives the comparison
// and the refusals; the drawing is left to the manual checklist.
//
// The refusals matter as much as the comparison. This action is a deliberate menu press, so "two
// streams, please" has to come back as a sentence rather than as an empty window.
#include <QApplication>
#include <QEventLoop>
#include <QSettings>
#include <QTimer>

#include <iostream>
#include <string>

#include "integration/StreamDiffWindow.h"
#include "playlistitem/playlistItemCompressedVideo.h"

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++failures;
}

// The comparison runs on its own thread and reports back through a queued signal, so the test has
// to let the event loop run. A timeout rather than a wait: a hang must fail, not stall the suite.
bool waitForResult(bda::integration::StreamDiffWindow &window, int timeoutMs)
{
  QEventLoop loop;
  QTimer     poll;
  QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
  QObject::connect(&poll, &QTimer::timeout, [&] {
    if (!window.busy())
      loop.quit();
  });
  poll.start(50);
  loop.exec();
  return !window.busy();
}
} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 3)
  {
    std::cerr << "usage: 39-find-diff-window <streamA> <streamB>\n";
    return 2;
  }

  QCoreApplication::setOrganizationName("bdAnalyzerFindDiff");
  QCoreApplication::setApplicationName("bdAnalyzerFindDiff");
  QSettings().clear();

  playlistItemCompressedVideo itemA(QString::fromUtf8(argv[1]), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);
  playlistItemCompressedVideo itemB(QString::fromUtf8(argv[2]), 0, InputFormat::Libav,
                                    decoder::DecoderEngine::Invalid);

  bda::integration::StreamDiffWindow window;

  {
    const auto r = window.compare({});
    check(!r.ok(), "an empty selection is refused");
    check(!r.message.isEmpty(), "and the refusal says why");
  }
  {
    const auto r = window.compare({&itemA});
    check(!r.ok(), "one stream is refused");
  }
  {
    const auto r = window.compare({&itemA, &itemA});
    check(!r.ok(), "the same file twice is refused");
  }

  {
    const auto r = window.compare({&itemA, &itemB});
    check(r.ok(), "two different streams are accepted");
    check(waitForResult(window, 120000), "the comparison finishes");

    const auto &result = window.lastResult();
    check(!result.sections.empty(), "OBUs were compared");
    // Two encodes of the same source at different quality: the headers must differ somewhere, or
    // the comparison is not looking at anything.
    check(!result.identical(), "two different encodes report differences");
    const auto *first = result.firstDiffering();
    check(first != nullptr, "a first differing OBU is named");
    if (first)
      std::cout << "        first divergence: " << first->label << std::endl;
  }

  std::cout << "RESULT: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
  return failures == 0 ? 0 : 1;
}
