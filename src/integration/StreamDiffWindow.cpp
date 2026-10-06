#include "StreamDiffWindow.h"

#include <QAbstractItemModel>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QRadioButton>
#include <QVBoxLayout>

#include <filesystem>
#include <vector>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"
#include "playlistitem/playlistItem.h"
#include "playlistitem/playlistItemCompressedVideo.h"

namespace bda::integration
{

namespace
{

constexpr int MiSize = 4;

// Where a row points, stored on the row itself. A row without it names no place.
constexpr int RoleFrame = Qt::UserRole;
constexpr int RolePixel = Qt::UserRole + 1;

//!< More rows than this in one section is a list nobody reads; the summary carries the count.
constexpr std::size_t MaxRows = 200;

/* Flatten one OBU's syntax, in bitstream order.
 *
 * Values the parser derives for convenience (FrameWidth, TileColsLog2 ...) are kept: they are
 * computed from the syntax, so a difference in one means a difference in the syntax behind it, and
 * the derived name is usually the clearer thing to show.
 */
void collectElements(const QAbstractItemModel &            model,
                     const QModelIndex &                   index,
                     std::vector<bda::diff::SyntaxElement> &out)
{
  const auto name  = model.data(model.index(index.row(), 0, index.parent())).toString();
  const auto value = model.data(model.index(index.row(), 1, index.parent())).toString();
  if (!value.isEmpty() && !name.startsWith("raw_byte"))
    out.push_back({name.toStdString(), value.toStdString()});

  for (int row = 0; row < model.rowCount(index); ++row)
    collectElements(model, model.index(row, 0, index), out);
}

/* One section per OBU that carries syntax.
 *
 * Per OBU rather than per stream because the alignment is exact only while the sections are small:
 * a whole 24 frame clip flattens to some 13000 elements, far past any sane table. It also gives
 * the report the locality that matters - "this OBU differs" instead of "element 8412 differs".
 *
 * OBUs with no syntax of their own (a temporal delimiter) are left out. They would pair with
 * anything and only shift the positional alignment.
 *
 * When displayOf is given, the same parse also works out which display frame shows each coded
 * frame - step 4 needs that, and parsing a 4K stream twice for it would be the slow part.
 */
bool readSections(const QString &                        path,
                  std::vector<bda::diff::SyntaxSection> &out,
                  DisplayMap *                           displayOf)
{
  parser::ParserAVFormat parser;
  parser.enableModel();
  if (!parser.runParsingOfFile(std::filesystem::path(path.toStdString())))
    return false;
  parser.updateNumberModelItems();

  auto *model = parser.getPacketItemModel();
  if (model == nullptr)
    return false;

  for (int packet = 0; packet < model->rowCount(); ++packet)
  {
    const auto packetIdx = model->index(packet, 0);

    // Label with the index the parser logged, not the row number: they are not the same, and a
    // label that disagrees with the Bitstream Analysis panel sends the reader to the wrong packet.
    auto packetLabel = QString("row %1").arg(packet);
    for (int child = 0; child < model->rowCount(packetIdx); ++child)
      if (model->data(model->index(child, 0, packetIdx)).toString() == "Global AVPacket Count")
      {
        packetLabel =
            "packet " + model->data(model->index(child, 1, packetIdx)).toString();
        break;
      }

    for (int child = 0; child < model->rowCount(packetIdx); ++child)
    {
      const auto childIdx = model->index(child, 0, packetIdx);
      const auto name     = model->data(childIdx).toString();
      if (!name.startsWith("OBU"))
        continue;
      bda::diff::SyntaxSection section;
      section.label = (packetLabel + " / " + name).toStdString();
      collectElements(*model, childIdx, section.elements);
      if (!section.elements.empty())
        out.push_back(std::move(section));
    }
  }

  if (displayOf != nullptr)
    *displayOf = buildDisplayMap(*model);
  return true;
}

QString kindText(bda::diff::DiffKind kind)
{
  switch (kind)
  {
  case bda::diff::DiffKind::ValueMismatch: return "value";
  case bda::diff::DiffKind::OnlyInA:       return "only in A";
  case bda::diff::DiffKind::OnlyInB:       return "only in B";
  }
  return {};
}

QString payloadKindText(bda::diff::PayloadDiffKind kind)
{
  switch (kind)
  {
  case bda::diff::PayloadDiffKind::Equal:          return "equal";
  case bda::diff::PayloadDiffKind::SizeDiffers:    return "size differs";
  case bda::diff::PayloadDiffKind::ContentDiffers: return "content differs";
  case bda::diff::PayloadDiffKind::OnlyInA:        return "only in A";
  case bda::diff::PayloadDiffKind::OnlyInB:        return "only in B";
  }
  return {};
}

std::size_t countElements(const std::vector<bda::diff::SyntaxSection> &sections)
{
  std::size_t n = 0;
  for (const auto &s : sections)
    n += s.elements.size();
  return n;
}

bool readFile(const QString &path, QByteArray &out)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  out = file.readAll();
  return true;
}

void setLocation(QTreeWidgetItem *row, const int frameIdx, const std::optional<QPoint> &pixel)
{
  row->setData(0, RoleFrame, frameIdx);
  if (pixel)
    row->setData(0, RolePixel, *pixel);
  row->setToolTip(0, "Double-click to show this place in the main window");
}

QString sbText(const bda::diff::SbDiff &sb)
{
  return QString("SB(%1, %2)").arg(sb.sbRow).arg(sb.sbCol);
}

} // namespace

