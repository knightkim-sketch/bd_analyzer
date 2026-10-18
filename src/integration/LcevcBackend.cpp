#include "LcevcBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <LCEVC/lcevc_dec.h>

#include <chrono>
#include <thread>

namespace bda::integration
{

int lcevcPlaneWidth(int width, int chromaFormat, int p)
{
  return p == 0 || chromaFormat == 3 ? width : chromaFormat == 0 ? 0 : width / 2;
}

int lcevcPlaneHeight(int height, int chromaFormat, int p)
{
  return p == 0 || chromaFormat >= 2 ? height : chromaFormat == 0 ? 0 : height / 2;
}

bool LcevcBackend::loadLibrary(const QStringList &names)
{
  // The base decoders' search order (decoderBaseSingleLib::loadDecoderLibrary).
  QSettings settings;
  settings.beginGroup("Decoders");
  auto searchPath = settings.value("SearchPath", "").toString();
  settings.endGroup();
  if (!searchPath.endsWith("/"))
    searchPath.append("/");
  const QStringList dirs = {searchPath,
                            QDir::currentPath() + "/",
                            QDir::currentPath() + "/decoder/",
                            QCoreApplication::applicationDirPath() + "/",
                            QCoreApplication::applicationDirPath() + "/decoder/",
                            ""};
  for (const auto &name : names)
    for (const auto &dir : dirs)
    {
      this->library.setFileName(dir + name);
      if (this->library.load())
      {
        this->error.clear();
        return true;
      }
    }
  this->error = QString("Could not load %1: %2").arg(names.join(" / "), this->library.errorString());
  return false;
}

namespace
{

// Pack planes (any stride) into YUView's raw layout: plane after plane, rows without padding.
void packPlanes(QByteArray       &dst,
                const void *const data[3],
                const std::ptrdiff_t stride[3],
                int              width,
                int              height,
                int              chromaFormat,
                int              bytesPerSample)
{
  const int nPlanes = chromaFormat == 0 ? 1 : 3;
  qsizetype total   = 0;
  for (int p = 0; p < nPlanes; ++p)
    total += qsizetype(lcevcPlaneWidth(width, chromaFormat, p)) * lcevcPlaneHeight(height, chromaFormat, p) *
             bytesPerSample;
  dst.resize(total);
  auto *out = reinterpret_cast<std::uint8_t *>(dst.data());
  for (int p = 0; p < nPlanes; ++p)
  {
    const int  w   = lcevcPlaneWidth(width, chromaFormat, p) * bytesPerSample;
    const int  h   = lcevcPlaneHeight(height, chromaFormat, p);
    const auto src = static_cast<const std::uint8_t *>(data[p]);
    for (int y = 0; y < h; ++y, out += w)
      std::memcpy(out, src + std::ptrdiff_t(y) * stride[p], std::size_t(w));
  }
}

// ---- LTM: libbda-lcevc-ltm.so ----------------------------------------------------------------------

class LtmBackend : public LcevcBackend
{
public:
  LtmBackend() : LcevcBackend(Kind::Ltm)
  {
    if (!this->loadLibrary({"libbda-lcevc-ltm.so", "bda-lcevc-ltm"}))
      return;
    auto resolve = [this](auto &fn, const char *symbol) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(this->library.resolve(symbol));
      if (!fn && this->error.isEmpty())
        this->error = QString("%1 has no %2").arg(this->library.fileName(), symbol);
    };
    resolve(this->apiVersion, "bda_ltm_api_version");
    resolve(this->versionString, "bda_ltm_version_string");
    resolve(this->createFn, "bda_ltm_create");
    resolve(this->destroyFn, "bda_ltm_destroy");
    resolve(this->decodeFn, "bda_ltm_decode");
    resolve(this->getSurface, "bda_ltm_get_surface");
    resolve(this->getInfo, "bda_ltm_get_picture_info");
    resolve(this->lastError, "bda_ltm_last_error");
    if (!this->error.isEmpty())
      return;
    // The structs below are shared with the library: a different layout would be read silently wrong.
    if (this->apiVersion() != BDA_LTM_API_VERSION)
    {
      this->error = QString("%1 has API version %2, bd-analyzer needs %3")
                        .arg(this->library.fileName())
                        .arg(this->apiVersion())
                        .arg(BDA_LTM_API_VERSION);
      return;
    }
    this->reset();
  }
  ~LtmBackend() override
  {
    if (this->handle)
      this->destroyFn(this->handle);
  }

