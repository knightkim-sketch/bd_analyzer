#include "MeCompareWindow.h"

#include <QHBoxLayout>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace bda::integration
{
namespace
{

QImage toQImage(const me::RgbImage &image)
{
  if (image.width <= 0 || image.height <= 0)
    return {};
  return QImage(image.rgb.data(), image.width, image.height, image.width * 3, QImage::Format_RGB888)
      .copy();
}

// Large enough to read a pixel, small enough that a 64x64 block and a GME window both fit.
int autoZoom(const me::CropRect &rect)
{
  const int longest = std::max(1, std::max(rect.w, rect.h));
  return std::clamp(512 / longest, 1, 32);
}

me::CropRect moved(const me::CropRect &rect, int dx, int dy)
{
  return {rect.x + dx, rect.y + dy, rect.w, rect.h};
}

} // namespace

MeCompareWindow::MeCompareWindow(QWidget *parent) : QDialog(parent)
{
  this->setWindowTitle("ME block compare");
  this->resize(1150, 700);

  auto *layout  = new QVBoxLayout(this);
  this->heading = new QLabel(this);
  this->heading->setWordWrap(true);
  this->heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(this->heading);

  auto *controls = new QHBoxLayout();
  controls->addWidget(new QLabel("View", this));
  this->mode = new QComboBox(this);
  this->mode->addItems({"Side by side (source | reference)", "Luma difference"});
  controls->addWidget(this->mode);
  controls->addWidget(new QLabel("Zoom", this));
  this->zoom = new QSpinBox(this);
  this->zoom->setRange(0, 32);
  this->zoom->setSpecialValueText("auto");
  this->zoom->setSuffix("x");
  controls->addWidget(this->zoom);
  controls->addWidget(new QLabel("Diff gain", this));
  this->gain = new QSpinBox(this);
  this->gain->setRange(1, 16);
  this->gain->setValue(4);
  this->gain->setToolTip("The difference image shows 128 + gain x (source - reference).");
  controls->addWidget(this->gain);
  controls->addStretch();
  layout->addLayout(controls);

  this->tabs = new QTabWidget(this);
  layout->addWidget(this->tabs, 1);

  connect(this->mode, &QComboBox::currentIndexChanged, this, &MeCompareWindow::redraw);
  connect(this->zoom, &QSpinBox::valueChanged, this, &MeCompareWindow::redraw);
  connect(this->gain, &QSpinBox::valueChanged, this, &MeCompareWindow::redraw);
}

void MeCompareWindow::setComparison(const QString                &heading,
                                    const me::YuvFrameLayout     &layout,
                                    const QList<MeCompareEntry> &entries)
{
  this->heading->setText(heading);
  this->layout  = layout;
  this->entries = entries;

  this->tabs->clear();
  this->tabWidgets.clear();
  for (const auto &entry : entries)
  {
    auto *page  = new QWidget(this->tabs);
    auto *v     = new QVBoxLayout(page);
    Tab   tab;
    tab.info    = new QLabel(page);
    tab.info->setWordWrap(true);
    tab.info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(tab.info);
    auto *scroll = new QScrollArea(page);
    tab.picture  = new QLabel(scroll);
    tab.picture->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll->setWidget(tab.picture);
    scroll->setWidgetResizable(true);
    v->addWidget(scroll, 1);
    this->tabs->addTab(page, entry.title);
    this->tabWidgets.push_back(tab);
  }
  this->redraw();
}

me::LumaDiff MeCompareWindow::diffOf(int index) const
{
  if (index < 0 || index >= this->entries.size())
    return {};
  const auto &e = this->entries[index];
  if (!me::checkLayout(this->layout, std::size_t(e.source.size())).empty() ||
      !me::checkLayout(this->layout, std::size_t(e.reference.size())).empty())
    return {};
  return me::lumaDiff(reinterpret_cast<const std::uint8_t *>(e.source.constData()),
                      reinterpret_cast<const std::uint8_t *>(e.reference.constData()),
                      this->layout,
                      e.rect,
                      e.dx,
                      e.dy,
                      this->gain->value());
}

void MeCompareWindow::redraw()
{
  const bool diffMode = this->mode->currentIndex() == 1;

  for (int i = 0; i < this->entries.size() && i < int(this->tabWidgets.size()); ++i)
  {
    const auto &e   = this->entries[i];
    auto       &tab = this->tabWidgets[std::size_t(i)];

    auto problem = me::checkLayout(this->layout, std::size_t(e.source.size()));
    if (problem.empty())
      problem = me::checkLayout(this->layout, std::size_t(e.reference.size()));
    if (!problem.empty())
    {
      tab.info->setText(e.detail + "\nCannot show this block: " + QString::fromStdString(problem));
      tab.picture->clear();
      continue;
    }

    const auto diff = this->diffOf(i);
    tab.info->setText(e.detail + QString("\nSAD %1 over %2x%3 (8-bit luma), largest |difference| %4")
                                     .arg(diff.sad)
                                     .arg(e.rect.w)
                                     .arg(e.rect.h)
                                     .arg(diff.maxAbs));

    const int z = this->zoom->value() == 0 ? autoZoom(e.rect) : this->zoom->value();
    const int w = e.rect.w * z, h = e.rect.h * z;

    if (diffMode)
    {
      tab.picture->setPixmap(
          QPixmap::fromImage(toQImage(diff.image).scaled(w, h, Qt::IgnoreAspectRatio,
                                                         Qt::FastTransformation)));
      continue;
    }

    const auto *src = reinterpret_cast<const std::uint8_t *>(e.source.constData());
    const auto *ref = reinterpret_cast<const std::uint8_t *>(e.reference.constData());
    const auto  a   = toQImage(me::cropRgb(src, this->layout, e.rect));
    const auto  b   = toQImage(me::cropRgb(ref, this->layout, moved(e.rect, e.dx, e.dy)));

    const int gap = 12;
    QImage    both(w * 2 + gap, h, QImage::Format_RGB888);
    both.fill(this->palette().color(QPalette::Window));
    QPainter painter(&both);
    painter.drawImage(QRect(0, 0, w, h), a);
    painter.drawImage(QRect(w + gap, 0, w, h), b);
    painter.end();
    tab.picture->setPixmap(QPixmap::fromImage(both));
  }
}

} // namespace bda::integration