StreamDiffWindow::StreamDiffWindow(QWidget *parent) : QDialog(parent)
{
  this->setWindowTitle("Find diff");
  this->resize(1100, 760);

  auto *layout = new QVBoxLayout(this);

  this->summary = new QLabel(this);
  this->summary->setWordWrap(true);
  this->summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(this->summary);

  this->progress = new QLabel(this);
  this->progress->setVisible(false);
  layout->addWidget(this->progress);

  /* Two independent choices, as the design asks: which stream the block panes follow, and which
   * stream's syntax tree the Bitstream Analysis pane shows. Reading one stream's headers beside the
   * other stream's block is a real use, so neither is derived from the other.
   */
  auto *toggles = new QHBoxLayout();
  const auto addChoice = [this, toggles](const QString &label, QButtonGroup *&group,
                                         const QString &tip) {
    auto *caption = new QLabel(label, this);
    caption->setToolTip(tip);
    toggles->addWidget(caption);
    group = new QButtonGroup(this);
    for (int stream = 0; stream < 2; ++stream)
    {
      auto *button = new QRadioButton(stream == 0 ? "A" : "B", this);
      button->setToolTip(tip);
      group->addButton(button, stream);
      toggles->addWidget(button);
    }
    group->button(0)->setChecked(true);
    toggles->addSpacing(24);
  };
  addChoice("Block info:", this->blockInfoChoice,
            "Which stream the main window's Block Info, Frame Info and hexdump panes and the block "
            "highlight follow when jumping to a difference. The table here always shows both.");
  addChoice("Syntax info:", this->syntaxInfoChoice,
            "Which stream the main window's Bitstream Analysis pane shows.");
  this->goToFirst = new QPushButton("Go to first difference", this);
  toggles->addWidget(this->goToFirst);
  toggles->addStretch(1);
  layout->addLayout(toggles);

  this->tree = new QTreeWidget(this);
  this->tree->setColumnCount(4);
  this->tree->setHeaderLabels({"Where", "Difference", "A", "B"});
  this->tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  this->tree->setAlternatingRowColors(true);
  layout->addWidget(this->tree, 1);

  this->compareCdf = new QCheckBox("Compare CDF (entropy coder state)", this);
  this->compareCdf->setEnabled(false);
  this->compareCdf->setToolTip("Not available: the analyzer decoder exports block data and bit "
                               "ranges, but not the adaptive probability tables.");
  layout->addWidget(this->compareCdf);

  this->compareRecon =
      new QCheckBox("Compare reconstructed pictures (decodes both whole streams)", this);
  this->compareRecon->setToolTip(
      "Every display frame of both streams is decoded and compared sample by sample. Measured: "
      "about 22 s for 130 frames of 4K 10-bit. Unticking stops a comparison that is running.");
  layout->addWidget(this->compareRecon);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);

  connect(this, &StreamDiffWindow::comparisonFinished, this, &StreamDiffWindow::showResult,
          Qt::QueuedConnection);
  connect(this, &StreamDiffWindow::progressChanged, this, &StreamDiffWindow::showProgress,
          Qt::QueuedConnection);
  connect(this->tree, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem *row, int) { this->activateRow(row); });
  connect(this->compareRecon, &QCheckBox::toggled, this, &StreamDiffWindow::reconToggled);
  connect(this->goToFirst, &QPushButton::clicked, this, [this] {
    if (const auto where = this->firstDifference())
      this->locate(*where);
  });
  // A toggle re-shows the place last jumped to with the other stream, rather than doing nothing
  // until the next jump.
  const auto reshow = [this] {
    if (this->lastLocation)
      this->locate(*this->lastLocation);
  };
  connect(this->blockInfoChoice, &QButtonGroup::idClicked, this, reshow);
  connect(this->syntaxInfoChoice, &QButtonGroup::idClicked, this, reshow);

  this->updateControls();
}

