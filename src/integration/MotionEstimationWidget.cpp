#include "MotionEstimationWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

namespace bda::integration
{

namespace
{
// "Start MV from" data: an Algorithm value, -1 for (0, 0), or this for the global model.
constexpr int kSeedGlobalModel = -2;
} // namespace

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
  this->algorithm_->addItem(tr("Odyssey closed-loop ME"),
                            static_cast<int>(me::Algorithm::OdysseyClosedLoop));
  this->algorithm_->setItemData(
      2,
      tr("odyssey's encoder ME: a full-pel 8x8 window around each block's start vector, then a "
         "64-point quarter/eighth-pel search on the frame pair. Matches odyssey's own trace."),
      Qt::ToolTipRole);
  this->algorithm_->addItem(tr("Odyssey open-loop ME adv (GF + TMV)"),
                            static_cast<int>(me::Algorithm::OdysseyOpenLoopAdv));
  this->algorithm_->setItemData(
      3,
      tr("odyssey's lookahead order: the whole GF group bottom-up, references from the GF "
         "structure, temporal MV candidates. Matches odyssey's own trace block for block."),
      Qt::ToolTipRole);
  form->addRow(tr("Algorithm"), this->algorithm_);

  // odyssey open-loop adv only (shown when it is chosen).
  this->form_ = form;
  this->gf_   = new QComboBox(this);
  for (int gf : {2, 4, 8, 16})
    this->gf_->addItem(QString::number(gf), gf);
  this->gf_->setCurrentIndex(3);
  this->gf_->setToolTip(tr("GF interval: the group the frames are analysed in, layer 5 first."));
  form->addRow(tr("GF interval"), this->gf_);
  this->tmvGrid_ = new QComboBox(this);
  this->tmvGrid_->addItem(tr("16x16 blocks"), false);
  this->tmvGrid_->addItem(tr("64x64 superblocks (odyssey trace)"), true);
  this->tmvGrid_->setToolTip(tr("Which stored vector a temporal candidate takes. odyssey's code "
                                "keeps one per superblock; 16x16 is the default here."));
  form->addRow(tr("TMV grid"), this->tmvGrid_);
  this->advReference_ = new QComboBox(this);
  this->advReference_->addItem(tr("Nearest reference"), 0);
  this->advReference_->addItem(tr("Second reference"), 1);
  this->advReference_->setToolTip(tr("B frames search L0 and L1, P frames two list-0 references; "
                                     "the overlay shows one of them."));
  form->addRow(tr("Show"), this->advReference_);

  // odyssey closed-loop only (shown when it is chosen).
  this->seed_ = new QComboBox(this);
  this->seed_->addItem(tr("None (0, 0)"), -1);
  this->seed_->addItem(tr("SVT-AV1 integer ME"), static_cast<int>(me::Algorithm::SvtIntegerMe));
  this->seed_->addItem(tr("Odyssey open-loop ME"), static_cast<int>(me::Algorithm::OdysseyOpenLoop));
  this->seed_->addItem(tr("Global model (SVT ME)"), kSeedGlobalModel);
  this->seed_->setToolTip(tr("Where each block's search starts: the vector the chosen ME finds for "
                             "the same block and size (odyssey uses its open-loop result), or "
                             "(0, 0)."));
  form->addRow(tr("Start MV from"), this->seed_);
  this->seedModel_ = new QComboBox(this);
  this->seedModel_->addItem(tr("Translation"), static_cast<int>(me::GmModelType::Translation));
  this->seedModel_->addItem(tr("RotZoom"), static_cast<int>(me::GmModelType::RotZoom));
  this->seedModel_->addItem(tr("Affine"), static_cast<int>(me::GmModelType::Affine));
  this->seedModel_->setCurrentIndex(1);
  this->seedModel_->setToolTip(tr("The global model fitted with RANSAC to SVT integer ME 16x16 vectors; "
                                  "each block starts from the model's vector at its centre."));
  form->addRow(tr("Model"), this->seedModel_);
  this->subpel_ = new QComboBox(this);
  this->subpel_->addItem(tr("1/8 pel allowed"), true);
  this->subpel_->addItem(tr("1/4 pel only"), false);
  this->subpel_->setToolTip(tr("allow_high_precision_mv: eighth-pel refinement with the 8-tap "
                               "filter, or quarter-pel with the 4-tap one."));
  form->addRow(tr("Sub-pel"), this->subpel_);

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

  /* Superblocks, not percent, as the underlying unit: the estimators count in superblocks and the
   * number is worth showing - it says how much work a frame actually is. Shown at all times rather
   * than only while running, because "no estimate for this frame" is exactly the state that needs
   * saying out loud.
   */
  this->progress_ = new QProgressBar(this);
  this->progress_->setTextVisible(true);
  outer->addWidget(this->progress_);
  this->setProgressIdle(tr("No estimate yet"));

  this->status_ = new QLabel(this);
  this->status_->setWordWrap(true);
  outer->addWidget(this->status_);
  outer->addStretch();

