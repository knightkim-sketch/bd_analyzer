#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace bda::integration
{

/* A .bdsession file: enough to come back to an analysis - the playlist (which already carries the
 * frame on screen, zoom and split of every item), the perspective and the window's dock layout.
 * Deliberately minimal (docs/ai/30-designs/ui-redesign-decisions.md D-23). JSON:
 *   { "schema": 1, "playlist": "<playlist XML>", "perspective": "...",
 *     "windowState": "<base64 of QMainWindow::saveState>", "windowStateVersion": <int> }
 * The playlist's relative paths are relative to the session file, like a .yuvplaylist's.
 */
struct SessionData
{
  static constexpr int kSchema = 1;

  QString    playlistXml;
  QString    perspective;
  QByteArray windowState;
  int        windowStateVersion{};
};

QByteArray                 writeSession(const SessionData &session);
std::optional<SessionData> readSession(const QByteArray &json, QString *error = nullptr);

} // namespace bda::integration