StreamDiffWindow::~StreamDiffWindow()
{
  // The worker writes into this object. Let it see the request and finish before the memory goes.
  this->cancelRequested      = true;
  this->reconCancelRequested = true;
  if (this->worker)
    this->worker->wait();
}

void StreamDiffWindow::closeEvent(QCloseEvent *event)
{
  // Nobody is looking any more. A whole-stream decode is not worth finishing for a hidden window.
  if (this->running)
  {
    this->cancelRequested      = true;
    this->reconCancelRequested = true;
  }
  QDialog::closeEvent(event);
}

void StreamDiffWindow::setBusy(const QString &what)
{
  this->summary->setText(what);
  this->progress->clear();
  this->progress->setVisible(true);
}

void StreamDiffWindow::updateControls()
{
  this->goToFirst->setEnabled(!this->running && this->firstDifference().has_value());
  // Ticking it starts a run, unticking stops one; it stays usable throughout for that reason.
  this->compareRecon->setEnabled(!this->nameA.isEmpty());
}

StreamDiffResult StreamDiffWindow::compare(const QList<playlistItem *> &selection)
{
  if (this->running)
    return {false, "A comparison is already running in this window."};

  QList<playlistItemCompressedVideo *> streams;
  for (auto *item : selection)
    if (auto *compressed = dynamic_cast<playlistItemCompressedVideo *>(item))
      streams.append(compressed);

  if (streams.size() != 2)
    return {false,
            QString("Select exactly two compressed streams. The selection has %1 of them (%2 "
                    "items in total).")
                .arg(streams.size())
                .arg(selection.size())};

  const auto nameA = streams[0]->properties().name;
  const auto nameB = streams[1]->properties().name;
  if (nameA == nameB)
    return {false, "Both selected items are the same file."};

  this->nameA = nameA;
  this->nameB = nameB;
  this->itemA = streams[0];
  this->itemB = streams[1];
  this->lastLocation.reset();
  this->report = {};
  this->tree->clear();

  this->startWorker(true, this->compareRecon->isChecked());
  return {true, {}};
}

void StreamDiffWindow::reconToggled(bool checked)
{
  if (!checked)
  {
    // Stops the picture comparison only; the steps before it are cheap and keep their answer.
    if (this->running)
      this->reconCancelRequested = true;
    return;
  }
  // Ticked after a comparison finished: run only the part that is missing.
  if (!this->running && !this->nameA.isEmpty() && !this->report.reconCompared)
    this->startWorker(false, true);
}

