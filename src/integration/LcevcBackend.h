// The two LCEVC enhancement decoders bd-analyzer can run over a decoded base picture, behind one
// interface. Both libraries are optional run-time dependencies, loaded with QLibrary from the same
// places as the base decoders (Settings search path, ./decoder next to the binary, the system):
//   * LTM - libbda-lcevc-ltm.so (scripts/setup-lcevc-ltm.sh), the MPEG reference: the analysis
//     backend, with the intermediate surfaces and the signalled configuration of every picture;
//   * LCEVCdec - liblcevc_dec_api.so (scripts/setup-lcevcdec.sh), V-Nova's decoder: playback only,
//     the enhanced picture and nothing else.
// Dithering is off in both, so they are deterministic and give the same picture (verified against
// LTM's ModelDecoder on the test streams).
#pragma once

#include <QByteArray>
#include <QLibrary>
#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "../../third_party/lcevc-ltm-wrapper/bda_lcevc_ltm.h"

namespace bda::integration
{

//!< A planar picture in memory: 8-bit samples, or 16-bit little endian above 8 bits.
struct LcevcPlanes
{
  int                  width{}, height{}, bitDepth{8};
  int                  chromaFormat{1}; //!< 0 4:0:0, 1 4:2:0, 2 4:2:2, 3 4:4:4
  const std::uint8_t  *data[3]{};
  std::ptrdiff_t       stride[3]{}; //!< Bytes
};

//!< Width/height of plane p of a picture of the given chroma format (as YUView lays planes out).
int lcevcPlaneWidth(int width, int chromaFormat, int p);
int lcevcPlaneHeight(int height, int chromaFormat, int p);

//!< An LTM intermediate surface, copied out of the library.
struct LcevcSurface
{
  int                        width{}, height{};
  int                        bytesPerSample{}; //!< 2: int16 S15 fixed point, 1: uint8 (temporal mask)
  std::vector<std::uint8_t>  data;             //!< Rows packed, width * bytesPerSample each
  bool                       valid() const { return !this->data.empty(); }
  std::int16_t               s16(int x, int y) const
  {
    std::int16_t v;
    std::memcpy(&v, this->data.data() + (std::size_t(y) * this->width + x) * 2, 2);
    return v;
  }
  std::uint8_t u8(int x, int y) const { return this->data[std::size_t(y) * this->width + x]; }
};

struct LcevcDecodedPicture
{
  int        width{}, height{}, bitDepth{8}, chromaFormat{1};
  QByteArray yuv; //!< Enhanced picture, planes packed one after the other (YUView's raw layout)
  //!< LTM only: [surface id][plane]
  std::array<std::array<LcevcSurface, 3>, BDA_LTM_SURF_COUNT> surfaces;
  std::optional<bda_ltm_picture_info>                         info;
};

class LcevcBackend
{
public:
  enum class Kind
  {
    Ltm,
    LcevcDec
  };

  static std::unique_ptr<LcevcBackend> create(Kind kind);
  virtual ~LcevcBackend() = default;

  Kind    kind() const { return this->backendKind; }
  bool    loaded() const { return this->error.isEmpty(); }
  QString errorString() const { return this->error; }
  QString libraryPath() const { return this->library.fileName(); }
  virtual QString name() const = 0;

  //!< The bit depth of the enhanced picture when it is not the base's (enhancement_depth_type).
  virtual void setOutputDepth(int) {}

  //!< Drop all decoder state; the next picture must start a stream (an IDR / global_config).
  virtual bool reset() = 0;

  /* Enhance one base picture with its LCEVC NAL unit (Annex-B bytes, start code included).
   * Pictures go in output order. `dumpMask` asks LTM for the intermediate surfaces (bit = surface
   * id); LCEVCdec ignores it. On failure errorString() says why and the backend needs reset().
   */
  virtual bool decode(const std::vector<std::uint8_t> &annexB,
                      const LcevcPlanes              &base,
                      unsigned                        dumpMask,
                      LcevcDecodedPicture            &out) = 0;

protected:
  explicit LcevcBackend(Kind kind) : backendKind(kind) {}
  //!< Load the first of `names` found in the decoder search path. Sets error on failure.
  bool loadLibrary(const QStringList &names);

  Kind     backendKind;
  QLibrary library;
  QString  error;
};

} // namespace bda::integration
