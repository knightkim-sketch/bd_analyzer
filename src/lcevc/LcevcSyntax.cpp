#include "LcevcSyntax.h"

#include <algorithm>
#include <stdexcept>

namespace bda::lcevc
{

namespace
{

struct OutOfData : std::runtime_error
{
  OutOfData() : std::runtime_error("ran out of data") {}
};
struct Unsupported : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

// LTM resolution_table (Deserializer.cpp).
const unsigned kResolution[51][2] = {
    {0, 0},       {360, 200},   {400, 240},   {480, 320},   {640, 360},   {640, 480},   {768, 480},   {800, 600},   {852, 480},
    {854, 480},   {856, 480},   {960, 540},   {960, 640},   {1024, 576},  {1024, 600},  {1024, 768},  {1152, 864},  {1280, 720},
    {1280, 800},  {1280, 1024}, {1360, 768},  {1366, 768},  {1440, 1050}, {1440, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080},
    {1920, 1200}, {2048, 1080}, {2048, 1152}, {2048, 1536}, {2160, 1440}, {2560, 1440}, {2560, 1600}, {2560, 2048}, {3200, 1800},
    {3200, 2048}, {3200, 2400}, {3440, 1440}, {3840, 1600}, {3840, 2160}, {3840, 3072}, {4096, 2160}, {4096, 3072}, {5120, 2880},
    {5120, 3200}, {5120, 4096}, {6400, 4096}, {6400, 4800}, {7680, 4320}, {7680, 4800}};

/* A bit reader with LTM's labelled reads: each read appends a node to the current group and a line
 * to the trace, labelled with the context stack joined by '.' (BitstreamUnpacker::u with a label).
 */
class Reader
{
public:
  Reader(const std::uint8_t *data, std::size_t size, std::vector<TraceLine> &trace)
      : d(data), n(size), traceLines(trace)
  {
  }

  bool        empty() const { return this->bit >= this->n * 8; }
  std::size_t bitOffset() const { return this->bit; }

  std::uint64_t u(unsigned bits, const char *label, SyntaxNode &node, const char *meaning = nullptr)
  {
    if (this->bit + bits > this->n * 8)
      throw OutOfData();
    std::uint64_t v = 0;
    for (unsigned i = 0; i < bits; ++i, ++this->bit)
      v = (v << 1) | ((this->d[this->bit >> 3] >> (7 - (this->bit & 7))) & 1);
    std::string full;
    for (const auto &c : this->context)
      full += c + ".";
    this->traceLines.push_back({full + label, bits, v});
    node.children.push_back({label, std::to_string(v), meaning ? meaning : "", int(bits), {}});
    return v;
  }

  //!< LTM read_multibyte: 7 bits a byte, most significant group first, top bit = more follows.
  std::uint64_t multibyte(const char *label, SyntaxNode &node)
  {
    this->context.push_back(label);
    SyntaxNode    scratch;
    std::uint64_t result = 0;
    bool          more   = false;
    unsigned      bits   = 0;
    do
    {
      more = this->u(1, "mb-more", scratch) != 0;
      result = (result << 7) | this->u(7, "mb-bits", scratch);
      bits += 8;
    } while (more);
    this->context.pop_back();
    node.children.push_back({label, std::to_string(result), "", int(bits), {}});
    return result;
  }

  //!< Whole bytes; the read must be byte aligned (LTM BitstreamUnpacker::bytes).
  std::vector<std::uint8_t> bytes(std::size_t count)
  {
    if (this->bit % 8 != 0)
      throw Unsupported("byte read at an unaligned position");
    const std::size_t at = this->bit / 8;
    if (at + count > this->n)
      throw OutOfData();
    this->bit += count * 8;
    return {this->d + at, this->d + at + count};
  }

  void align(SyntaxNode &node)
  {
    while (this->bit % 8 != 0)
      this->u(1, "alignment", node);
  }