  QString name() const override
  {
    return this->versionString ? QString::fromUtf8(this->versionString()) : QString("LTM");
  }

  bool reset() override
  {
    if (!this->createFn)
      return false;
    if (this->handle)
      this->destroyFn(this->handle);
    this->handle = this->createFn(); // dithering off by default
    if (!this->handle)
      this->error = "bda_ltm_create failed";
    return this->handle != nullptr;
  }

  bool decode(const std::vector<std::uint8_t> &annexB,
              const LcevcPlanes              &base,
              unsigned                        dumpMask,
              LcevcDecodedPicture            &out) override
  {
    if (!this->handle)
      return false;
    bda_ltm_planes in{};
    in.width         = base.width;
    in.height        = base.height;
    in.bit_depth     = base.bitDepth;
    in.chroma_format = base.chromaFormat;
    for (int p = 0; p < 3; ++p)
    {
      in.data[p]   = base.data[p];
      in.stride[p] = base.stride[p];
    }
    bda_ltm_planes enhanced{};
    const auto     rc = this->decodeFn(
        this->handle, annexB.data(), annexB.size(), BDA_LTM_PAYLOAD_ANNEXB, -1, &in, &enhanced, dumpMask);
    if (rc != BDA_LTM_OK)
    {
      this->error = QString("LTM decode failed (%1): %2").arg(rc).arg(QString::fromUtf8(this->lastError(this->handle)));
      return false;
    }
    out.width        = enhanced.width;
    out.height       = enhanced.height;
    out.bitDepth     = enhanced.bit_depth;
    out.chromaFormat = enhanced.chroma_format;
    packPlanes(out.yuv, enhanced.data, enhanced.stride, enhanced.width, enhanced.height, enhanced.chroma_format,
               enhanced.bit_depth > 8 ? 2 : 1);

    bda_ltm_picture_info info{};
    if (this->getInfo(this->handle, &info) == BDA_LTM_OK)
      out.info = info;
    for (int s = 0; s < BDA_LTM_SURF_COUNT; ++s)
      for (int p = 0; p < 3; ++p)
      {
        auto &dst = out.surfaces[std::size_t(s)][std::size_t(p)];
        dst       = {};
        bda_ltm_surface surface{};
        if (!(dumpMask & (1u << s)) || this->getSurface(this->handle, s, p, &surface) != BDA_LTM_OK)
          continue;
        dst.width          = surface.width;
        dst.height         = surface.height;
        dst.bytesPerSample = surface.bytes_per_sample;
        const auto rowBytes = std::size_t(surface.width) * std::size_t(surface.bytes_per_sample);
        dst.data.resize(rowBytes * std::size_t(surface.height));
        for (int y = 0; y < surface.height; ++y)
          std::memcpy(dst.data.data() + rowBytes * std::size_t(y),
                      static_cast<const std::uint8_t *>(surface.data) + std::ptrdiff_t(y) * surface.stride,
                      rowBytes);
      }
    return true;
  }

private:
  bda_ltm *handle{};

