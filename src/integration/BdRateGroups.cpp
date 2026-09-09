#include "BdRateGroups.h"

#include <QFileInfo>
#include <QList>

#include <algorithm>

#include "playlistitem/playlistItemCompressedVideo.h"
#include "playlistitem/playlistItemRawFile.h"

namespace bda::integration
{
namespace
{

/* The raw item a selection offers as the original.
 *
 * Dropping the source in with the streams is how this is meant to be used, so a .yuv or .y4m in the
 * selection is read as "compare against this" rather than as another curve point. More than one is
 * ambiguous and refused rather than guessed at.
 */
QStringList rawItemPaths(const QList<playlistItem *> &selection)
{
  QStringList paths;
  for (auto *item : selection)
    if (auto *raw = dynamic_cast<playlistItemRawFile *>(item))
      if (const auto path = raw->properties().name; !path.isEmpty())
        paths.append(path);
  return paths;
}

QString fileNameOf(const playlistItem *item)
{
  return item ? QFileInfo(item->properties().name).fileName() : QString();
}

} // namespace

QString bdRateGroupName(const QStringList &fileNames, int groupIndex)
{
  const auto fallback = QString("Group %1").arg(groupIndex + 1);
  if (fileNames.isEmpty())
    return fallback;
  if (fileNames.size() == 1)
    return QFileInfo(fileNames.first()).completeBaseName();

  auto prefix = fileNames.first();
  for (const auto &name : fileNames)
  {
    int keep = 0;
    while (keep < prefix.size() && keep < name.size() && prefix.at(keep) == name.at(keep))
      ++keep;
    prefix.truncate(keep);
    if (prefix.isEmpty())
      break;
  }

  /* Trim the separator the QP suffix hangs off, so "fast_q30 / fast_q35" gives "fast" and not
   * "fast_q". Anything shorter than two characters is noise rather than a name.
   */
  while (!prefix.isEmpty() && (prefix.back() == '_' || prefix.back() == '-' || prefix.back() == '.' ||
                               prefix.back() == 'q' || prefix.back() == 'Q'))
    prefix.chop(1);

  return prefix.size() >= 2 ? prefix : fallback;
}

BdRateGroupResult makeBdRateGroup(const QList<playlistItem *>    &selection,
                                  const std::vector<BdRateGroup> &existing)
{
  BdRateGroupResult result;
  const auto        fail = [&result](BdRateGroupError error, const QString &message) {
    result.error   = error;
    result.message = message;
    return result;
  };

  // --- the streams -----------------------------------------------------------------------------
  QStringList names;
  for (auto *item : selection)
  {
    auto *stream = dynamic_cast<playlistItemCompressedVideo *>(item);
    if (!stream)
      continue; // raw items are the original, containers and text items are not points

    if (!stream->providesSuperblockBits())
      return fail(BdRateGroupError::NoBitCounts,
                  QString("%1 has no per superblock bit counts. BD-rate needs the dav1d analyzer "
                          "decoder; this stream is decoded by something else.")
                      .arg(fileNameOf(stream)));

    const auto size = stream->getRawFrameSize();
    const auto grid = stream->getDefaultGridSize();
    if (size.width == 0 || size.height == 0 || grid == 0)
      return fail(BdRateGroupError::MixedGeometry,
                  QString("%1 has not reported its frame size or superblock size yet. Open it "
                          "once so the sequence header is parsed.")
                      .arg(fileNameOf(stream)));

    if (result.group.points.empty())
    {
      result.group.frameSize      = size;
      result.group.superblockSize = grid;
    }
    else if (size.width != result.group.frameSize.width ||
             size.height != result.group.frameSize.height ||
             grid != result.group.superblockSize)
    {
      return fail(BdRateGroupError::MixedGeometry,
                  QString("%1 is %2x%3 with a %4 superblock, but the others in this selection are "
                          "%5x%6 with %7. One curve has to describe one grid.")
                      .arg(fileNameOf(stream))
                      .arg(size.width)
                      .arg(size.height)
                      .arg(grid)
                      .arg(result.group.frameSize.width)
                      .arg(result.group.frameSize.height)
                      .arg(result.group.superblockSize));
    }

    result.group.points.push_back({stream, fileNameOf(stream)});
    names.append(fileNameOf(stream));
  }

  if (result.group.points.empty())
    return fail(BdRateGroupError::NoStreams,
                "Select the compressed streams that make up one curve, and the original YUV or "
                "Y4M they were coded from.");
  if (result.group.points.size() < 2)
    return fail(BdRateGroupError::OnePointOnly,
                QString("%1 is a single operating point. A curve needs at least two, and the "
                        "standard BD-rate uses four.")
                    .arg(names.first()));

  // --- must agree with the groups already added ------------------------------------------------
  if (!existing.empty())
  {
    const auto &first = existing.front();
    if (first.frameSize.width != result.group.frameSize.width ||
        first.frameSize.height != result.group.frameSize.height ||
        first.superblockSize != result.group.superblockSize)
      return fail(BdRateGroupError::GeometryMismatch,
                  QString("This selection is %1x%2 with a %3 superblock; \"%4\" is %5x%6 with %7. "
                          "Superblock coordinates would not line up.")
                      .arg(result.group.frameSize.width)
                      .arg(result.group.frameSize.height)
                      .arg(result.group.superblockSize)
                      .arg(first.name)
                      .arg(first.frameSize.width)
                      .arg(first.frameSize.height)
                      .arg(first.superblockSize));
  }

  // --- the original ----------------------------------------------------------------------------
  const auto raws = rawItemPaths(selection);
  if (raws.size() > 1)
    return fail(BdRateGroupError::NoOriginal,
                "More than one raw file is selected. Include exactly one, so it is clear which is "
                "the original.");

  QString originalPath = raws.isEmpty() ? QString() : raws.first();
  if (originalPath.isEmpty())
  {
    /* Nothing offered, so fall back to what the streams already carry - a user who attached the
     * original by hand for the SSE overlay should not have to select it again. All of them have to
     * agree, or the PSNRs would be measured against different sources.
     */
    for (const auto &point : result.group.points)
    {
      const auto attached = point.item->getOriginalYUVSource();
      if (attached.isEmpty())
        return fail(BdRateGroupError::NoOriginal,
                    QString("No original to measure against. Add the source YUV or Y4M to the "
                            "selection, or attach it to %1 with \"Load Org YUV\".")
                        .arg(point.label));
      if (originalPath.isEmpty())
        originalPath = attached;
      else if (originalPath != attached)
        return fail(BdRateGroupError::NoOriginal,
                    "The selected streams have different originals attached. Add the one to "
                    "compare against to the selection.");
    }
  }

  // Attaching is what produces the SSE, so it happens here rather than at collection time.
  for (const auto &point : result.group.points)
  {
    QString error;
    if (!point.item->setOriginalYUVSource(originalPath, &error))
      return fail(BdRateGroupError::OriginalRejected,
                  QString("%1 cannot use that original: %2").arg(point.label, error));
  }
  result.group.originalPath = originalPath;

  /* Unique against the groups already added. The name comes from the file names, so pressing the
   * button twice on the same selection - which is a reasonable thing to do while setting up a
   * comparison - would otherwise give two curves called the same thing, and the anchor radio and
   * the legend would both become ambiguous.
   */
  auto name = bdRateGroupName(names, int(existing.size()));
  const auto taken = [&existing](const QString &candidate) {
    return std::any_of(existing.begin(), existing.end(), [&candidate](const BdRateGroup &group) {
      return group.name == candidate;
    });
  };
  if (taken(name))
  {
    const auto base = name;
    for (int suffix = 2; taken(name); ++suffix)
      name = QString("%1 (%2)").arg(base).arg(suffix);
  }
  result.group.name = name;
  return result;
}

QString bdRateGroupErrorText(const BdRateGroupResult &result)
{
  return result.ok() ? QString() : result.message;
}

} // namespace bda::integration
