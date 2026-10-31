#include "OverlayControls.h"

#include <QAction>
#include <QApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>
#include <QSettings>

#include <algorithm>
#include <mutex>
#include <optional>

#include "common/FunctionsGui.h"
#include "playlistitem/playlistItem.h"
#include "statistics/StatisticUIHandler.h"
#include "statistics/StatisticsData.h"
#include "statistics/StatisticsType.h"

namespace bda::integration
{

namespace
{

const char *kLegendSettingKey = "View/ShowOverlayLegend";
// Read on first use: at static initialisation the application name, and so the settings file, is
// not set yet.
std::optional<bool> legendEnabled;

// The item's own statistics and its motion estimation overlays (ME, GM) - two separate handlers.
std::vector<stats::StatisticUIHandler *> overlayHandlers(playlistItem *item)
{
  std::vector<stats::StatisticUIHandler *> handlers;
  if (!item)
    return handlers;
  for (auto *handler : {item->getStatisticsUIHandler(), item->getMotionEstimationUIHandler()})
    if (handler && handler->getStatisticsData() &&
        std::find(handlers.begin(), handlers.end(), handler) == handlers.end())
      handlers.push_back(handler);
  return handlers;
}

QString groupOf(const stats::StatisticsType &type)
{
  return type.uiGroup.isEmpty() ? type.typeName : type.uiGroup;
}

// The colour that stands for a type in a button or a legend line.
QColor representativeColor(const stats::StatisticsType &type)
{
  using stats::color::MappingType;
  if (type.hasVectorData && type.renderVectorData && !type.mapVectorToColor)
    return functionsGui::toQColor(type.vectorStyle.color);
  if (type.hasValueData && type.renderValueData)
  {
    const auto &mapper = type.colorMapper;
    if (mapper.mappingType == MappingType::Map && !mapper.colorMap.empty())
      return functionsGui::toQColor(mapper.colorMap.begin()->second);
    return functionsGui::toQColor(mapper.getColor(mapper.valueRange.max));
  }
  return functionsGui::toQColor(type.gridStyle.color);
}

QIcon swatchIcon(const QColor &color)
{
  QPixmap pixmap(14, 14);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setPen(QPen(Qt::black, 1));
  painter.setBrush(color);
  painter.drawRect(0, 0, 13, 13);
  return QIcon(pixmap);
}

/* What one type adds to the legend. A colour map gets a swatch per named value, a gradient a bar
 * with its range, a vector a line in its colour.
 */
struct LegendLine
{
  QString                             label;
  std::vector<std::pair<QColor, QString>> swatches;
  bool                                gradient{};
  QColor                              gradientFrom, gradientTo;
  QString                             rangeText;
  std::optional<QColor>               line;
};

std::vector<LegendLine> legendLines(playlistItem *item)
{
  using stats::color::MappingType;
  std::vector<LegendLine> lines;
  for (auto *handler : overlayHandlers(item))
  {
    auto                        *data = handler->getStatisticsData();
    std::unique_lock<std::mutex> lock(data->accessMutex);
    for (const auto &type : data->getStatisticsTypes())
    {
      if (!type.render)
        continue;
      LegendLine line;
      line.label = type.typeName;
      if (type.hasValueData && type.renderValueData)
      {
        const auto &mapper = type.colorMapper;
        if (mapper.mappingType == MappingType::Map)
        {
          for (const auto &[value, color] : mapper.colorMap)
          {
            const auto name = type.getMappedValue(value);
            line.swatches.push_back(
                {functionsGui::toQColor(color), name.isEmpty() ? QString::number(value) : name});
          }
        }
        else
        {
          // The range the last paint used: fitted to the frame, or the type's fixed one.
          auto shown = mapper;
          if (type.fittedRange)
            shown.valueRange = *type.fittedRange;
          line.gradient     = true;
          line.gradientFrom = functionsGui::toQColor(shown.getColor(shown.valueRange.min));
          line.gradientTo   = functionsGui::toQColor(shown.getColor(shown.valueRange.max));
          line.rangeText    = QString("%1 %2 .. %3")
                               .arg(type.fittedRange ? "frame" : "fixed")
                               .arg(shown.valueRange.min)
                               .arg(shown.valueRange.max);
        }
      }
      if (type.hasVectorData && type.renderVectorData)
        line.line = type.mapVectorToColor ? QColor(Qt::white)
                                          : functionsGui::toQColor(type.vectorStyle.color);
      if (line.swatches.empty() && !line.gradient && !line.line)
        line.line = functionsGui::toQColor(type.gridStyle.color);
      lines.push_back(line);
    }
  }
  return lines;
}

/* One toolbar button: a row of the properties panel. Copied under the data's mutex - a decoder
 * thread fills the types while the GUI reads them.
 */
struct OverlayGroup
{
  stats::StatisticUIHandler *handler{};
  QString                    name;
  QString                    description;
  QColor                     color;
  bool                       rendered{};
};

std::vector<OverlayGroup> overlayGroups(playlistItem *item)
{
  std::vector<OverlayGroup> groups;
  for (auto *handler : overlayHandlers(item))
  {
    auto                        *data = handler->getStatisticsData();
    std::unique_lock<std::mutex> lock(data->accessMutex);
    for (const auto &type : data->getStatisticsTypes())
    {
      const auto name = groupOf(type);
      if (std::any_of(groups.begin(), groups.end(), [&](const OverlayGroup &g) {
            return g.handler == handler && g.name == name;
          }))
        continue;
      groups.push_back({handler, name, type.description, representativeColor(type), type.render});
    }
  }
  return groups;
}

} // namespace

bool overlayLegendEnabled()
{
  if (!legendEnabled)
    legendEnabled = QSettings().value(kLegendSettingKey, true).toBool();
  return *legendEnabled;
}

void setOverlayLegendEnabled(bool enabled)
{
  if (legendEnabled == enabled)
    return;
  legendEnabled = enabled;
  QSettings().setValue(kLegendSettingKey, enabled);
}

void paintOverlayLegend(QPainter &painter, playlistItem *item, const QRect &area)
{
  if (!overlayLegendEnabled() || !item)
    return;
  const auto lines = legendLines(item);
  if (lines.empty())
    return;

  painter.save();
  QFont font = QApplication::font();
  font.setPointSize(9);
  painter.setFont(font);
  const QFontMetrics metrics(font);
  const int          rowHeight = metrics.height() + 4;
  const int          swatch    = metrics.height() - 2;
  const int          pad       = 6;

  // Measure: the label, then its swatches / bar / line on the same row.
  int width = 0;
  for (const auto &line : lines)
  {
    int w = metrics.horizontalAdvance(line.label) + pad;
    for (const auto &[color, name] : line.swatches)
      w += swatch + 3 + metrics.horizontalAdvance(name) + pad;
    if (line.gradient)
      w += 60 + 4 + metrics.horizontalAdvance(line.rangeText) + pad;
    if (line.line)
      w += 24 + pad;
    width = std::max(width, w);
  }
  const QRect box(std::max(area.left() + 8, area.right() - width - 2 * pad - 8),
                  area.top() + 8,
                  width + 2 * pad,
                  int(lines.size()) * rowHeight + pad);

  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0, 0, 0, 170));
  painter.drawRect(box);

  int y = box.top() + pad / 2;
  for (const auto &line : lines)
  {
    int       x    = box.left() + pad;
    const int midY = y + rowHeight / 2;
    painter.setPen(Qt::white);
    painter.drawText(QRect(x, y, width, rowHeight), Qt::AlignVCenter, line.label);
    x += metrics.horizontalAdvance(line.label) + pad;
    for (const auto &[color, name] : line.swatches)
    {
      painter.setPen(QPen(Qt::white, 1));
      painter.setBrush(color);
      painter.drawRect(x, midY - swatch / 2, swatch, swatch);
      x += swatch + 3;
      painter.drawText(QRect(x, y, width, rowHeight), Qt::AlignVCenter, name);
      x += metrics.horizontalAdvance(name) + pad;
    }
    if (line.gradient)
    {
      QLinearGradient gradient(x, 0, x + 60, 0);
      gradient.setColorAt(0, line.gradientFrom);
      gradient.setColorAt(1, line.gradientTo);
      painter.setPen(QPen(Qt::white, 1));
      painter.setBrush(gradient);
      painter.drawRect(x, midY - swatch / 2, 60, swatch);
      x += 64;
      painter.drawText(QRect(x, y, width, rowHeight), Qt::AlignVCenter, line.rangeText);
      x += metrics.horizontalAdvance(line.rangeText) + pad;
    }
    if (line.line)
    {
      painter.setPen(QPen(*line.line, 3));
      painter.drawLine(x, midY, x + 24, midY);
    }
    y += rowHeight;
  }
  painter.restore();
}