void StreamDiffWindow::startWorker(const bool headersAndBlocks, const bool recon)
{
  this->running              = true;
  this->cancelRequested      = false;
  this->reconCancelRequested = false;
  this->updateControls();
  this->setBusy(QString("Comparing …\n  A  %1\n  B  %2")
                    .arg(QFileInfo(this->nameA).fileName())
                    .arg(QFileInfo(this->nameB).fileName()));

  /* Parsing and decoding take long enough to freeze the window, so they run on their own thread.
   * Everything it touches - the parsers, the private playlist items - is created and destroyed
   * there, and only plain data crosses back, through a queued signal.
   *
   * QThread rather than std::thread: the parser and the FFmpeg handler are QObjects that set up
   * timers and socket notifiers, and Qt refuses those on a thread it did not start ("Timers can
   * only be used with threads started with QThread"). It happened to finish anyway, which is the
   * worst kind of working.
   */
  this->worker = QThread::create(
      [this, headersAndBlocks, recon]
      {
        const auto say = [this](const std::string &text) {
          emit this->progressChanged(QString::fromStdString(text));
        };

        FFmpeg::FFmpegVersionHandler ff;
        ff.loadFFmpegLibraries();
        if (!ff.loadingSuccessfull())
        {
          this->workerError = "The FFmpeg libraries could not be loaded.";
          emit this->comparisonFinished();
          return;
        }

        if (headersAndBlocks)
        {
          // ---- steps 1-2: header syntax
          say("steps 1-2: parsing both streams");
          std::vector<bda::diff::SyntaxSection> a, b;
          DisplayMap                            displayOf;
          if (!readSections(this->nameA, a, &displayOf))
          {
            this->workerError = "Could not parse " + QFileInfo(this->nameA).fileName();
            emit this->comparisonFinished();
            return;
          }
          if (!readSections(this->nameB, b, nullptr))
          {
            this->workerError = "Could not parse " + QFileInfo(this->nameB).fileName();
            emit this->comparisonFinished();
            return;
          }
          this->report.elementsA = countElements(a);
          this->report.elementsB = countElements(b);
          this->report.headers   = bda::diff::compareSections(a, b);

          // ---- step 3: OBU payloads as bytes
          say("step 3: comparing OBU payloads");
          QByteArray dataA, dataB;
          if (readFile(this->nameA, dataA) && readFile(this->nameB, dataB))
          {
            const auto *pa = reinterpret_cast<const std::uint8_t *>(dataA.constData());
            const auto *pb = reinterpret_cast<const std::uint8_t *>(dataB.constData());
            const auto scanA = bda::diff::scanObus(pa, std::size_t(dataA.size()));
            const auto scanB = bda::diff::scanObus(pb, std::size_t(dataB.size()));
            if (scanA.ok && scanB.ok)
            {
              this->report.payloadCompared = true;
              this->report.obusA           = scanA.obus.size();
              this->report.obusB           = scanB.obus.size();
              this->report.payload = bda::diff::comparePayloads(scanA.obus, pa, scanB.obus, pb);
            }
            else
              this->report.blockNote = "Step 3 could not split the stream into OBUs: " +
                                       QString::fromStdString(scanA.ok ? scanB.error
                                                                       : scanA.error);
          }
          else
            this->report.blockNote = "Step 3 could not read the files.";

          // ---- step 4: the frame step 3 named, block by block
          if (const auto *first = this->report.payload.first();
              first != nullptr && !this->cancelRequested)
          {
            const CodedFrameId id{int(first->a.temporalUnit), int(first->a.indexInUnit)};
            const auto         it = displayOf.find(id);
            if (it == displayOf.end())
              this->report.blockNote =
                  QString("The first differing OBU (%1) is not a frame that is ever displayed, "
                          "so there is no picture to compare blocks in.")
                      .arg(QString::fromStdString(first->a.label()));
            else
            {
              BlockStepOptions options;
              options.cancel   = &this->cancelRequested;
              options.progress = [say](const std::string &text) { say("step 4: " + text); };
              this->report.blocks = runBlockStep(this->nameA, this->nameB, it->second, options);
              this->report.blockCompared = this->report.blocks.ok();
              if (!this->report.blocks.error.empty())
                this->report.blockNote =
                    "Step 4 failed: " + QString::fromStdString(this->report.blocks.error);
            }
          }
        }

        // ---- D: reconstructed pictures
        if (recon && !this->cancelRequested && !this->reconCancelRequested)
        {
          ReconStepOptions options;
          options.cancel   = &this->reconCancelRequested;
          options.progress = [say](const std::string &text) { say("pictures: " + text); };
          this->report.recon = runReconStep(this->nameA, this->nameB, options);
          this->report.reconCompared = this->report.recon.ok();
        }
        emit this->comparisonFinished();
      });
  connect(this->worker, &QThread::finished, this->worker, &QObject::deleteLater);
  this->worker->start();
}

