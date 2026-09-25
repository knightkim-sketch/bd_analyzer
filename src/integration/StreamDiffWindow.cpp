#include "StreamDiffWindow.h"

#include <QAbstractItemModel>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QVBoxLayout>

#include <QThread>

#include <vector>

#include "ffmpeg/FFmpegVersionHandler.h"
#include "parser/AVFormat/ParserAVFormat.h"
#include "playlistitem/playlistItem.h"
#include "playlistitem/playlistItemCompressedVideo.h"

namespace bda::integration
{

namespace
{

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
 */
bool readSections(const QString &path, std::vector<bda::diff::SyntaxSection> &out)
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

std::size_t countElements(const std::vector<bda::diff::SyntaxSection> &sections)
{
  std::size_t n = 0;
  for (const auto &s : sections)
    n += s.elements.size();
  return n;
}

} // namespace

StreamDiffWindow::StreamDiffWindow(QWidget *parent) : QDialog(parent)
{
  this->setWindowTitle("Find diff");
  this->resize(1000, 700);

  auto *layout = new QVBoxLayout(this);

  this->summary = new QLabel(this);
  this->summary->setWordWrap(true);
  this->summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(this->summary);

  this->tree = new QTreeWidget(this);
  this->tree->setColumnCount(4);
  this->tree->setHeaderLabels({"Syntax element", "Difference", "A", "B"});
  this->tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  this->tree->setAlternatingRowColors(true);
  layout->addWidget(this->tree, 1);

  /* The remaining layers are designed but not wired. Showing them disabled, with the reason, is
   * the honest state: a window that silently offers only header comparison reads as if the two
   * streams agreed everywhere else.
   */
  this->compareCdf = new QCheckBox("Compare CDF (entropy coder state)", this);
  this->compareCdf->setEnabled(false);
  this->compareCdf->setToolTip("Not available: the analyzer decoder exports block data and bit "
                               "ranges, but not the adaptive probability tables.");
  layout->addWidget(this->compareCdf);

  this->compareRecon = new QCheckBox("Compare reconstructed pictures", this);
  this->compareRecon->setEnabled(false);
  this->compareRecon->setToolTip("Not wired yet - planned as layer D of the Find diff design.");
  layout->addWidget(this->compareRecon);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(buttons);

  connect(this, &StreamDiffWindow::comparisonFinished, this, &StreamDiffWindow::showResult,
          Qt::QueuedConnection);
}

void StreamDiffWindow::setBusy(const QString &what)
{
  this->summary->setText(what);
  this->tree->clear();
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

  this->nameA = streams[0]->properties().name;
  this->nameB = streams[1]->properties().name;
  if (this->nameA == this->nameB)
    return {false, "Both selected items are the same file."};

  this->running = true;
  this->setBusy(QString("Parsing and comparing …\n  A  %1\n  B  %2")
                    .arg(QFileInfo(this->nameA).fileName())
                    .arg(QFileInfo(this->nameB).fileName()));

  /* Parsing a whole stream takes long enough to freeze the window, so it runs on its own thread.
   * Everything it touches - the parser and its model - is created and destroyed there, and only
   * plain data crosses back, through a queued signal.
   *
   * QThread rather than std::thread: the parser and the FFmpeg handler are QObjects that set up
   * timers and socket notifiers, and Qt refuses those on a thread it did not start ("Timers can
   * only be used with threads started with QThread"). It happened to finish anyway, which is the
   * worst kind of working.
   */
  auto *worker = QThread::create(
      [this]
      {
        FFmpeg::FFmpegVersionHandler ff;
        ff.loadFFmpegLibraries();
        if (!ff.loadingSuccessfull())
        {
          this->workerError = "The FFmpeg libraries could not be loaded.";
          emit this->comparisonFinished();
          return;
        }

        std::vector<bda::diff::SyntaxSection> a, b;
        if (!readSections(this->nameA, a))
          this->workerError = "Could not parse " + QFileInfo(this->nameA).fileName();
        else if (!readSections(this->nameB, b))
          this->workerError = "Could not parse " + QFileInfo(this->nameB).fileName();
        else
        {
          this->elementsA = countElements(a);
          this->elementsB = countElements(b);
          this->result    = bda::diff::compareSections(a, b);
        }
        emit this->comparisonFinished();
      });
  connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();

  return {true, {}};
}

void StreamDiffWindow::showResult()
{
  this->running = false;

  if (!this->workerError.isEmpty())
  {
    this->summary->setText(this->workerError);
    this->workerError.clear();
    return;
  }

  QString text = QString("A  %1  (%2 syntax elements)\nB  %3  (%4 syntax elements)\n")
                     .arg(QFileInfo(this->nameA).fileName())
                     .arg(this->elementsA)
                     .arg(QFileInfo(this->nameB).fileName())
                     .arg(this->elementsB);

  if (this->result.identical())
  {
    text += "\nThe header syntax is identical.\n\nThat does not mean the streams are the same: "
            "everything inside the tile payload is still uncompared. Where two encodings differ "
            "only in how blocks were coded, this is exactly what you see.";
    this->summary->setText(text);
    this->tree->clear();
    return;
  }

  std::size_t differingSections = 0;
  for (const auto &section : this->result.sections)
    if (section.differs())
      ++differingSections;

  const auto *first = this->result.firstDiffering();
  text += QString("\n%1 differences across %2 of %3 OBUs.\nFirst divergence in %4.")
              .arg(this->result.totalDiffs)
              .arg(differingSections)
              .arg(this->result.sections.size())
              .arg(QString::fromStdString(first ? first->label : std::string()));
  this->summary->setText(text);

  this->tree->clear();
  for (const auto &section : this->result.sections)
  {
    if (!section.differs())
      continue;

    auto *node = new QTreeWidgetItem(this->tree);
    node->setText(0, QString::fromStdString(section.label));
    if (section.onlyInA)
      node->setText(1, "only in A");
    else if (section.onlyInB)
      node->setText(1, "only in B");
    else
    {
      node->setText(1, QString("%1 differences").arg(section.result.diffs.size()));
      for (const auto &d : section.result.diffs)
      {
        auto *child = new QTreeWidgetItem(node);
        child->setText(0, QString::fromStdString(d.name));
        child->setText(1, kindText(d.kind));
        if (d.kind != bda::diff::DiffKind::OnlyInB)
          child->setText(2, QString::fromStdString(d.valueA));
        if (d.kind != bda::diff::DiffKind::OnlyInA)
          child->setText(3, QString::fromStdString(d.valueB));
      }
    }
  }
  // The first divergence is the one worth reading; everything after it may only be its wake.
  if (this->tree->topLevelItemCount() > 0)
  {
    this->tree->topLevelItem(0)->setExpanded(true);
    this->tree->setCurrentItem(this->tree->topLevelItem(0));
  }
}

} // namespace bda::integration
