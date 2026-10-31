#include "SessionFile.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace bda::integration
{

QByteArray writeSession(const SessionData &session)
{
  QJsonObject root;
  root["schema"]             = SessionData::kSchema;
  root["playlist"]           = session.playlistXml;
  root["perspective"]        = session.perspective;
  root["windowState"]        = QString::fromLatin1(session.windowState.toBase64());
  root["windowStateVersion"] = session.windowStateVersion;
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::optional<SessionData> readSession(const QByteArray &json, QString *error)
{
  QJsonParseError parseError;
  const auto      document = QJsonDocument::fromJson(json, &parseError);
  if (!document.isObject())
  {
    if (error)
      *error = "Not a bd-analyzer session file: " + parseError.errorString();
    return std::nullopt;
  }
  const auto root = document.object();
  if (root["schema"].toInt() != SessionData::kSchema || !root["playlist"].isString())
  {
    if (error)
      *error = QString("Unsupported session file (schema %1, this version reads %2)")
                   .arg(root["schema"].toInt())
                   .arg(SessionData::kSchema);
    return std::nullopt;
  }
  SessionData session;
  session.playlistXml        = root["playlist"].toString();
  session.perspective        = root["perspective"].toString();
  session.windowState        = QByteArray::fromBase64(root["windowState"].toString().toLatin1());
  session.windowStateVersion = root["windowStateVersion"].toInt();
  return session;
}

} // namespace bda::integration