void StreamDiffWindow::showProgress(const QString &text)
{
  if (this->running)
    this->progress->setText(text);
}

void StreamDiffWindow::showResult()
{
  this->running = false;
  this->progress->setVisible(false);

  if (!this->workerError.isEmpty())
  {
    this->summary->setText(this->workerError);
    this->workerError.clear();
    this->updateControls();
    return;
  }

  QString text = QString("A  %1\nB  %2\n")
                     .arg(QFileInfo(this->nameA).fileName())
                     .arg(QFileInfo(this->nameB).fileName());

  this->tree->clear();
  this->addHeaderSection(text);
  this->addPayloadSection(text);
  this->addBlockSection(text);
  this->addReconSection(text);
  if (this->cancelRequested)
    text += "\nStopped before the end - the sections above are what finished.";
  this->summary->setText(text);

  // The answer is the one worth reading: open the block section if there is one, else the first.
  for (int i = this->tree->topLevelItemCount() - 1; i >= 0; --i)
  {
    auto *section = this->tree->topLevelItem(i);
    if (section->data(0, Qt::UserRole + 2).toBool() || i == 0)
    {
      section->setExpanded(true);
      this->tree->setCurrentItem(section);
      break;
    }
  }
  this->updateControls();
}

void StreamDiffWindow::addHeaderSection(QString &text)
{
  const auto &result = this->report.headers;
  auto *      node   = new QTreeWidgetItem(this->tree);
  node->setText(0, "Header syntax (steps 1-2)");

  if (result.sections.empty())
  {
    node->setText(1, "not compared");
    return;
  }
  if (result.identical())
  {
    node->setText(1, QString("identical, %1 / %2 elements")
                         .arg(this->report.elementsA)
                         .arg(this->report.elementsB));
    text += "\nHeaders: identical.";
    return;
  }

  std::size_t differingSections = 0;
  for (const auto &section : result.sections)
    if (section.differs())
      ++differingSections;
  const auto *first = result.firstDiffering();
  node->setText(1, QString("%1 differences in %2 of %3 OBUs")
                       .arg(result.totalDiffs)
                       .arg(differingSections)
                       .arg(result.sections.size()));
  text += QString("\nHeaders: first divergence in %1.")
              .arg(QString::fromStdString(first ? first->label : std::string()));

  for (const auto &section : result.sections)
  {
    if (!section.differs())
      continue;
    auto *row = new QTreeWidgetItem(node);
    row->setText(0, QString::fromStdString(section.label));
    if (section.onlyInA)
      row->setText(1, "only in A");
    else if (section.onlyInB)
      row->setText(1, "only in B");
    else
    {
      row->setText(1, QString("%1 differences").arg(section.result.diffs.size()));
      for (const auto &d : section.result.diffs)
      {
        auto *child = new QTreeWidgetItem(row);
        child->setText(0, QString::fromStdString(d.name));
        child->setText(1, kindText(d.kind));
        if (d.kind != bda::diff::DiffKind::OnlyInB)
          child->setText(2, QString::fromStdString(d.valueA));
        if (d.kind != bda::diff::DiffKind::OnlyInA)
          child->setText(3, QString::fromStdString(d.valueB));
      }
    }
  }
}

