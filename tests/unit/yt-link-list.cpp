// Unit test for the yt-transcode link list. No Qt, no library - see src/yt/LinkList.h.
//
// The rules being pinned are the *script's*, taken from the header of its own links.txt. If the
// pane and the script disagree about what a line means, the list on screen is not the list that
// gets encoded - a disabled line that the pane still queues, or a link the pane hides and the
// script happily downloads. Every case below is one the shipped example file actually contains.
#include <iostream>
#include <string>

#include "yt/LinkList.h"

namespace
{
int  g_failures = 0;
void check(bool ok, const std::string &what)
{
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
  if (!ok)
    ++g_failures;
}

template <typename T> void checkEqual(T got, T want, const std::string &what)
{
  const bool ok = got == want;
  std::cout << (ok ? "  ok    " : "  FAIL  ") << what;
  if (!ok)
    std::cout << "  (got '" << got << "', want '" << want << "')";
  std::cout << std::endl;
  if (!ok)
    ++g_failures;
}

using namespace bda::yt;
} // namespace

int main()
{
  std::cout << "yt link list" << std::endl;

  // --- what counts as a link ---------------------------------------------------------------
  {
    check(looksLikeVideoLink("https://youtu.be/cWSXtFMh0Yk"), "a youtu.be URL is a link");
    check(looksLikeVideoLink("https://www.youtube.com/watch?v=bGYi7eOoVJ4&list=PL&index=3"),
          "a watch URL with query parameters is a link");
    check(looksLikeVideoLink("dQw4w9WgXcQ"), "a bare 11 character id is a link");
    check(looksLikeVideoLink("a-b_c1234XY"), "ids may contain - and _");

    /* Length matters. Anything that is not a URL is handed to the script as an id, and a wrong
     * one fails deep inside yt-dlp with a message about the video not existing - so it is worth
     * refusing here, where the user can see which line is wrong.
     */
    check(!looksLikeVideoLink("dQw4w9WgXc"), "ten characters is not an id");
    check(!looksLikeVideoLink("dQw4w9WgXcQQ"), "twelve is not either");
    check(!looksLikeVideoLink("some random text"), "prose is not a link");
    check(!looksLikeVideoLink(""), "nor is nothing");
    check(!looksLikeVideoLink("dQw4w9WgXc!"), "nor an id with punctuation in it");
  }

  // --- the example file's own cases ---------------------------------------------------------
  {
    const std::string text =
        "# yt-transcode list file\n"
        "\n"
        "https://www.youtube.com/watch?v=bGYi7eOoVJ4&list=PLKtRB&index=3\n"
        "https://youtu.be/cWSXtFMh0Yk\n"
        "dQw4w9WgXcQ          # trailing comments are stripped\n"
        "# https://youtu.be/SomeVideoID\n";

    const auto entries = parseLinkList(text);
    const auto links   = linksOnly(entries);

    checkEqual(links.size(), std::size_t(3), "three links are found");
    checkEqual(links.at(2), std::string("dQw4w9WgXcQ"), "a trailing comment is stripped");

    /* The last line is a link that has been commented out. Treating '#' as a comment only at the
     * start of a line would be equally defensible and completely wrong here: the script disables
     * the line, so the pane must too, or it queues a download the user turned off.
     */
    for (const auto &link : links)
      check(link.find("SomeVideoID") == std::string::npos,
            "a commented out link is not queued: " + link);
  }

  // --- decoration people leave behind when pasting -------------------------------------------
  {
    const auto entries = parseLinkList("\"https://youtu.be/cWSXtFMh0Yk\",\n"
                                       "'dQw4w9WgXcQ'\n"
                                       "  https://youtu.be/aaaaaaaaaaa  \n");
    const auto links   = linksOnly(entries);
    checkEqual(links.size(), std::size_t(3), "three links survive their decoration");
    checkEqual(links.at(0), std::string("https://youtu.be/cWSXtFMh0Yk"),
               "quotes and a trailing comma are removed");
    checkEqual(links.at(1), std::string("dQw4w9WgXcQ"), "single quotes too");
    checkEqual(links.at(2), std::string("https://youtu.be/aaaaaaaaaaa"),
               "and surrounding whitespace");
  }

  // --- round trip ---------------------------------------------------------------------------
  {
    /* Saving a file the user has not edited must not rewrite it. The list in the repository is
     * half documentation, and a save that dropped the comments would be a poor trade for adding
     * one URL.
     */
    const std::string text = "# a heading\n"
                             "https://youtu.be/cWSXtFMh0Yk\n"
                             "dQw4w9WgXcQ          # a note\n";
    const auto        once = renderLinkList(parseLinkList(text));
    checkEqual(once, text, "parse then render reproduces the file");

    const auto twice = renderLinkList(parseLinkList(once));
    checkEqual(twice, once, "and is stable on a second pass");
  }
  {
    const auto entries = parseLinkList("# heading\n\nhttps://youtu.be/cWSXtFMh0Yk\n");
    check(entries.size() == 3, "every line is kept, including the blank one");
    check(entries.at(0).comment == "heading" && !entries.at(0).isLink(),
          "a comment line is a comment");
    check(entries.at(1).blank, "a blank line is blank");
    check(entries.at(2).isLink(), "and the link is a link");
  }

  // --- degenerate input ----------------------------------------------------------------------
  {
    check(linksOnly(parseLinkList("")).empty(), "an empty file has no links");
    check(linksOnly(parseLinkList("\n\n\n")).empty(), "nor has one of blank lines");
    check(linksOnly(parseLinkList("#\n#\n")).empty(), "nor one of bare comment markers");
    checkEqual(renderLinkList({}), std::string(), "and rendering nothing gives nothing");
  }

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_failures << " failures)"
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
