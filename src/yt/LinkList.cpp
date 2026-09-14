#include "yt/LinkList.h"

#include <algorithm>
#include <sstream>

namespace bda::yt
{

namespace
{

constexpr std::size_t kVideoIdLength = 11;

std::string trim(const std::string &text)
{
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

/* Strip what people leave behind when a list is pasted out of somewhere else: wrapping quotes and
 * a trailing comma. The script tolerates both, so the pane has to read them the same way or a
 * line that encodes fine would look invalid on screen.
 */
std::string stripDecoration(std::string value)
{
  while (!value.empty() && value.back() == ',')
    value.pop_back();
  value = trim(value);

  if (value.size() >= 2)
  {
    const auto front = value.front();
    if ((front == '"' || front == '\'') && value.back() == front)
      value = trim(value.substr(1, value.size() - 2));
  }
  return value;
}

bool isVideoIdCharacter(char character)
{
  return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') || character == '-' || character == '_';
}

} // namespace

bool looksLikeVideoLink(const std::string &candidate)
{
  const auto value = trim(candidate);
  if (value.empty())
    return false;

  if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0)
    return true;

  if (value.size() != kVideoIdLength)
    return false;
  return std::all_of(value.begin(), value.end(), isVideoIdCharacter);
}

std::vector<LinkEntry> parseLinkList(const std::string &text)
{
  std::vector<LinkEntry> entries;

  std::istringstream stream(text);
  std::string        line;
  while (std::getline(stream, line))
  {
    LinkEntry entry;

    /* The first '#' ends the link, wherever it is. A URL cannot contain one unescaped, and the
     * list format says a trailing comment is stripped - so this is the whole rule.
     */
    const auto hash = line.find('#');
    std::string beforeComment = hash == std::string::npos ? line : line.substr(0, hash);
    if (hash != std::string::npos)
      entry.comment = trim(line.substr(hash + 1));

    const auto value = stripDecoration(beforeComment);
    if (value.empty())
      entry.blank = hash == std::string::npos;
    else
      entry.link = value;

    entries.push_back(entry);
  }
  return entries;
}

std::vector<std::string> linksOnly(const std::vector<LinkEntry> &entries)
{
  std::vector<std::string> links;
  for (const auto &entry : entries)
    if (entry.isLink())
      links.push_back(entry.link);
  return links;
}

std::string renderLinkList(const std::vector<LinkEntry> &entries)
{
  std::string text;
  for (const auto &entry : entries)
  {
    if (entry.isLink())
    {
      text += entry.link;
      if (!entry.comment.empty())
        text += "          # " + entry.comment;
    }
    else if (!entry.comment.empty())
    {
      text += "# " + entry.comment;
    }
    text += "\n";
  }
  return text;
}

} // namespace bda::yt