  struct Context
  {
    Context(Reader &r, std::string label) : reader(r) { r.context.push_back(std::move(label)); }
    ~Context() { this->reader.context.pop_back(); }
    Reader &reader;
  };

private:
  const std::uint8_t      *d;
  std::size_t              n;
  std::size_t              bit{};
  std::vector<std::string> context;
  std::vector<TraceLine>  &traceLines;
};

SyntaxNode group(std::string name) { return {std::move(name), "", "", -1, {}}; }

const char *scalingName(std::uint64_t v) { return v == 0 ? "none" : v == 1 ? "1D" : v == 2 ? "2D" : "?"; }

const char *upsampleName(std::uint64_t v)
{
  static const char *n[] = {"nearest", "linear", "cubic", "modified cubic", "adaptive cubic"};
  return v < 5 ? n[v] : "?";
}

// LTM Dimensions::set - plane, layer and tile sizes per LoQ.
struct Dims
{
  unsigned layerW[3][2]{}, layerH[3][2]{}, tileW[3][2]{}, tileH[3][2]{};
  unsigned baseW{}, baseH{};
};

void setLoq(const GlobalState &g, Dims &d, unsigned loq, unsigned w, unsigned h)
{
  const auto plane = [&](unsigned p, unsigned pw, unsigned ph, unsigned sw, unsigned sh) {
    const unsigned tbs = g.transformBlockSize;
    d.layerW[p][loq]   = (pw + tbs - 1) / tbs;
    d.layerH[p][loq]   = (ph + tbs - 1) / tbs;
    d.tileW[p][loq]    = g.tileWidth ? g.tileWidth / (tbs * sw) : 0;
    d.tileH[p][loq]    = g.tileHeight ? g.tileHeight / (tbs * sh) : 0;
  };
  plane(0, w, h, 1, 1);
  if (g.numImagePlanes == 3)
  {
    const unsigned cw = (w + g.chromaScaleW - 1) / g.chromaScaleW, ch = (h + g.chromaScaleH - 1) / g.chromaScaleH;
    plane(1, cw, ch, g.chromaScaleW, g.chromaScaleH);
    plane(2, cw, ch, g.chromaScaleW, g.chromaScaleH);
  }
}

void setDims(const GlobalState &g, Dims &d, unsigned w, unsigned h)
{
  setLoq(g, d, 1, w, h);
  if (g.scalingModeLevel2 == 0)
    setLoq(g, d, 0, w, h);
  else if (g.scalingModeLevel2 == 1)
    setLoq(g, d, 0, (w + 1) / 2, h);
  else
    setLoq(g, d, 0, (w + 1) / 2, (h + 1) / 2);
}

Dims dimensions(const GlobalState &g)
{
  Dims d;
  setDims(g, d, g.resolutionWidth, g.resolutionHeight);
  const unsigned last = std::max(g.numProcessedPlanes, 1u) - 1;
  unsigned       w = d.layerW[last][0] * g.transformBlockSize, h = d.layerH[last][0] * g.transformBlockSize;
  while (w < g.resolutionWidth)
    w *= 2;
  while (h < g.resolutionHeight)
    h *= 2;
  setDims(g, d, w, h);
  for (unsigned mode : {g.scalingModeLevel2, g.scalingModeLevel1})
  {
    if (mode >= 1)
      w /= 2;
    if (mode == 2)
      h /= 2;
  }
  d.baseW = w, d.baseH = h;
  return d;
}

} // namespace

const char *nalUnitTypeName(int type)
{
  switch (type)
  {
  case kNalNonIdr:
    return "LCEVC_NON_IDR";
  case kNalIdr:
    return "LCEVC_IDR";
  default:
    return "LCEVC_RSV";
  }
}

SyntaxNode SyntaxParser::parse(const LcevcNal &nal)
{
  SyntaxNode root = group("LCEVC NAL: " + std::string(nalUnitTypeName(nal.nalUnitType)));
  root.children.push_back({"nal_unit_type", std::to_string(nal.nalUnitType), nalUnitTypeName(nal.nalUnitType), 5, {}});
  root.children.push_back({"payload bytes", std::to_string(nal.payload.size()), "", -1, {}});
  this->picture             = {};
  this->picture.nalUnitType = nal.nalUnitType;

  Reader     top(nal.payload.data(), nal.payload.size(), this->traceLines);
  SyntaxNode headers;
  try
  {
    while (!top.empty())
    {
      SyntaxNode block = group("process_block");
      const auto sizeType = top.u(3, "payload_size_type", block);
      const auto type     = top.u(5, "payload_type", block);
      std::uint64_t size  = 0;
      if (sizeType <= 5)
        size = sizeType;
      else if (sizeType == 7)
        size = top.multibyte("payload_byte_size", block);
      else
        throw Unsupported("payload_size_type " + std::to_string(sizeType));
      if (sizeType <= 5)
        block.children.push_back({"payload_byte_size", std::to_string(size), "", -1, {}});
      const auto payload = top.bytes(std::size_t(size));
      Reader     b(payload.data(), payload.size(), this->traceLines);
      auto      &g     = this->state;
      auto      &p     = this->picture;
      const auto first = block.children.size(); // the block's own fields start here
      try
      {
      switch (type)
      {
      case 0: {
        block.name = "sequence_config";
        Reader::Context c(b, "sequence_config");
        const auto profile = b.u(4, "profile_idc", block, nullptr);
        const auto level   = b.u(4, "level_idc", block);
        b.u(2, "sublevel_idc", block);
        const auto conformance = b.u(1, "conformance_window_flag", block);
        b.u(5, "reserved", block);
        if (profile == 15 || level == 15)
        {
          b.u(3, "extended_profile_idc", block);
          b.u(3, "extended_level_idc", block);
          b.u(1, "reserved", block);
        }
        g.conformanceWindow = conformance;
        unsigned i          = 0;
        for (const char *o : {"conf_win_left_offset", "conf_win_right_offset", "conf_win_top_offset",
                              "conf_win_bottom_offset"})
          g.confWin[i++] = conformance ? unsigned(b.multibyte(o, block)) : 0;
        block.children[first].meaning = profile == 0 ? "Main" : profile == 1 ? "Main 4:4:4" : "";
        break;
      }
      case 1: {
        block.name = "global_config";
        Reader::Context c(b, "global_config");
        const auto planesType = b.u(1, "processed_planes_type", block);
        const auto resType    = b.u(6, "resolution_type", block);
        const auto transform  = b.u(1, "transform_type", block, nullptr);
        block.children.back().meaning = transform ? "DDS (4x4, 16 layers)" : "DD (2x2, 4 layers)";
        const auto chroma = b.u(2, "chroma_sampling_type", block);
        static const char *chromaNames[] = {"monochrome", "4:2:0", "4:2:2", "4:4:4"};
        block.children.back().meaning    = chromaNames[chroma];
        const auto baseDepth = b.u(2, "base_depth_type", block);
        const auto enhDepth  = b.u(2, "enhancement_depth_type", block);
        const auto tswmSignalled = b.u(1, "temporal_step_width_modifier_signalled", block);
        b.u(1, "predicted_residual_mode", block);
        g.temporalTileIntraSignalling = b.u(1, "temporal_tile_intra_signalling_enabled", block);
        g.temporalEnabled             = b.u(1, "temporal_enabled", block);
        g.upsampleType                = unsigned(b.u(3, "upsample_type", block));
        block.children.back().meaning = upsampleName(g.upsampleType);
        const auto l1Filtering        = b.u(1, "level_1_filtering_signalled", block);
        g.scalingModeLevel1           = unsigned(b.u(2, "scaling_mode_level1", block));
        block.children.back().meaning = scalingName(g.scalingModeLevel1);
        g.scalingModeLevel2           = unsigned(b.u(2, "scaling_mode_level2", block));
        block.children.back().meaning = scalingName(g.scalingModeLevel2);
        g.tileDimensionsType          = unsigned(b.u(2, "tile_dimensions_type", block));
        b.u(2, "user_data_enabled", block);
        b.u(1, "level1_depth_flag", block);
        const auto chromaStepFlag = b.u(1, "chroma_step_width_flag", block);
        g.numProcessedPlanes      = 1;
        if (planesType)
        {
          const auto planes = b.u(4, "planes_type", block);
          b.u(4, "reserved", block);
          if (planes != 1)
            throw Unsupported("planes_type " + std::to_string(planes));
          g.numProcessedPlanes = 3;
        }
        if (tswmSignalled)
          b.u(8, "temporal_step_width_modifier", block);
        if (g.upsampleType == 4)
          for (const char *k : {"upsampling_coefficient[0]", "upsampling_coefficient[1]", "upsampling_coefficient[2]",
                                "upsampling_coefficient[3]"})
            b.u(16, k, block);
        if (l1Filtering)
        {
          b.u(4, "level_1_filtering_first_coefficient", block);
          b.u(4, "level_1_filtering_second_coefficient", block);
        }
        g.tileWidth = g.tileHeight = 0;
        if (g.tileDimensionsType == 1)
          g.tileWidth = 512, g.tileHeight = 256;
        else if (g.tileDimensionsType == 2)
          g.tileWidth = 1024, g.tileHeight = 512;
        else if (g.tileDimensionsType == 3)
        {
          g.tileWidth  = unsigned(b.u(16, "custom_tile_width", block));
          g.tileHeight = unsigned(b.u(16, "custom_tile_height", block));
        }
        if (g.tileDimensionsType > 0)
        {
          b.u(5, "reserved", block);
          g.entropyEnabledPerTile = b.u(1, "compression_type_entropy_enabled_per_tile", block);
          g.sizePerTile           = unsigned(b.u(2, "compression_type_size_per_tile", block));
        }
        if (resType > 0 && resType < 51)
        {
          g.resolutionWidth  = kResolution[resType][0];
          g.resolutionHeight = kResolution[resType][1];
          block.children[first + 1].meaning = std::to_string(g.resolutionWidth) + "x" + std::to_string(g.resolutionHeight);
        }
        else if (resType == 63)
        {
          g.resolutionWidth  = unsigned(b.u(16, "resolution_width", block));
          g.resolutionHeight = unsigned(b.u(16, "resolution_height", block));
        }
        else
          throw Unsupported("resolution_type " + std::to_string(resType));
        if (chromaStepFlag)
          b.u(8, "chroma_step_width_multiplier", block);
        g.numImagePlanes     = chroma == 0 ? 1 : 3;
        g.chromaScaleW       = chroma == 1 || chroma == 2 ? 2 : 1;
        g.chromaScaleH       = chroma == 1 ? 2 : 1;
        g.transformBlockSize = transform ? 4 : 2;
        g.numResidualLayers  = transform ? 16 : 4;
        g.baseDepth          = 8 + 2 * unsigned(baseDepth);
        g.enhancementDepth   = 8 + 2 * unsigned(enhDepth);
        g.valid              = true;
        break;
      }
      case 2: {
        block.name = "picture_config";
        Reader::Context c(b, "picture_config");
        p.enhancementEnabled = !b.u(1, "no_enhancement_bit", block);
        if (p.enhancementEnabled)
        {
          p.quantMatrixMode = unsigned(b.u(3, "quant_matrix_mode", block));
          const auto dequantOffsetSignalled = b.u(1, "dequant_offset_signalled", block);
          const auto pictureType            = b.u(1, "picture_type", block);
          p.temporalRefresh                 = b.u(1, "temporal_refresh", block);
          p.temporalSignallingPresent       = g.temporalEnabled && !p.temporalRefresh;
          const auto l1Enabled              = b.u(1, "step_width_level1_enabled", block);
          p.stepWidthLevel2                 = unsigned(b.u(15, "step_width_level2", block));
          p.ditheringControl                = b.u(1, "dithering_control", block);
          if (pictureType)
          {
            b.u(1, "field_type", block);
            b.u(7, "reserved", block);
          }
          p.stepWidthLevel1 = 32767;
          if (l1Enabled)
          {
            p.stepWidthLevel1 = unsigned(b.u(15, "step_width_level1", block));
            b.u(1, "level_1_filtering_enabled", block);
          }
          const unsigned qm = p.quantMatrixMode;
          if (qm == 2 || qm == 3 || qm == 5)
            for (unsigned l = 0; l < g.numResidualLayers; ++l)
              b.u(8, "qm_coefficient_0", block);
          if (qm == 4 || qm == 5)
            for (unsigned l = 0; l < g.numResidualLayers; ++l)
              b.u(8, "qm_coefficient_1", block);
          if (dequantOffsetSignalled)
          {
            b.u(1, "dequant_offset_mode", block);
            b.u(7, "dequant_offset", block);
          }
          if (p.ditheringControl)
          {
            p.ditheringType = unsigned(b.u(2, "dithering_type", block));
            b.u(1, "reserved", block);
            const auto s = b.u(5, p.ditheringType ? "dithering_strength" : "reserved", block);
            if (p.ditheringType)
              p.ditheringStrength = unsigned(s);
          }
        }
        else
        {
          b.u(4, "reserved", block);
          b.u(1, "picture_type", block);
          p.temporalRefresh           = b.u(1, "temporal_refresh", block);
          p.temporalSignallingPresent = b.u(1, "temporal_signalling_present", block);
        }
        break;
      }
      case 3:
      case 4: {
        if (!g.valid)
          throw Unsupported("encoded data before any global_config");
        const bool tiled = type == 4;
        block.name       = tiled ? "encoded_data_tiled" : "encoded_data";
        const unsigned firstLayer = p.enhancementEnabled ? 0 : g.numResidualLayers;
        const auto     totalLayers = [&](unsigned loq) {
          return g.numResidualLayers + (loq == 1 && p.temporalSignallingPresent ? 1 : 0);
        };
        if (!tiled)
        {
          // The flags come first for every surface; they are shown under each surface below.
          bool       entropy[3][2][17]{}, rle[3][2][17]{};
          SyntaxNode flags;
          for (unsigned pl = 0; pl < g.numProcessedPlanes; ++pl)
            for (unsigned loq = 0; loq < 2; ++loq)
              for (unsigned layer = firstLayer; layer < totalLayers(loq); ++layer)
              {
                Reader::Context c(b, "encoded_data");
                entropy[pl][loq][layer] = b.u(1, "entropy_enabled", flags);
                rle[pl][loq][layer]     = b.u(1, "rle_only", flags);
              }
          b.align(flags);
          if (p.enhancementEnabled || p.temporalSignallingPresent)
            for (unsigned pl = 0; pl < g.numProcessedPlanes; ++pl)
              for (unsigned loq = 0; loq < 2; ++loq)
                for (unsigned layer = firstLayer; layer < totalLayers(loq); ++layer)
                {
                  const std::string name = "surface[" + std::to_string(pl) + "][" + std::to_string(loq) + "][" +
                                           std::to_string(layer) + "]";
                  SyntaxNode surface = group(name + (layer == g.numResidualLayers ? " temporal" : ""));
                  surface.children.push_back({"entropy_enabled", std::to_string(entropy[pl][loq][layer]), "", 1, {}});
                  surface.children.push_back({"rle_only", std::to_string(rle[pl][loq][layer]), "", 1, {}});
                  if (entropy[pl][loq][layer])
                  {
                    Reader::Context c(b, name);
                    const auto      size = b.multibyte("data_size", surface);
                    b.bytes(std::size_t(size));
                    p.surfaceBytes[pl][loq] += unsigned(size);
                  }
                  block.children.push_back(std::move(surface));
                }
        }
        else
        {
          const Dims d = dimensions(g);
          for (unsigned pl = 0; pl < g.numProcessedPlanes; ++pl)
            for (unsigned loq = 0; loq < 2; ++loq)
              if (p.enhancementEnabled)
                for (unsigned layer = firstLayer; layer < totalLayers(loq); ++layer)
                  b.u(1, "rle_only", block);
          b.align(block);
          if (g.entropyEnabledPerTile)
            throw Unsupported("per-tile entropy_enabled flags are entropy coded - not parsed further");
          std::vector<bool> entropy;
          unsigned          tiles[3][2]{};
          for (unsigned pl = 0; pl < g.numProcessedPlanes; ++pl)
            for (unsigned loq = 0; loq < 2; ++loq)
            {
              const unsigned tw = std::max(1u, d.tileW[pl][loq]), th = std::max(1u, d.tileH[pl][loq]);
              tiles[pl][loq]    = ((d.layerW[pl][loq] + tw - 1) / tw) * ((d.layerH[pl][loq] + th - 1) / th);
            }
          for (unsigned pl = 0; pl < g.numProcessedPlanes; ++pl)
            if (p.enhancementEnabled)
              for (unsigned loq = 0; loq < 2; ++loq)
                for (unsigned layer = firstLayer; layer < totalLayers(loq); ++layer)
                  for (unsigned t = 0; t < tiles[pl][loq]; ++t)
                    entropy.push_back(b.u(1, "entropy_enabled", block) != 0);
          b.align(block);
          if (g.sizePerTile != 0)
            throw Unsupported("per-tile sizes are entropy coded - not parsed further");
          std::size_t idx = 0;
          for (unsigned pl = 0; pl < g.numProcessedPlanes; ++pl)
            for (unsigned loq = 0; loq < 2; ++loq)
              for (unsigned layer = firstLayer; layer < totalLayers(loq); ++layer)
              {
                SyntaxNode surface = group("surface[" + std::to_string(pl) + "][" + std::to_string(loq) + "][" +
                                           std::to_string(layer) + "] " + std::to_string(tiles[pl][loq]) + " tiles");
                for (unsigned t = 0; t < tiles[pl][loq]; ++t, ++idx)
                  if (idx < entropy.size() && entropy[idx])
                  {
                    const auto size = b.multibyte("data_size", surface);
                    b.bytes(std::size_t(size));
                    p.surfaceBytes[pl][loq] += unsigned(size);
                  }
                block.children.push_back(std::move(surface));
              }
        }
        break;
      }
      case 5: {
        block.name = "additional_info";
        const auto t = b.u(8, "additional_info_type", block);
        block.children.back().meaning = t == 0 ? "SEI payload" : t == 1 ? "VUI parameters" : "";
        if (t == 0)
          b.u(8, "payload_type", block);
        break;
      }
      case 6: {
        block.name = "filler";
        Reader::Context c(b, "filler");
        while (!b.empty())
          b.u(8, "filler", block);
        break;
      }
      default:
        block.name = "process_block (type " + std::to_string(type) + ")";
        break;
      }
      }
      catch (const std::exception &e)
      {
        // Keep what was read of the block, say why it stops, and go on with the next one: the
        // block's size is known, so the units after it are still where they should be.
        block.children.push_back({"error", e.what(), "", -1, {}});
      }
      root.children.push_back(std::move(block));
    }
  }
  catch (const std::exception &e)
  {
    root.children.push_back({"error", e.what(), "", -1, {}});
  }

  if (this->state.valid)
  {
    const Dims d               = dimensions(this->state);
    this->picture.enhancedWidth  = this->state.resolutionWidth;
    this->picture.enhancedHeight = this->state.resolutionHeight;
    this->picture.baseWidth      = d.baseW;
    this->picture.baseHeight     = d.baseH;
  }
  return root;
}

StreamSummary summarizeStream(const std::vector<std::vector<std::uint8_t>> &perPicture)
{
  StreamSummary summary;
  SyntaxParser  parser;
  for (const auto &bytes : perPicture)
  {
    const auto nal = lcevcNalFromUnit(bytes.data(), bytes.size());
    summary.present.push_back(nal.has_value());
    summary.idr.push_back(nal && nal->isIdr());
    if (!nal)
    {
      summary.pictures.emplace_back();
      continue;
    }
    parser.parse(*nal);
    parser.clearTrace();
    summary.pictures.push_back(parser.lastPicture());
    if (!summary.global.valid && parser.global().valid)
      summary.global = parser.global();
  }
  return summary;
}

} // namespace bda::lcevc
