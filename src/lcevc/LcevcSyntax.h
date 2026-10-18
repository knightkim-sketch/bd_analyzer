// LCEVC enhancement data syntax (MPEG-5 Part 2) as a tree, for the bitstream analysis view. Qt-free.
//
// The ground truth is the MPEG LCEVC Test Model's deserializer (LTM decoder/src/Deserializer.cpp,
// pin 452a9869), not the DIS text: field order, widths and conditions are copied from it, including
// what the DIS lacks (chroma_step_width_multiplier, adaptive-cubic upsampling coefficients, the
// temporal layer). Every field read is also recorded in LTM's BITSTREAM_DEBUG trace form
// ("context.label", bits, value) so a parse can be compared with the reference line for line.
//
// The residual surfaces themselves are not entropy decoded here - that is the decoder's job - but
// their flags and byte sizes are, which is what the tree shows per plane, LoQ and layer. Tiled data
// whose per-tile flags or sizes are themselves entropy coded stops at that point with a note.
//
// The parser is stateful: a picture's encoded data depends on the last global_config (planes,
// transform, temporal, tiles), which only IDR pictures need to carry.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "LcevcBitstream.h"

namespace bda::lcevc
{

struct SyntaxNode
{
  std::string             name;
  std::string             value;   //!< Empty for a group
  std::string             meaning; //!< Decoded meaning of a coded value, when there is one
  int                     bits{-1};
  std::vector<SyntaxNode> children;
};

struct TraceLine
{
  std::string   label; //!< LTM style: context labels joined with '.', then the field label
  unsigned      bits{};
  std::uint64_t value{};
};

//!< The global_config values later pictures depend on, as the parser holds them.
struct GlobalState
{
  bool     valid{};
  unsigned numProcessedPlanes{1};
  unsigned numImagePlanes{3};
  unsigned chromaScaleW{2}, chromaScaleH{2};
  unsigned transformBlockSize{2};
  unsigned numResidualLayers{4};
  bool     temporalEnabled{};
  bool     temporalTileIntraSignalling{};
  unsigned scalingModeLevel1{}, scalingModeLevel2{}; //!< 0 none, 1 1D, 2 2D
  unsigned tileWidth{}, tileHeight{};
  unsigned tileDimensionsType{};
  bool     entropyEnabledPerTile{};
  unsigned sizePerTile{}; //!< 0 none, 1 prefix, 2 prefix on diff
  unsigned resolutionWidth{}, resolutionHeight{};
  unsigned baseDepth{8}, enhancementDepth{8};
  unsigned upsampleType{};
  bool     conformanceWindow{};
  unsigned confWin[4]{}; //!< left, right, top, bottom, in chroma sample units (sequence_config)

  //!< The decoded picture size: the resolution less the conformance window (as LTM crops it).
  unsigned outputWidth() const { return this->resolutionWidth - (this->confWin[0] + this->confWin[1]) * this->chromaScaleW; }
  unsigned outputHeight() const { return this->resolutionHeight - (this->confWin[2] + this->confWin[3]) * this->chromaScaleH; }
};

//!< What one LCEVC NAL unit says about its picture, for the frame tab and overlays.
struct PictureSummary
{
  int      nalUnitType{};
  bool     enhancementEnabled{};
  bool     temporalRefresh{};
  bool     temporalSignallingPresent{};
  unsigned stepWidthLevel1{}, stepWidthLevel2{};
  unsigned quantMatrixMode{};
  bool     ditheringControl{};
  unsigned ditheringType{}, ditheringStrength{};
  unsigned surfaceBytes[3][2]{}; //!< Per plane and LoQ (0 = level 1, 1 = level 2), all layers
  unsigned enhancedWidth{}, enhancedHeight{};
  unsigned baseWidth{}, baseHeight{};
};

class SyntaxParser
{
public:
  //!< Parse one NAL unit's payload into a tree named after it. Errors become nodes, never throws.
  SyntaxNode parse(const LcevcNal &nal);

  const std::vector<TraceLine> &trace() const { return this->traceLines; }
  void                          clearTrace() { this->traceLines.clear(); }
  const GlobalState            &global() const { return this->state; }
  const PictureSummary         &lastPicture() const { return this->picture; }

private:
  GlobalState            state;
  PictureSummary         picture;
  std::vector<TraceLine> traceLines;
};

const char *nalUnitTypeName(int type);

//!< What a decoder needs to know about an LCEVC stream before it decodes a picture.
struct StreamSummary
{
  GlobalState       global;      //!< From the first global_config (valid false when there is none)
  std::vector<bool> idr;         //!< Per picture: nal_unit_type 29
  std::vector<bool> present;     //!< Per picture: carries an LCEVC NAL unit at all
  std::vector<PictureSummary> pictures; //!< Per picture (default-constructed where none)
  std::optional<int> bitstreamVersion;  //!< V-Nova SDK streams signal one (signalledBitstreamVersion)
};

/* Parse the LCEVC NAL units of a stream, one per picture in output order, each as Annex-B bytes
 * (empty for a picture without one).
 */
StreamSummary summarizeStream(const std::vector<std::vector<std::uint8_t>> &perPicture);

} // namespace bda::lcevc