void StreamDiffWindow::addPayloadSection(QString &text)
{
  auto *node = new QTreeWidgetItem(this->tree);
  node->setText(0, "OBU payload (step 3)");
  if (!this->report.payloadCompared)
  {
    node->setText(1, "not compared");
    return;
  }
  const auto &result = this->report.payload;
  if (result.identical())
  {
    node->setText(1, QString("every payload identical, %1 OBUs").arg(result.comparedObus));
    text += "\nOBU payloads: identical.";
    return;
  }

  const auto *first = result.first();
  node->setText(1, QString("%1 of %2 OBUs differ").arg(result.diffs.size()).arg(result.comparedObus));
  node->setText(2, QString::fromStdString(first->a.label()));
  text += QString("\nOBU payloads: first difference in %1, %2, from payload byte %3.")
              .arg(QString::fromStdString(first->a.label()))
              .arg(payloadKindText(first->kind))
              .arg(first->firstDifferingByte);

  std::size_t shown = 0;
  for (const auto &d : result.diffs)
  {
    if (shown++ >= MaxRows)
    {
      auto *more = new QTreeWidgetItem(node);
      more->setText(0, QString("… %1 more").arg(result.diffs.size() - MaxRows));
      break;
    }
    auto *row = new QTreeWidgetItem(node);
    row->setText(0, QString::fromStdString(d.a.label()));
    row->setText(1, QString("%1, first diff at byte %2")
                        .arg(payloadKindText(d.kind))
                        .arg(d.firstDifferingByte));
    row->setText(2, QString("%1 bytes").arg(d.a.payloadSize));
    row->setText(3, QString("%1 bytes").arg(d.b.payloadSize));
  }
}

