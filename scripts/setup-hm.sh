#!/usr/bin/env bash
# Build libHMDecoder.so: the HM 16.15 HEVC reference decoder with IENT's libHM C wrapper.
#
#   bash scripts/setup-hm.sh
#     -> build/YUViewApp/decoder/libHMDecoder.so   (dlopen target of YUView's decoderHM, API:
#                                                   source/App/libHMDecoder/libHMDecoder.h in the tarball)
#
# Notes
#   - Source: third_party/libhm/libHM-HM16.15_v1.02.tar.gz (github.com/IENT/libHM release HM16.15_v1.02,
#     the version YUView's decoderHM is written against). The sha256 is checked; override with
#     HM_TARBALL=... to build another copy of the same release.
#   - Built with libHM's own makefiles (release, non-HighBitDepth like YUView's binaries) from an
#     extracted copy in the build directory. -Werror is dropped: gcc 13 warns (class-memaccess,
#     array-bounds) on code that older compilers accepted, and third-party warnings are not ours to fix.
#   - patches/0001-eof-flush.patch is applied to the extracted copy: libHMDec_push_nal_unit() accepts the
#     empty end-of-stream push, so the last pictures of a stream are output (without it the last frames
#     of every stream are missing or wrong).
#   - The static libraries are then relinked like libbda-jm-analyzer.so: libstdc++/libgcc are static and
#     only the libHMDec_* / libHMDEC_* API functions are exported, so HM's C++ classes cannot clash with
#     another decoder library loaded into the same process.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLSET="${TOOLSET:-gcc-toolset-13}"

# Re-exec once in a clean environment under the toolset so an RDA shell's gcc/libstdc++ cannot leak in.
if [[ -z "${BDA_HM_CLEAN_ENV:-}" ]]; then
    [[ -f "/opt/rh/$TOOLSET/enable" ]] || { echo "ERROR: $TOOLSET is not installed" >&2; exit 2; }
    exec env -i HOME="$HOME" USER="${USER:-}" PATH="/usr/bin:/bin:/usr/local/bin" \
        BDA_HM_CLEAN_ENV=1 HM_TARBALL="${HM_TARBALL:-}" BUILD="${BUILD:-}" OUT="${OUT:-}" JOBS="${JOBS:-}" \
        TOOLSET="$TOOLSET" scl enable "$TOOLSET" -- bash "$0" "$@"
fi

HM_TARBALL="${HM_TARBALL:-$ROOT/third_party/libhm/libHM-HM16.15_v1.02.tar.gz}"
HM_SHA256_EXPECTED=3c1c1b129dc2467c31436dd48666f021c4eb08a3a35f58b9ff098e2cc635add8
BUILD="${BUILD:-$ROOT/build/hm}"
OUT="${OUT:-$ROOT/build/YUViewApp/decoder}"
JOBS="${JOBS:-$(nproc)}"
# Every function decoderHM::resolveLibraryFunctionPointers() resolves.
EXPORTS=(libHMDec_get_version libHMDec_new_decoder libHMDec_free_decoder libHMDec_set_SEI_Check
         libHMDec_set_max_temporal_layer libHMDec_push_nal_unit libHMDec_get_picture
         libHMDEC_get_POC libHMDEC_get_picture_width libHMDEC_get_picture_height libHMDEC_get_picture_stride
         libHMDEC_get_image_plane libHMDEC_get_chroma_format libHMDEC_get_internal_bit_depth
         libHMDEC_get_internal_type_number libHMDEC_get_internal_type_name libHMDEC_get_internal_type
         libHMDEC_get_internal_type_max libHMDEC_get_internal_type_vector_scaling
         libHMDEC_get_internal_type_description libHMDEC_get_internal_info libHMDEC_clear_internal_info)

die() { echo "ERROR: $*" >&2; exit 2; }

[[ -f "$HM_TARBALL" ]] || die "libHM tarball not found at $HM_TARBALL"
HM_SHA256="$(sha256sum "$HM_TARBALL" | cut -d' ' -f1)"
[[ "$HM_SHA256" == "$HM_SHA256_EXPECTED" ]] || echo "WARNING: $HM_TARBALL sha256 $HM_SHA256, verified against $HM_SHA256_EXPECTED" >&2
gcc --version | head -1 | grep -q ' 13\.' || die "gcc 13 expected ($TOOLSET), got: $(gcc --version | head -1)"

echo "== 1. extract =="
rm -rf "$BUILD" && mkdir -p "$BUILD"
tar xzf "$HM_TARBALL" -C "$BUILD"
SRC="$(find "$BUILD" -mindepth 1 -maxdepth 1 -type d -name 'libHM-*' | head -1)"
[[ -f "$SRC/build/linux/common/makefile.base" ]] || die "unexpected tarball layout (no build/linux/common/makefile.base)"
sed -i 's/ -Werror//' "$SRC/build/linux/common/makefile.base"
# The wrapper rejected the empty end-of-stream push of YUView's decoderHM, so the last pictures were
# never filtered or output (see the patch).
patch -s -p1 -d "$SRC" < "$ROOT/third_party/libhm/patches/0001-eof-flush.patch" || die "patch did not apply"

echo "== 2. compile (libHM makefiles, release) =="
for d in lib/TLibCommon lib/TLibDecoder app/libHMDecoder; do
    make -C "$SRC/build/linux/$d" release -j"$JOBS" >"$BUILD/$(basename "$d").log" 2>&1 \
        || { tail -30 "$BUILD/$(basename "$d").log" >&2; die "make $d failed (log: $BUILD/$(basename "$d").log)"; }
done

echo "== 3. link =="
OBJ="$SRC/build/linux/app/libHMDecoder/objects/libHMDecoder.r.o"
[[ -f "$OBJ" ]] || die "missing $OBJ"
{ echo "{"; echo "  global:"; printf '    %s;\n' "${EXPORTS[@]}"; echo "  local: *;"; echo "};"; } > "$BUILD/exports.map"
mkdir -p "$OUT"
g++ -shared -o "$BUILD/libHMDecoder.so" "$OBJ" "$SRC/lib/libTLibDecoderStatic.a" "$SRC/lib/libTLibCommonStatic.a" \
    -Wl,-soname,libHMDecoder.so -Wl,-z,defs -Wl,--exclude-libs,ALL -Wl,--version-script="$BUILD/exports.map" \
    -static-libstdc++ -static-libgcc -lm -ldl -lpthread
strip --strip-unneeded -o "$OUT/libHMDecoder.so" "$BUILD/libHMDecoder.so"

echo "== 4. check =="
SO="$OUT/libHMDecoder.so"
ldd "$SO" | grep -q 'libstdc++' && die "$SO depends on libstdc++ (static link flags not applied)"
ldd "$SO" | grep -q 'not found' && die "$SO has unresolved dependencies"
exported="$(nm -D --defined-only "$SO" | awk '$2 ~ /^[TDBVW]$/ {print $3}' | sort)"
expected="$(printf '%s\n' "${EXPORTS[@]}" | sort)"
[[ "$exported" == "$expected" ]] || die "unexpected exported symbols: $(echo "$exported" | tr '\n' ' ')"
echo "   exports: ${#EXPORTS[@]} libHMDec_*/libHMDEC_* functions"
echo "   $SO ($(stat -c %s "$SO") bytes, $(basename "$SRC"))"
echo "OK"