OverlayToolBar::OverlayToolBar(QWidget *parent) : QToolBar(tr("Overlays"), parent)
{
  this->setObjectName("overlayToolBar");
  this->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  this->setIconSize(QSize(14, 14));
}

void OverlayToolBar::currentSelectedItemsChanged(playlistItem *item1, playlistItem *)
{
  this->item = item1;
  this->rebuild();
}

void OverlayToolBar::refresh()
{
  const auto groups = overlayGroups(this->item);
  QStringList names;
  for (const auto &group : groups)
    names.append(group.name);
  if (names != this->shownGroups)
  {
    this->rebuild();
    return;
  }
  const auto buttons = this->actions();
  for (int i = 0; i < int(groups.size()) && i < buttons.size(); ++i)
  {
    // Not a QSignalBlocker: the tool button follows the action through those same signals.
    this->syncing = true;
    buttons[i]->setChecked(groups[i].rendered);
    this->syncing = false;
  }
}

void OverlayToolBar::rebuild()
{
  this->syncing = true;
  /* clear() only removes the actions. Deleted later rather than now: a rebuild can run inside one
   * of these actions' own toggled() (button -> panel check box -> updateItem -> refresh).
   */
  const auto old = this->actions();
  this->clear();
  for (auto *action : old)
    action->deleteLater();
  this->shownGroups.clear();

  // The handlers' updateItem() is what a panel check box or a new ME run sends.
  for (auto *handler : overlayHandlers(this->item))
    connect(handler,
            &stats::StatisticUIHandler::updateItem,
            this,
            &OverlayToolBar::refresh,
            Qt::UniqueConnection);

  int number = 0;
  for (const auto &group : overlayGroups(this->item))
  {
    this->shownGroups.append(group.name);
    auto *action = this->addAction(swatchIcon(group.color), group.name);
    action->setCheckable(true);
    action->setChecked(group.rendered);
    action->setToolTip(group.description.isEmpty() ? group.name : group.description);
    if (number < 8)
    {
      action->setShortcut(QKeySequence(Qt::Key_F1 + number));
      action->setToolTip(QString("%1 (F%2)").arg(action->toolTip()).arg(number + 1));
      // Also on the main window, so F1 - F8 keep working with the toolbar hidden (full screen).
      if (auto *window = this->window(); window && window != this)
        window->addAction(action);
    }
    ++number;
    QPointer<stats::StatisticUIHandler> target(group.handler);
    connect(action, &QAction::toggled, this, [this, target, name = group.name](bool on) {
      if (target && !this->syncing)
        target->setOverlayGroupRendered(name, on);
    });
  }
  this->syncing = false;
}

} // namespace bda::integration