void StreamDiffWindow::addBlockSection(QString &text)
{
  const auto &step = this->report.blocks;
  auto *      node = new QTreeWidgetItem(this->tree);
  node->setText(0, step.frameIdx >= 0 ? QString("Blocks, frame %1 (step 4)").arg(step.frameIdx)
                                      : QString("Blocks (step 4)"));
  if (!this->report.blockCompared)
  {
    node->setText(1, this->report.blockNote.isEmpty()
                         ? (this->report.payload.identical() ? "nothing to compare"
                                                             : "not compared")
                         : this->report.blockNote);
    if (!this->report.blockNote.isEmpty())
      text += "\n" + this->report.blockNote;
    return;
  }

  node->setData(0, Qt::UserRole + 2, true); // the answer lives here; showResult() opens it
  const auto *firstSb = step.firstWithBlockDiff();
  if (firstSb == nullptr)
  {
    node->setText(1, "no block syntax differs");
    text += QString("\nBlocks: every block in frame %1 is identical. The divergence is in syntax "
                    "the decoder does not export per block.")
                .arg(step.frameIdx);
    return;
  }

  const auto &firstBlock = firstSb->blocks.front();
  node->setText(1, QString("%1, first block MI(%2, %3)")
                       .arg(sbText(*firstSb))
                       .arg(firstBlock.miRow)
                       .arg(firstBlock.miCol));
  text += QString("\nBlocks: frame %1, first differing superblock %2, first block MI(%3, %4) "
                  "[%5]. Double-click a block to go there.")
              .arg(step.frameIdx)
              .arg(sbText(*firstSb))
              .arg(firstBlock.miRow)
              .arg(firstBlock.miCol)
              .arg(bda::diff::blockDiffKindName(firstBlock.kind));
  if (const auto *totalsOnly = step.firstTotalsOnlyBefore())
    text += QString("\n  Earlier, %1 differs in superblock totals only (same decisions, "
                    "different residual).")
                .arg(sbText(*totalsOnly));

  for (const auto &sb : step.differing)
  {
    auto *sbRow = new QTreeWidgetItem(node);
    sbRow->setText(0, sbText(sb));
    sbRow->setText(1, sb.hasBlockDiff() ? QString("%1 differing blocks").arg(sb.blocks.size())
                                        : QString("superblock totals only"));
    setLocation(sbRow, step.frameIdx,
                QPoint(sb.sbCol * step.sbSize, sb.sbRow * step.sbSize));
    for (const auto &d : sb.superblock.diffs)
    {
      auto *row = new QTreeWidgetItem(sbRow);
      row->setText(0, QString::fromStdString(d.name) + " (superblock)");
      row->setText(1, kindText(d.kind));
      row->setText(2, QString::fromStdString(d.valueA));
      row->setText(3, QString::fromStdString(d.valueB));
    }
    for (const auto &blk : sb.blocks)
    {
      auto *row = new QTreeWidgetItem(sbRow);
      row->setText(0, QString("MI(%1, %2)").arg(blk.miRow).arg(blk.miCol));
      row->setText(1, bda::diff::blockDiffKindName(blk.kind));
      const auto geometry = [](const bda::diff::BlockSyntax &b) {
        return b.valid ? QString("%1x%2 at (%3, %4)").arg(b.w).arg(b.h).arg(b.x).arg(b.y)
                       : QString("no block");
      };
      row->setText(2, geometry(blk.a));
      row->setText(3, geometry(blk.b));
      setLocation(row, step.frameIdx, QPoint(blk.miCol * MiSize, blk.miRow * MiSize));
      for (const auto &d : blk.syntax.diffs)
      {
        auto *child = new QTreeWidgetItem(row);
        child->setText(0, QString::fromStdString(d.name));
        child->setText(1, kindText(d.kind));
        if (d.kind != bda::diff::DiffKind::OnlyInB)
          child->setText(2, QString::fromStdString(d.valueA));
        if (d.kind != bda::diff::DiffKind::OnlyInA)
          child->setText(3, QString::fromStdString(d.valueB));
      }
    }
    if (&sb == firstSb)
      sbRow->setExpanded(true);
  }
}

void StreamDiffWindow::addReconSection(QString &text)
{
  if (!this->report.reconCompared)
  {
    const auto &recon = this->report.recon;
    if (!recon.error.empty() || recon.cancelled)
    {
      auto *node = new QTreeWidgetItem(this->tree);
      node->setText(0, "Reconstructed pictures (D)");
      node->setText(1, recon.cancelled
                           ? QString("stopped after %1 frames").arg(recon.frames.size())
                           : "failed: " + QString::fromStdString(recon.error));
    }
    return;
  }

  const auto &step  = this->report.recon;
  auto *      node  = new QTreeWidgetItem(this->tree);
  const auto  first = step.firstDiffering();
  node->setText(0, "Reconstructed pictures (D)");
  if (first < 0)
  {
    node->setText(1, QString("all %1 frames identical").arg(step.frames.size()));
    text += "\nPictures: every frame reconstructs identically.";
    return;
  }

  std::size_t differing = 0;
  for (const auto &f : step.frames)
    if (!f.diff.identical())
      ++differing;
  node->setText(1, QString("first differing frame %1, %2 of %3 differ")
                       .arg(step.frames[first].frameIdx)
                       .arg(differing)
                       .arg(step.frames.size()));
  text += QString("\nPictures: first differing frame %1 (%2 of %3 differ).")
              .arg(step.frames[first].frameIdx)
              .arg(differing)
              .arg(step.frames.size());

  std::size_t shown = 0;
  for (const auto &f : step.frames)
  {
    if (f.diff.identical())
      continue;
    if (shown++ >= MaxRows)
    {
      auto *more = new QTreeWidgetItem(node);
      more->setText(0, QString("… %1 more").arg(differing - MaxRows));
      break;
    }
    QStringList psnr;
    for (std::size_t p = 0; p < f.diff.planes.size(); ++p)
      psnr << QString("%1 %2").arg(QString::fromStdString(step.layout.planes[p].name))
                  .arg(QString::fromStdString(
                      formatPlanePsnr(f.diff.planes[p], step.layout.bitDepth)));
    const auto &luma = f.diff.planes.front();
    auto *      row  = new QTreeWidgetItem(node);
    row->setText(0, QString("frame %1").arg(f.frameIdx));
    row->setText(1, psnr.join("   "));
    if (!luma.identical())
    {
      row->setText(2, QString("Y SSE %1, %2 samples differ").arg(luma.sse).arg(luma.differingSamples));
      row->setText(3, QString("first at pixel (%1, %2)").arg(*luma.firstX).arg(*luma.firstY));
      setLocation(row, f.frameIdx, QPoint(*luma.firstX, *luma.firstY));
    }
    else
      setLocation(row, f.frameIdx, std::nullopt);
  }
}