  const auto notify = [this]() { this->emitIfLive(); };
  connect(this->enable_, &QCheckBox::toggled, this, notify);
  connect(this->interval_, &QSpinBox::valueChanged, this, notify);
  connect(this->algorithm_, &QComboBox::currentIndexChanged, this, notify);
  connect(this->algorithm_, &QComboBox::currentIndexChanged, this, [this] { this->updateAdvControls(); });
  connect(this->seed_, &QComboBox::currentIndexChanged, this, [this] { this->updateAdvControls(); });
  for (auto *box : {this->gf_, this->tmvGrid_, this->advReference_, this->seed_, this->subpel_, this->seedModel_})
    connect(box, &QComboBox::currentIndexChanged, this, notify);
  this->updateAdvControls();
  connect(this->staticBypass_, &QCheckBox::toggled, this, notify);
  for (auto *box : {this->size8_, this->size16_, this->size32_, this->size64_})
    connect(box, &QCheckBox::toggled, this, notify);
}

void MotionEstimationWidget::emitIfLive()
{
  emit this->parametersChanged();
}

void MotionEstimationWidget::updateAdvControls()
{
  const bool adv =
      this->algorithm_->currentData().toInt() == static_cast<int>(me::Algorithm::OdysseyOpenLoopAdv);
  for (QWidget *w : {static_cast<QWidget *>(this->gf_), static_cast<QWidget *>(this->tmvGrid_),
                     static_cast<QWidget *>(this->advReference_)})
    this->form_->setRowVisible(w, adv);
  const bool closedLoop =
      this->algorithm_->currentData().toInt() == static_cast<int>(me::Algorithm::OdysseyClosedLoop);
  for (QWidget *w : {static_cast<QWidget *>(this->seed_), static_cast<QWidget *>(this->subpel_)})
    this->form_->setRowVisible(w, closedLoop);
  this->form_->setRowVisible(this->seedModel_,
                             closedLoop && this->seed_->currentData().toInt() == kSeedGlobalModel);
  if (this->controlsEnabled_)
    this->interval_->setEnabled(!adv);
  this->interval_->setToolTip(adv ? tr("Not used by odyssey open-loop adv: the GF structure picks "
                                       "the references.")
                                  : tr("Reference = current - interval. Negative reaches forward "
                                       "in display order."));
}

int MotionEstimationWidget::gfInterval() const { return this->gf_->currentData().toInt(); }

bool MotionEstimationWidget::tmvSb64() const { return this->tmvGrid_->currentData().toBool(); }

int MotionEstimationWidget::advSearchIndex() const { return this->advReference_->currentData().toInt(); }

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

  if (const int seed = this->seed_->currentData().toInt(); seed >= 0)
    p.seedAlgorithm = static_cast<me::Algorithm>(seed);
  else if (seed == kSeedGlobalModel)
  {
    p.seedAlgorithm = me::Algorithm::SvtIntegerMe;
    p.seedModel     = static_cast<me::GmModelType>(this->seedModel_->currentData().toInt());
  }
  p.allowHighPrecisionMv = this->subpel_->currentData().toBool();

  return p;
}

void MotionEstimationWidget::setUnavailable(const QString &reason)
{
  // The text is refreshed either way; the enabled state only when it actually changes.
  this->status_->setText(reason);
  if (!this->controlsEnabled_)
    return;
  this->controlsEnabled_ = false;
  // The status text and the progress bar stay readable: they are what explains the grey controls.
  for (auto *w : this->findChildren<QWidget *>())
    if (w != this->status_ && w != this->progress_)
      w->setEnabled(false);
}

void MotionEstimationWidget::setAvailable()
{
  if (this->controlsEnabled_)
    return;
  this->controlsEnabled_ = true;
  for (auto *w : this->findChildren<QWidget *>())
    w->setEnabled(true);
  this->updateAdvControls(); // the interval stays grey for adv
}

void MotionEstimationWidget::setStatus(const QString &text)
{
  this->status_->setText(text);
}

void MotionEstimationWidget::setProgressIdle(const QString &text)
{
  this->progress_->setRange(0, 1);
  this->progress_->setValue(0);
  this->progress_->setFormat(text);
}

void MotionEstimationWidget::setProgressRunning(int doneSuperblocks, int totalSuperblocks)
{
  if (totalSuperblocks <= 0)
  {
    /* The worker has not reached its own loop yet, so the total is not known. A busy indicator
     * (range 0..0) says "started, no idea how far" without inventing a percentage.
     */
    this->progress_->setRange(0, 0);
    this->progress_->setFormat(tr("Starting..."));
    return;
  }
  this->progress_->setRange(0, totalSuperblocks);
  this->progress_->setValue(qBound(0, doneSuperblocks, totalSuperblocks));
  this->progress_->setFormat(tr("%p%  (%v/%m superblocks)"));
}

void MotionEstimationWidget::setProgressComplete(const QString &text)
{
  const int max = std::max(1, this->progress_->maximum());
  this->progress_->setRange(0, max);
  this->progress_->setValue(max);
  this->progress_->setFormat(text);
}

} // namespace bda::integration
