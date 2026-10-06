#include "GmCsv.h"

namespace bda::me
{

void writeGmCsvHeader(std::ostream &out)
{
  out << "poc,ref_poc,win_i,win_j,n,dx_qpel,dy_qpel,accept_x,accept_y,"
         "cost_best_x,cost_zero_x,cost_best_y,cost_zero_y,a_q_x,a_q_y,sad2d_best,sad2d_zero,"
         "sad2d_no_x,sad2d_no_y,dx_coarse,dy_coarse\n";
}

void writeGmCsvRows(std::ostream &out, const int poc, const int refPoc, const GmFrameResult &frame)
{
  for (const auto &w : frame.windows)
    out << poc << ',' << refPoc << ',' << w.winI << ',' << w.winJ << ',' << frame.n << ','
        << w.dxQpel << ',' << w.dyQpel << ',' << int(w.acceptX) << ',' << int(w.acceptY) << ','
        << w.x.costBest << ',' << w.x.costZero << ',' << w.y.costBest << ',' << w.y.costZero
        << ',' << w.x.aQ << ',' << w.y.aQ << ',' << w.sad2dBest << ',' << w.sad2dZero << ','
        << w.sad2dNoX << ',' << w.sad2dNoY << ','
        // The downsampled search's answer in source pixels, before refinement.
        << (w.x.d << frame.n) << ',' << (w.y.d << frame.n) << '\n';
}

} // namespace bda::me