std::optional<StreamDiffLocation> StreamDiffWindow::firstDifference() const
{
  if (this->report.blockCompared)
    if (const auto *sb = this->report.blocks.firstWithBlockDiff())
    {
      const auto &blk = sb->blocks.front();
      return StreamDiffLocation{this->report.blocks.frameIdx,
                                QPoint(blk.miCol * MiSize, blk.miRow * MiSize)};
    }
  if (this->report.reconCompared)
    if (const auto first = this->report.recon.firstDiffering(); first >= 0)
    {
      const auto &f    = this->report.recon.frames[first];
      const auto &luma = f.diff.planes.front();
      std::optional<QPoint> pixel;
      if (!luma.identical())
        pixel = QPoint(*luma.firstX, *luma.firstY);
      return StreamDiffLocation{f.frameIdx, pixel};
    }
  return std::nullopt;
}

void StreamDiffWindow::activateRow(QTreeWidgetItem *row)
{
  // A syntax element row names no place of its own; its block row does.
  for (auto *r = row; r != nullptr; r = r->parent())
    if (r->data(0, RoleFrame).isValid())
    {
      StreamDiffLocation where;
      where.frameIdx = r->data(0, RoleFrame).toInt();
      if (r->data(0, RolePixel).isValid())
        where.pixel = r->data(0, RolePixel).toPoint();
      this->locate(where);
      return;
    }
}

void StreamDiffWindow::locate(const StreamDiffLocation &where)
{
  if (!this->itemA || !this->itemB)
  {
    this->summary->setText(this->summary->text() +
                           "\n\nOne of the two streams has been removed from the playlist, so "
                           "there is nothing to show this place in.");
    return;
  }
  this->lastLocation = where;

  const bool blockOnB  = this->blockInfoChoice->checkedId() == 1;
  const bool syntaxOnB = this->syntaxInfoChoice->checkedId() == 1;
  auto *     block     = blockOnB ? this->itemB.data() : this->itemA.data();
  auto *     other     = blockOnB ? this->itemA.data() : this->itemB.data();
  auto *     syntax    = syntaxOnB ? this->itemB.data() : this->itemA.data();
  emit locateRequested(block, other, syntax, where.frameIdx,
                       where.pixel.value_or(QPoint(-1, -1)));
}

void StreamDiffWindow::setBlockInfoStream(const int stream)
{
  this->blockInfoChoice->button(stream == 1 ? 1 : 0)->click();
}

void StreamDiffWindow::setSyntaxInfoStream(const int stream)
{
  this->syntaxInfoChoice->button(stream == 1 ? 1 : 0)->click();
}

} // namespace bda::integration