  decltype(&bda_ltm_api_version)      apiVersion{};
  decltype(&bda_ltm_version_string)   versionString{};
  decltype(&bda_ltm_create)           createFn{};
  decltype(&bda_ltm_destroy)          destroyFn{};
  decltype(&bda_ltm_decode)           decodeFn{};
  decltype(&bda_ltm_get_surface)      getSurface{};
  decltype(&bda_ltm_get_picture_info) getInfo{};
  decltype(&bda_ltm_last_error)       lastError{};
};

// ---- LCEVCdec: liblcevc_dec_api.so ---------------------------------------------------------------

LCEVC_ColorFormat colorFormat(int chromaFormat, int bitDepth)
{
  const int depthStep = bitDepth <= 8 ? 1 : bitDepth <= 10 ? 2 : bitDepth <= 12 ? 3 : bitDepth <= 14 ? 4 : 5;
  const int base      = chromaFormat == 0 ? 5000 : chromaFormat == 2 ? 1200 : chromaFormat == 3 ? 1400 : 1000;
  return LCEVC_ColorFormat(base + depthStep);
}

class LcevcDecBackend : public LcevcBackend
{
public:
  LcevcDecBackend() : LcevcBackend(Kind::LcevcDec)
  {
    if (!this->loadLibrary({"liblcevc_dec_api.so.4", "liblcevc_dec_api.so", "lcevc_dec_api"}))
      return;
    auto resolve = [this](auto &fn, const char *symbol) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(this->library.resolve(symbol));
      if (!fn && this->error.isEmpty())
        this->error = QString("%1 has no %2").arg(this->library.fileName(), symbol);
    };
    resolve(this->createDecoder, "LCEVC_CreateDecoder");
    resolve(this->destroyDecoder, "LCEVC_DestroyDecoder");
    resolve(this->configureBool, "LCEVC_ConfigureDecoderBool");
    resolve(this->configureInt, "LCEVC_ConfigureDecoderInt");
    resolve(this->initializeDecoder, "LCEVC_InitializeDecoder");
    resolve(this->defaultPictureDesc, "LCEVC_DefaultPictureDesc");
    resolve(this->allocPicture, "LCEVC_AllocPicture");
    resolve(this->allocPictureExternal, "LCEVC_AllocPictureExternal");
    resolve(this->freePicture, "LCEVC_FreePicture");
    resolve(this->sendPicture, "LCEVC_SendDecoderPicture");
    resolve(this->sendEnhancement, "LCEVC_SendDecoderEnhancementData");
    resolve(this->sendBase, "LCEVC_SendDecoderBase");
    resolve(this->receivePicture, "LCEVC_ReceiveDecoderPicture");
    resolve(this->receiveBase, "LCEVC_ReceiveDecoderBase");
    resolve(this->getPictureDesc, "LCEVC_GetPictureDesc");
    resolve(this->getPlaneCount, "LCEVC_GetPicturePlaneCount");
    resolve(this->lockPicture, "LCEVC_LockPicture");
    resolve(this->getLockPlaneDesc, "LCEVC_GetPictureLockPlaneDesc");
    resolve(this->unlockPicture, "LCEVC_UnlockPicture");
    if (!this->error.isEmpty())
      return;
    this->reset();
  }
  ~LcevcDecBackend() override { this->destroy(); }

  QString name() const override { return QString("LCEVCdec %1.%2.%3").arg(LCEVC_DEC_VERSION_MAJOR).arg(LCEVC_DEC_VERSION_MINOR).arg(LCEVC_DEC_VERSION_PATCH); }

  bool reset() override
  {
    if (!this->createDecoder)
      return false;
    this->destroy();
    LCEVC_DecoderHandle dec{};
    if (this->createDecoder(&dec, LCEVC_AccelContextHandle{0}) != LCEVC_Success)
      return this->fail("LCEVC_CreateDecoder failed");
    this->decoder = dec;
    this->live    = true;
    this->configureInt(dec, "log_level", 1);
    this->configureBool(dec, "allow_dithering", false);
    /* A stream that does not signal its version is read as LTM writes it. LCEVCdec's default (version
     * 2) sizes a Huffman code length field of 0 as 0 bits where LTM uses 1; on an LTM stream whose
     * temporal chunk has such a table, LCEVCdec decodes past the chunk and never returns. A V-Nova
     * SDK stream signals its version, and forcing one would override it.
     */
    if (this->forcedVersion >= 0)
      this->configureInt(dec, "force_bitstream_version", this->forcedVersion);
    if (this->initializeDecoder(dec) != LCEVC_Success)
      return this->fail("LCEVC_InitializeDecoder failed");
    this->timestamp = 0;
    return true;
  }

