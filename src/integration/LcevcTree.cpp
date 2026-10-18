#include "LcevcTree.h"

#include <parser/common/TreeItem.h>

#include <string>

namespace bda::integration
{

namespace
{

void addSyntax(const lcevc::SyntaxNode &node, const std::shared_ptr<TreeItem> &parent)
{
  const bool isError = node.name == "error";
  auto       item    = node.value.empty()
                           ? parent->createChildItem(node.name)
                           : parent->createChildItem(node.name, node.value,
                                                     node.bits > 0 ? "u(" + std::to_string(node.bits) + ")" : "",
                                                     "", node.meaning, isError);
  for (const auto &child : node.children)
    addSyntax(child, item);
}

} // namespace

bool addLcevcNalUnit(const ByteVector              &data,
                     std::size_t                    startCodeOffset,
                     const std::shared_ptr<TreeItem> &nalRoot,
                     lcevc::SyntaxParser           &parser,
                     int                            nalId)
{
  if (data.size() < startCodeOffset + 2 ||
      !lcevc::isLcevcNalHeader(data.data() + startCodeOffset, data.size() - startCodeOffset))
    return false;

  lcevc::LcevcNal nal;
  nal.nalUnitType = (data[startCodeOffset] & 0x3e) >> 1;
  nal.payload     = lcevc::unescapeRbsp(data.data() + startCodeOffset + 2, data.size() - startCodeOffset - 2);
  // A NAL unit handed over without its trailing zero bytes still ends in the 0x80 stop byte.
  while (!nal.payload.empty() && nal.payload.back() == 0)
    nal.payload.pop_back();
  if (!nal.payload.empty())
    nal.payload.pop_back();
  const auto tree = parser.parse(nal);
  if (nalRoot)
  {
    nalRoot->setProperties("NAL " + std::to_string(nalId) + ": LCEVC " + (nal.isIdr() ? "IDR" : "non-IDR"));
    for (const auto &child : tree.children)
      addSyntax(child, nalRoot);
  }
  return true;
}

bool addLcevcAv1Metadata(const std::uint8_t            *payload,
                         std::size_t                    size,
                         const std::shared_ptr<TreeItem> &obuRoot,
                         lcevc::SyntaxParser           &parser)
{
  // An OBU_METADATA alone is a temporal unit as far as the scanner is concerned: give it a header.
  std::vector<std::uint8_t> obu = {0x2a}; // obu_type 5, has_size_field
  for (std::size_t v = size;;)
  {
    obu.push_back(std::uint8_t((v & 0x7f) | (v > 0x7f ? 0x80 : 0)));
    if ((v >>= 7) == 0)
      break;
  }
  obu.insert(obu.end(), payload, payload + size);
  const auto nals = lcevc::findLcevcInAv1TemporalUnit(obu.data(), obu.size());
  if (nals.empty())
    return false;
  for (const auto &nal : nals)
  {
    const auto tree = parser.parse(nal);
    if (obuRoot)
    {
      auto group = obuRoot->createChildItem(std::string("LCEVC (ITU-T T.35): ") + (nal.isIdr() ? "IDR" : "non-IDR"));
      for (const auto &child : tree.children)
        addSyntax(child, group);
    }
  }
  return true;
}

} // namespace bda::integration
