// Frame-level global motion candidates for odyssey's open-loop ME, from a source clip.
//
//   gm-estimate <in.y4m | in.yuv> [--size WxH --bitdepth 8|10] [--frames A:B]
//               [--r 256] [--refine 16] [--smooth off|3|5] [--bright off|dc|dcgain]
//               [--csv out.csv] [--json out.json]
//
// One CSV row per window, 16 per frame, against the nearest past frame in display order
// (ref_poc = poc - 1). The first nine columns are what odyssey's loader reads
// (analyzer/source/gm_mv.c, gm_parse_row: poc,ref_poc,win_i,win_j,n,dx,dy,accept_x,accept_y);
// the rest are the costs and gains behind each decision, so the gates can be re-tuned from data.
//
// poc is the display index - the file order of a Y4M or raw clip. Writing decode order here would
// shift every frame on the consumer side, and the symptom would only be "no gain".
//
// Design: docs/ai/30-designs/global-motion-design.md. Qt-free.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "me/GlobalMotion.h"
#include "me/GmCsv.h"
#include "me/YuvLumaReader.h"

using namespace bda::me;

namespace
{
int usage()
{
  std::cerr << "usage: gm-estimate <in.y4m | in.yuv> [--size WxH --bitdepth 8|10] [--frames A:B]\n"
               "                   [--r 256] [--refine 16] [--smooth off|3|5]\n"
               "                   [--bright off|dc|dcgain]\n"
               "                   [--csv out.csv] [--json out.json]\n"
               "  --frames A:B   display frames A..B inclusive (frame 0 has no past reference)\n";
  return 2;
}

bool endsWith(const std::string &s, const std::string &tail)
{
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
} // namespace

int main(int argc, char **argv)
{
  std::string path, csvPath, jsonPath;
  int         width = 0, height = 0, bitDepth = 8, first = 1, last = -1;
  GmParams    params;

  for (int i = 1; i < argc; ++i)
  {
    const std::string a    = argv[i];
    const auto        next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--size")
    {
      const auto v = next();
      if (std::sscanf(v.c_str(), "%dx%d", &width, &height) != 2)
        return usage();
    }
    else if (a == "--bitdepth")
      bitDepth = std::atoi(next().c_str());
    else if (a == "--frames")
    {
      const auto v = next();
      if (std::sscanf(v.c_str(), "%d:%d", &first, &last) != 2)
        return usage();
    }
    else if (a == "--r")
      params.rOrig = std::atoi(next().c_str());
    else if (a == "--refine")
      params.refineRange = std::atoi(next().c_str());
    else if (a == "--smooth")
    {
      const auto v     = next();
      params.smoothing = v == "off" ? GmSmoothing::Off
                         : v == "3" ? GmSmoothing::Binomial3
                                    : GmSmoothing::Binomial5;
    }
    else if (a == "--bright")
    {
      const auto v      = next();
      params.brightness = v == "off" ? GmBrightness::Off
                          : v == "dc" ? GmBrightness::Dc
                                      : GmBrightness::DcGain;
    }
    else if (a == "--csv")
      csvPath = next();
    else if (a == "--json")
      jsonPath = next();
    else if (path.empty())
      path = a;
    else
      return usage();
  }
  if (path.empty())
    return usage();

  YuvLumaReader reader;
  std::string   error;
  const bool    opened = endsWith(path, ".y4m") ? reader.openY4m(path, error)
                                                : reader.openRaw(path, width, height, bitDepth, error);
  if (!opened)
  {
    std::cerr << error << std::endl;
    return 1;
  }
  if (last < 0 || last >= reader.frameCount())
    last = reader.frameCount() - 1;
  if (first < 1)
    first = 1; // frame 0 has no past reference

  std::ofstream csvFile, jsonFile;
  std::ostream *csv = &std::cout;
  if (!csvPath.empty())
  {
    csvFile.open(csvPath);
    csv = &csvFile;
  }
  if (!jsonPath.empty())
    jsonFile.open(jsonPath);

  writeGmCsvHeader(*csv);
  if (jsonFile)
    jsonFile << "{\"input\":\"" << path << "\",\"width\":" << reader.width()
             << ",\"height\":" << reader.height() << ",\"bit_depth\":" << reader.bitDepth()
             << ",\"r_orig\":" << params.rOrig << ",\"refine\":" << params.refineRange
             << ",\"frames\":[";

  std::cerr << path << ": " << reader.width() << "x" << reader.height() << ", "
            << reader.bitDepth() << "-bit, " << reader.frameCount() << " frames; estimating "
            << first << ".." << last << std::endl;

  // Read each frame once: this frame's current is the next frame's reference.
  MePlane   ref = reader.readLuma(first - 1, 0, error);
  long      accepted = 0, windows = 0;
  double    millis   = 0;
  bool      firstJson = true;
  for (int poc = first; poc <= last && !ref.empty(); ++poc)
  {
    MePlane cur = reader.readLuma(poc, 0, error);
    if (cur.empty())
      break;

    const auto start = std::chrono::steady_clock::now();
    const auto r     = estimateGlobalMotion(cur, ref, params);
    millis += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count();
    if (!r.ok())
    {
      std::cerr << "frame " << poc << ": " << r.error << std::endl;
      return 1;
    }

    if (jsonFile)
      jsonFile << (firstJson ? "" : ",") << "{\"poc\":" << poc << ",\"ref_poc\":" << poc - 1
               << ",\"n\":" << r.n << ",\"wd\":" << r.wd << ",\"hd\":" << r.hd
               << ",\"r_ds\":" << r.rDs << ",\"windows\":[";
    writeGmCsvRows(*csv, poc, poc - 1, r);
    bool firstWin = true;
    for (const auto &w : r.windows)
    {
      if (jsonFile)
        jsonFile << (firstWin ? "" : ",") << "{\"win_i\":" << w.winI << ",\"win_j\":" << w.winJ
                 << ",\"dx_qpel\":" << w.dxQpel << ",\"dy_qpel\":" << w.dyQpel
                 << ",\"accept_x\":" << (w.acceptX ? "true" : "false")
                 << ",\"accept_y\":" << (w.acceptY ? "true" : "false")
                 << ",\"cost_best_x\":" << w.x.costBest << ",\"cost_zero_x\":" << w.x.costZero
                 << ",\"cost_best_y\":" << w.y.costBest << ",\"cost_zero_y\":" << w.y.costZero
                 << ",\"a_q_x\":" << w.x.aQ << ",\"a_q_y\":" << w.y.aQ
                 << ",\"sad2d_best\":" << w.sad2dBest << ",\"sad2d_zero\":" << w.sad2dZero
                 << ",\"sad2d_no_x\":" << w.sad2dNoX << ",\"sad2d_no_y\":" << w.sad2dNoY
                 << ",\"dx_coarse\":" << (w.x.d << r.n) << ",\"dy_coarse\":" << (w.y.d << r.n)
                 << "}";
      firstWin = false;
      accepted += w.acceptX + w.acceptY;
      windows += 2;
    }
    if (jsonFile)
      jsonFile << "]}";
    firstJson = false;

    if (poc % 10 == 0)
      std::cerr << "\rframe " << poc << " of " << last << std::flush;
    ref = std::move(cur);
  }
  if (jsonFile)
    jsonFile << "]}\n";
  if (!error.empty())
  {
    std::cerr << "\n" << error << std::endl;
    return 1;
  }

  const int done = last - first + 1;
  std::cerr << "\r" << done << " frames, " << millis / std::max(done, 1)
            << " ms per frame (estimation only), axes accepted " << accepted << " of " << windows
            << std::endl;
  return 0;
}