  bool decode(const std::vector<std::uint8_t> &annexB,
              const LcevcPlanes              &base,
              unsigned,
              LcevcDecodedPicture &out) override
  {
    if (!this->live)
      return false;
    const auto dec = this->decoder;
    // A base picture of an earlier call may come back late (its task is independent of the output).
    this->takeBackBases();

    // A reusable output picture; the decoder sizes it to the enhanced picture.
    LCEVC_PictureDesc outDesc;
    this->defaultPictureDesc(&outDesc, colorFormat(base.chromaFormat, this->outputDepth(base)), 2, 2);
    LCEVC_PictureHandle outPicture;
    if (this->allocPicture(dec, &outDesc, &outPicture) != LCEVC_Success)
      return this->fail("LCEVC_AllocPicture failed");
    if (this->sendPicture(dec, outPicture) != LCEVC_Success)
    {
      this->freePicture(dec, outPicture);
      return this->fail("LCEVC_SendDecoderPicture failed");
    }

    LCEVC_PictureDesc baseDesc;
    this->defaultPictureDesc(&baseDesc, colorFormat(base.chromaFormat, base.bitDepth), std::uint32_t(base.width),
                             std::uint32_t(base.height));
    /* The decoder reads the base from its own threads, possibly after this call gave up waiting.
     * Hand it a copy that lives until the decoder is destroyed (reset() does that before reusing it
     * after a failure), not the caller's buffer.
     */
    const int nPlanes  = base.chromaFormat == 0 ? 1 : 3;
    std::size_t total  = 0;
    for (int p = 0; p < nPlanes; ++p)
      total += std::size_t(base.stride[p]) * std::size_t(lcevcPlaneHeight(base.height, base.chromaFormat, p));
    this->baseStorage.resize(total);
    LCEVC_PicturePlaneDesc planes[3]{};
    std::size_t            offset = 0;
    for (int p = 0; p < nPlanes; ++p)
    {
      const auto bytes = std::size_t(base.stride[p]) * std::size_t(lcevcPlaneHeight(base.height, base.chromaFormat, p));
      std::memcpy(this->baseStorage.data() + offset, base.data[p], bytes);
      planes[p] = {this->baseStorage.data() + offset, std::uint32_t(base.stride[p])};
      offset += bytes;
    }
    LCEVC_PictureHandle basePicture;
    if (this->allocPictureExternal(dec, &baseDesc, nullptr, planes, &basePicture) != LCEVC_Success)
      return this->fail("LCEVC_AllocPictureExternal failed");

    const auto ts = this->timestamp++;
    if (this->sendEnhancement(dec, ts, annexB.data(), std::uint32_t(annexB.size())) != LCEVC_Success)
    {
      this->freePicture(dec, basePicture);
      return this->fail("LCEVC_SendDecoderEnhancementData failed");
    }
    if (this->sendBase(dec, ts, basePicture, 1000000, nullptr) != LCEVC_Success)
    {
      this->freePicture(dec, basePicture);
      return this->fail("LCEVC_SendDecoderBase failed");
    }

    LCEVC_PictureHandle     picture{};
    LCEVC_DecodeInformation information{};
    LCEVC_ReturnCode        rc = LCEVC_Again;
    for (int tries = 0; tries < 5000 && rc == LCEVC_Again; ++tries)
    {
      rc = this->receivePicture(dec, &picture, &information);
      if (rc == LCEVC_Again)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    this->takeBackBases();
    if (rc == LCEVC_Again)
    {
      /* No picture: a worker is stuck (seen with a misread temporal chunk), and LCEVC_DestroyDecoder
       * would then wait for it forever. Abandon the decoder instead - its thread spins until exit -
       * so that reset() starts a new one and the caller is not blocked.
       */
      this->live    = false;
      this->decoder = {};
      // Its threads may still hold the base picture: leave that memory to them (deliberately leaked).
      new std::vector<std::uint8_t>(std::move(this->baseStorage));
      this->baseStorage = {};
      return this->fail("LCEVCdec did not deliver the picture within 5 s; the decoder was abandoned");
    }
    if (rc != LCEVC_Success)
      return this->fail(QString("LCEVC_ReceiveDecoderPicture failed (%1)").arg(int(rc)));

    LCEVC_PictureDesc desc;
    std::uint32_t     planeCount = 0;
    this->getPictureDesc(dec, picture, &desc);
    this->getPlaneCount(dec, picture, &planeCount);
    LCEVC_PictureLockHandle lock;
    if (this->lockPicture(dec, picture, LCEVC_Access_Read, &lock) != LCEVC_Success)
    {
      this->freePicture(dec, picture);
      return this->fail("LCEVC_LockPicture failed");
    }
    const int bytesPerSample = this->outputDepth(base) > 8 ? 2 : 1;
    out.width                = int(desc.width - desc.cropLeft - desc.cropRight);
    out.height               = int(desc.height - desc.cropTop - desc.cropBottom);
    out.bitDepth             = this->outputDepth(base);
    out.chromaFormat         = base.chromaFormat;
    const void     *data[3]{};
    std::ptrdiff_t  stride[3]{};
    for (std::uint32_t p = 0; p < std::min(planeCount, 3u); ++p)
    {
      LCEVC_PicturePlaneDesc plane;
      this->getLockPlaneDesc(dec, lock, p, &plane);
      const auto cropX = p == 0 ? desc.cropLeft : desc.cropLeft * std::uint32_t(lcevcPlaneWidth(2, base.chromaFormat, 1)) / 2;
      const auto cropY = p == 0 ? desc.cropTop : desc.cropTop * std::uint32_t(lcevcPlaneHeight(2, base.chromaFormat, 1)) / 2;
      data[p]   = plane.firstSample + std::size_t(cropY) * plane.rowByteStride + std::size_t(cropX) * std::size_t(bytesPerSample);
      stride[p] = plane.rowByteStride;
    }
    packPlanes(out.yuv, data, stride, out.width, out.height, out.chromaFormat, bytesPerSample);
    this->unlockPicture(dec, lock);
    this->freePicture(dec, picture);
    return true;
  }

  //!< The output bit depth (the enhancement depth), when it differs from the base's.
  void setOutputDepth(int depth) override { this->enhancementDepth = depth; }
  void setForcedBitstreamVersion(int version) override { this->forcedVersion = version; }

private:
  int outputDepth(const LcevcPlanes &base) const
  {
    return this->enhancementDepth > 0 ? this->enhancementDepth : base.bitDepth;
  }
  bool fail(const QString &why)
  {
    this->error = why;
    return false;
  }
  void takeBackBases()
  {
    LCEVC_PictureHandle returned;
    while (this->receiveBase(this->decoder, &returned) == LCEVC_Success)
      this->freePicture(this->decoder, returned);
  }
  void destroy()
  {
    if (this->live)
      this->destroyDecoder(this->decoder);
    this->live = false;
  }

  LCEVC_DecoderHandle decoder{};
  bool                live{};
  std::uint64_t       timestamp{};
  int                 enhancementDepth{};
  int                 forcedVersion{1};
  std::vector<std::uint8_t> baseStorage; //!< The base picture the decoder reads (see decode())

  decltype(&LCEVC_CreateDecoder)              createDecoder{};
  decltype(&LCEVC_DestroyDecoder)             destroyDecoder{};
  decltype(&LCEVC_ConfigureDecoderBool)       configureBool{};
  decltype(&LCEVC_ConfigureDecoderInt)        configureInt{};
  decltype(&LCEVC_InitializeDecoder)          initializeDecoder{};
  decltype(&LCEVC_DefaultPictureDesc)         defaultPictureDesc{};
  decltype(&LCEVC_AllocPicture)               allocPicture{};
  decltype(&LCEVC_AllocPictureExternal)       allocPictureExternal{};
  decltype(&LCEVC_FreePicture)                freePicture{};
  decltype(&LCEVC_SendDecoderPicture)         sendPicture{};
  decltype(&LCEVC_SendDecoderEnhancementData) sendEnhancement{};
  decltype(&LCEVC_SendDecoderBase)            sendBase{};
  decltype(&LCEVC_ReceiveDecoderPicture)      receivePicture{};
  decltype(&LCEVC_ReceiveDecoderBase)         receiveBase{};
  decltype(&LCEVC_GetPictureDesc)             getPictureDesc{};
  decltype(&LCEVC_GetPicturePlaneCount)       getPlaneCount{};
  decltype(&LCEVC_LockPicture)                lockPicture{};
  decltype(&LCEVC_GetPictureLockPlaneDesc)    getLockPlaneDesc{};
  decltype(&LCEVC_UnlockPicture)              unlockPicture{};
};

} // namespace

std::unique_ptr<LcevcBackend> LcevcBackend::create(Kind kind)
{
  if (kind == Kind::Ltm)
    return std::make_unique<LtmBackend>();
  return std::make_unique<LcevcDecBackend>();
}

} // namespace bda::integration
