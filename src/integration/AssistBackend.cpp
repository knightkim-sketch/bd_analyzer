#include "integration/AssistBackend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace bda::integration
{

QStringList readOnlyToolAllowlist()
{
  /* Read is the file reader; Bash is here because this CLI build has no Grep or Glob tool, so
   * searching a tree at all means a shell. What makes that acceptable is the layers around it -
   * plan mode, the permission rules, and a read-only mount namespace - not this list.
   */
  return {QStringLiteral("Read"), QStringLiteral("Bash")};
}

QStringList unexpectedTools(const QStringList &tools)
{
  const auto  allowed = readOnlyToolAllowlist();
  QStringList unexpected;
  for (const auto &tool : tools)
    if (!allowed.contains(tool))
      unexpected.append(tool);
  return unexpected;
}

QByteArray encodeUserMessage(const QString &text)
{
  QJsonObject block{{"type", "text"}, {"text", text}};
  QJsonObject message{{"role", "user"}, {"content", QJsonArray{block}}};
  QJsonObject envelope{{"type", "user"}, {"message", message}};

  auto line = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
  line.append('\n'); // The CLI reads one message per line; without this the turn never starts.
  return line;
}

AssistEvent parseAssistEvent(const QByteArray &line)
{
  AssistEvent event;

  const auto trimmed = line.trimmed();
  if (trimmed.isEmpty())
    return event; // Ignored.

  QJsonParseError error{};
  const auto      document = QJsonDocument::fromJson(trimmed, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
  {
    /* Not JSON. The CLI prints argument complaints and warnings as bare text on the same stream,
     * and those are the failures worth surfacing - a session that refused to start looks exactly
     * like a session that is thinking, unless we report this.
     */
    event.kind = AssistEvent::Kind::Failed;
    event.text = QString::fromUtf8(trimmed);
    return event;
  }

  const auto object    = document.object();
  const auto type      = object.value("type").toString();
  const auto subtype   = object.value("subtype").toString();
  event.sessionId      = object.value("session_id").toString();

  if (type == QLatin1String("system") && subtype == QLatin1String("init"))
  {
    event.kind           = AssistEvent::Kind::SessionStarted;
    event.permissionMode = object.value("permissionMode").toString();
    event.model          = object.value("model").toString();
    for (const auto tool : object.value("tools").toArray())
      event.tools.append(tool.toString());
    return event;
  }

  if (type == QLatin1String("stream_event"))
  {
    const auto inner = object.value("event").toObject();
    if (inner.value("type").toString() == QLatin1String("content_block_delta"))
    {
      const auto delta = inner.value("delta").toObject();
      if (delta.value("type").toString() == QLatin1String("text_delta"))
      {
        event.kind = AssistEvent::Kind::TextDelta;
        event.text = delta.value("text").toString();
        return event;
      }
    }
    return event; // Other stream events are structure we do not draw.
  }

  if (type == QLatin1String("result"))
  {
    const auto isError = object.value("is_error").toBool();
    event.kind    = isError ? AssistEvent::Kind::Failed : AssistEvent::Kind::TurnFinished;
    event.text    = object.value("result").toString();
    event.costUsd = object.value("total_cost_usd").toDouble();
    return event;
  }

  return event; // assistant / user / rate_limit_event and anything added later.
}

} // namespace bda::integration
