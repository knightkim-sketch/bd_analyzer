// Regression: the Frame tab and double-click seek for H.264 (patch 0079, FrameAvcSummary).
//
// One libx264 encode with B-frames (-bf 3 -g 10), once as MP4 and once as the Annex-B stream inside
// it. The reference comes from ffprobe on the MP4: the frames in presentation order give the picture
// type of every display frame, the packets in file order give the coding order. Checked for both the
// Annex-B parser model (one row per NAL unit) and the container model (one row per AVPacket):
//   * every display frame's coding_order and slice type equal ffprobe's, POC rises in display order;
//   * the row of each picture's slice NAL unit maps back to that display frame (the seek lookup);
//   * the Frame tab shows those values and hides the AV1-only rows.
// The container's "YUView frame index" row is a decode-order packet index; the summary must not use it.
//
// args: <Annex-B .h264> <.mp4> <ffprobe packet pts csv> <ffprobe frame pts,pict_type csv>
#include <QAbstractItemModel>
#include <QApplication>
#include <QLabel>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "integration/FrameAvcSummary.h"
#include "parser/AVC/ParserAnnexBAVC.h"
#include "parser/AVFormat/ParserAVFormat.h"
#include "ui/widgets/FileInfoWidget.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << std::endl;
  if (!condition)
    ++failures;
}

struct Reference
{
  std::vector<int>         codingOrder; //!< Per display frame
  std::vector<std::string> pictType;    //!< Per display frame
};

Reference readReference(const std::string &packetsCsv, const std::string &framesCsv)
{
  std::vector<long> packetPts;
  std::ifstream     packets(packetsCsv);
  for (std::string line; std::getline(packets, line);)
    if (!line.empty())
      packetPts.push_back(std::stol(line));
  std::vector<std::pair<long, std::string>> frames;
  std::ifstream                             framesFile(framesCsv);
  for (std::string line; std::getline(framesFile, line);)
  {
    const auto comma = line.find(',');
    if (comma != std::string::npos)
      frames.emplace_back(std::stol(line.substr(0, comma)), line.substr(comma + 1, 1));
  }
  std::sort(frames.begin(), frames.end());
  Reference r;
  for (const auto &[pts, type] : frames)
  {
    r.codingOrder.push_back(int(std::find(packetPts.begin(), packetPts.end(), pts) - packetPts.begin()));
    r.pictType.push_back(type);
  }
  return r;
}

void checkModel(const std::string &what, QAbstractItemModel &model, const Reference &ref)
{
  std::cout << "== " << what << " ==" << std::endl;
  bda::integration::FrameAvcIndex index;
  index.update(model);
  const int n = int(ref.codingOrder.size());
  check(index.isAvc() && index.parsedFrames() == n,
        "pictures found: " + std::to_string(index.parsedFrames()) + " / " + std::to_string(n));
  int sameOrder = 0, sameType = 0, rising = 0, seekBack = 0;
  for (int d = 0; d < n; ++d)
  {
    const auto *p = index.frame(d);
    if (!p)
      continue;
    sameOrder += p->codingOrder == ref.codingOrder[std::size_t(d)];
    sameType += p->sliceTypes.size() == 1 && p->sliceTypes[0] == ref.pictType[std::size_t(d)];
    rising += d == 0 || (index.frame(d - 1) && index.frame(d - 1)->poc < p->poc);
    const int top   = p->packetRow >= 0 ? p->packetRow : p->nalRows.front();
    const int child = p->packetRow >= 0 ? p->nalRows.front() : -1;
    seekBack += index.displayIndexOf(top, child) == d;
  }
  check(sameOrder == n, "coding_order equals ffprobe's packet order (" + std::to_string(sameOrder) + "/" + std::to_string(n) + ")");
  check(sameType == n, "slice type equals ffprobe's pict_type (" + std::to_string(sameType) + "/" + std::to_string(n) + ")");
  check(rising == n, "POC rises in display order");
  check(seekBack == n, "each picture's NAL row seeks to its display frame (" + std::to_string(seekBack) + "/" + std::to_string(n) + ")");

  FileInfoWidget widget;
  widget.setObuModel(&model);
  const int d = 1; // a B picture, coded after the P it precedes
  widget.setCurrentFrame(d);
  const auto text = [&widget](const char *key) {
    auto *label = widget.findChild<QLabel *>(QString("frameInfo:%1").arg(key));
    return label ? label->text().toStdString() : std::string("?");
  };
  check(text("coding_order") == std::to_string(ref.codingOrder[std::size_t(d)]) &&
            text("slice_type").rfind(ref.pictType[std::size_t(d)], 0) == 0 && text("POC") == std::to_string(index.frame(d)->poc),
        "Frame tab, display frame 1: coding_order " + text("coding_order") + ", slice_type " + text("slice_type") + ", POC " + text("POC"));
  auto *orderHint = widget.findChild<QLabel *>("frameInfo:order_hint");
  auto *poc       = widget.findChild<QLabel *>("frameInfo:POC");
  check(orderHint && poc && orderHint->isHidden() && !poc->isHidden(), "the AV1 rows are hidden, the H.264 rows shown");
  widget.setObuModel(nullptr);
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc < 5)
  {
    std::cerr << "usage: 59-avc-frame-tab <annexb.h264> <file.mp4> <packets.csv> <frames.csv>\n";
    return 2;
  }
  const auto ref = readReference(argv[3], argv[4]);
  check(ref.codingOrder.size() == 25, "ffprobe reference: 25 frames");

  {
    parser::ParserAnnexBAVC p;
    p.enableModel();
    p.runParsingOfFile(argv[1]);
    p.updateNumberModelItems();
    checkModel("Annex-B", *p.getPacketItemModel(), ref);
  }
  {
    FFmpeg::FFmpegVersionHandler ff;
    ff.loadFFmpegLibraries();
    parser::ParserAVFormat p;
    p.enableModel();
    p.runParsingOfFile(argv[2]);
    p.updateNumberModelItems();
    p.setFilterStreamIndex(-1);
    checkModel("MP4", *p.getPacketItemModel(), ref);
  }

  std::cout << (failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") << std::endl;
  std::cout.flush();
  _exit(failures == 0 ? 0 : 1);
}
