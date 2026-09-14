// Reading and writing the link list that yt-transcode.sh takes with -l.
//
// Qt-free, like src/bdrate and src/container, so the parsing rules can be tested on their own.
//
// The rules are the script's, not ours - the pane and the script have to agree on what a line
// means, or the list shown on screen is not the list that gets encoded. They come from the header
// of links.txt: one link or bare 11 character video ID per line, '#' starts a comment anywhere on
// the line, blank lines are ignored, and surrounding quotes and trailing commas are tolerated.
#pragma once

#include <string>
#include <vector>

namespace bda::yt
{

//!< One line of the list, kept whole so rewriting the file does not throw away the user's comments.
struct LinkEntry
{
  std::string link;    //!< The URL or bare ID, cleaned. Empty for a comment or blank line.
  std::string comment; //!< Anything from '#' onwards, without the '#'. Empty when there is none.
  bool        blank{}; //!< A line with nothing on it at all.

  bool isLink() const { return !this->link.empty(); }
};

/* Parse the whole file, line by line, keeping every line.
 *
 * Comments and blank lines are kept rather than dropped because this list is edited by hand as
 * well as by the pane - the file in the repository is half documentation - and a save that
 * silently deleted all of it would be a poor trade for adding one URL.
 */
std::vector<LinkEntry> parseLinkList(const std::string &text);

//!< Just the links, in order. What the pane shows and what the script is handed.
std::vector<std::string> linksOnly(const std::vector<LinkEntry> &entries);

/* Render entries back to file text, ending with a newline.
 *
 * parse -> render over an unmodified file reproduces it, so saving after no edit changes nothing.
 */
std::string renderLinkList(const std::vector<LinkEntry> &entries);

/* Is this something the script will accept?
 *
 * A full URL of any shape is accepted - the script hands those to yt-dlp, which knows far more
 * about YouTube's URL forms than we should try to. A bare ID has to be exactly the 11 characters
 * YouTube uses, because anything else would be sent as an ID and fail obscurely much later.
 */
bool looksLikeVideoLink(const std::string &candidate);

} // namespace bda::yt
