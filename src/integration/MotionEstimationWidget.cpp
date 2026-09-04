#include "MotionEstimationWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace bda::integration
{

MotionEstimationWidget::MotionEstimationWidget(QWidget *parent) : QWidget(parent)
{
  auto *outer = new QVBoxLayout(this);

  this->enable_ = new QCheckBox(tr("Compute in the background"), this);
  this->enable_->setToolTip(tr("Estimate the displayed frame against its reference whenever either "
                               "changes. Unchecked, nothing is computed."));
  outer->addWidget(this->enable_);

  auto *form = new QFormLayout();
  outer->addLayout(form);

  /* Signed, and 0 is excluded rather than clamped away: a positive interval takes the reference
   * from the past, a negative one from the future, and 0 would match a frame against itself.
   * QSpinBox cannot express "any integer but zero", so the range allows it and the run refuses it
   * with a reason - which is better than silently moving the value the user typed.
   */
  this->interval_ = new QSpinBox(this);
  this->interval_->setRange(-60, 60);
  this->interval_->setValue(1);
  this->interval_->setToolTip(
      tr("Reference = current - interval. Negative reaches forward in display order."));
  form->addRow(tr("Frame interval"), this->interval_);

  this->algorithm_ = new QComboBox(this);
  this->algorithm_->addItem(tr("SVT-AV1 integer ME"),
                            static_cast<int>(me::Algorithm::SvtIntegerMe));
  this->algorithm_->addItem(tr("Odyssey open-loop ME"),
                            static_cast<int>(me::Algorithm::OdysseyOpenLoop));
  /* The closed-loop estimator is the second phase. It is listed so the plan is visible, and
   * disabled so choosing it cannot produce an empty overlay: it needs a reconstruction to search
   * against and an open-loop result to centre on.
   */
  this->algorithm_->addItem(tr("Odyssey closed-loop ME (not yet)"),
                            static_cast<int>(me::Algorithm::OdysseyClosedLoop));
  if (auto *model = qobject_cast<QStandardItemModel *>(this->algorithm_->model()))
    if (auto *item = model->item(2))
      item->setEnabled(false);
  form->addRow(tr("Algorithm"), this->algorithm_);

  /* The block-size checkboxes are the only switch for which overlays exist - the statistics panel
   * gets exactly the types selected here. Putting the same choice in both places would leave the
   * user unable to tell which one wins.
   *
   * 64x64 alone by default: one arrow per superblock is readable, where 8x8 on a large picture is
   * a thicket.
   */
  auto *sizes  = new QHBoxLayout();
  this->size8_  = new QCheckBox(tr("8"), this);
  this->size16_ = new QCheckBox(tr("16"), this);
  this->size32_ = new QCheckBox(tr("32"), this);
  this->size64_ = new QCheckBox(tr("64"), this);
  this->size64_->setChecked(true);
  for (auto *box : {this->size8_, this->size16_, this->size32_, this->size64_})
    sizes->addWidget(box);
  sizes->addStretch();
  form->addRow(tr("Block sizes"), sizes);

  /* Reproducing SVT's static-block early exit is on by default, because leaving it out changes
   * which blocks end up with a searched vector and the point of this feature is to match the
   * encoder. Exposed because it is also the first thing to turn off when a result looks too empty.
   */
  this->staticBypass_ = new QCheckBox(tr("SVT static block bypass"), this);
  this->staticBypass_->setChecked(true);
  outer->addWidget(this->staticBypass_);

  this->status_ = new QLabel(this);
  this->status_->setWordWrap(true);
  outer->addWidget(this->status_);
  outer->addStretch();

  const auto notify = [this]() { this->emitIfLive(); };
  connect(this->enable_, &QCheckBox::toggled, this, notify);
  connect(this->interval_, &QSpinBox::valueChanged, this, notify);
  connect(this->algorithm_, &QComboBox::currentIndexChanged, this, notify);
  connect(this->staticBypass_, &QCheckBox::toggled, this, notify);
  for (auto *box : {this->size8_, this->size16_, this->size32_, this->size64_})
    connect(box, &QCheckBox::toggled, this, notify);
}

void MotionEstimationWidget::emitIfLive()
{
  emit this->parametersChanged();
}

bool MotionEstimationWidget::autoComputeEnabled() const
{
  return this->enable_->isChecked();
}

me::MeParams MotionEstimationWidget::params() const
{
  me::MeParams p;
  p.frameInterval = this->interval_->value();
  p.algorithm =
      static_cast<me::Algorithm>(this->algorithm_->currentData().toInt());
  p.staticBypass = this->staticBypass_->isChecked();

  me::BlockSizeSet sizes;
  if (this->size8_->isChecked())
    sizes.add(me::BlockSize::Blk8);
  if (this->size16_->isChecked())
    sizes.add(me::BlockSize::Blk16);
  if (this->size32_->isChecked())
    sizes.add(me::BlockSize::Blk32);
  if (this->size64_->isChecked())
    sizes.add(me::BlockSize::Blk64);
  p.blockSizes = sizes;

  return p;
}

void MotionEstimationWidget::setUnavailable(const QString &reason)
{
  // The text is refreshed either way; the enabled state only when it actually changes.
  this->status_->setText(reason);
  if (!this->controlsEnabled_)
    return;
  this->controlsEnabled_ = false;
  for (auto *w : this->findChildren<QWidget *>())
    if (w != this->status_)
      w->setEnabled(false);
}

void MotionEstimationWidget::setAvailable()
{
  if (this->controlsEnabled_)
    return;
  this->controlsEnabled_ = true;
  for (auto *w : this->findChildren<QWidget *>())
    w->setEnabled(true);
}

void MotionEstimationWidget::setStatus(const QString &text)
{
  this->status_->setText(text);
}

} // namespace bda::integration
