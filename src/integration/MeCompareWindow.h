// The ME compare popup: Shift+click on a block shows the source block next to the reference block
// its motion vector fetched, or their luma difference - one tab per estimate covering the pixel
// (each enabled block-ME size, and the global-motion window).
#pragma once

#include <QByteArray>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QList>
#include <QSpinBox>
#include <QTabWidget>

#include <vector>

#include "me/MeCompareCrop.h"

namespace bda::integration
{

//!< One estimate to show: a block of the source frame and where its vector points in the reference.
struct MeCompareEntry
{
  QString      title;     //!< Tab text, e.g. "ME 16x16", "GME window (1, 2)".
  QString      detail;    //!< One line of facts for the tab header.
  QByteArray   source;    //!< Raw frame the block is in.
  QByteArray   reference; //!< Raw frame the vector points into.
  me::CropRect rect;      //!< The block, luma pixels.
  int          dx{}, dy{}; //!< Whole-pixel displacement into the reference.
};

class MeCompareWindow : public QDialog
{
  Q_OBJECT

public:
  explicit MeCompareWindow(QWidget *parent = nullptr);

  //!< Replace what is shown. The first entry's tab is selected.
  void setComparison(const QString                &heading,
                     const me::YuvFrameLayout     &layout,
                     const QList<MeCompareEntry> &entries);

  int     tabCount() const { return this->tabs->count(); }
  QString tabTitle(int index) const { return this->tabs->tabText(index); }
  QString tabDetail(int index) const
  {
    return index >= 0 && index < this->entries.size() ? this->entries[index].detail : QString();
  }
  //!< The luma difference of a tab, as drawn - for tests.
  me::LumaDiff diffOf(int index) const;
  void         setDiffMode(bool diff) { this->mode->setCurrentIndex(diff ? 1 : 0); }

private slots:
  void redraw();

private:
  QLabel     *heading{};
  QComboBox  *mode{};
  QSpinBox   *zoom{}, *gain{};
  QTabWidget *tabs{};

  struct Tab
  {
    QLabel *info{};
    QLabel *picture{};
  };
  std::vector<Tab>       tabWidgets;
  me::YuvFrameLayout     layout;
  QList<MeCompareEntry> entries;
};

} // namespace bda::integration
