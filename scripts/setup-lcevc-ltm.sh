#!/usr/bin/env bash
# Build libbda-lcevc-ltm.so: the LTM (MPEG LCEVC Test Model) enhancement decoder + C wrapper.
#
#   bash scripts/setup-lcevc-ltm.sh
#     -> build/YUViewApp/decoder/libbda-lcevc-ltm.so   (dlopen target, API: third_party/lcevc-ltm-wrapper/bda_lcevc_ltm.h)
#     -> build/lcevc-ltm/test_ltm_wrapper              (verification CLI, see test_ltm_wrapper.cpp)
#
# Notes
#   - Source: third_party/ltm (git submodule, github.com/Jamaika1/LTM-master @ 452a9869). Override with
#     LTM_SRC=... to build from another checkout of the same commit.
#   - Only the decoder core is compiled (decoder/, util/, src/Types.cpp). No base codec (JM/HM/VTM), no
#     codec API loader, no ModelDecoderApp: the base picture is decoded by the caller.
#   - LTM writes intermediate surfaces only to files (--dump_surfaces) and exits the process on errors.
#     patches/0001-library-build-hooks.patch is applied to COPIES of three LTM files in the build
#     directory (never to the submodule) to capture surfaces in memory, throw instead of exit, and keep
#     stdout/stderr quiet. All patched code is guarded by BDA_LTM_LIBRARY.
#   - Same compiler and optimisation flags as LTM's CMake Release build (gcc-toolset-13, -O3
#     -fomit-frame-pointer -funroll-loops, forced include of the libstdc++ fixups), so results stay
#     bit-exact with ModelDecoder. libstdc++/libgcc are static; only bda_ltm_* symbols are exported.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Re-exec once in a clean environment so an RDA shell's gcc/libstdc++ cannot leak in.
if [[ -z "${BDA_LTM_CLEAN_ENV:-}" ]]; then
    exec env -i HOME="$HOME" USER="${USER:-}" PATH="/usr/bin:/bin:/usr/local/bin" \
        BDA_LTM_CLEAN_ENV=1 LTM_SRC="${LTM_SRC:-}" BUILD="${BUILD:-}" OUT="${OUT:-}" JOBS="${JOBS:-}" \
        TOOLSET="${TOOLSET:-}" bash "$0" "$@"
fi

LTM_SRC="${LTM_SRC:-$ROOT/third_party/ltm}"
WRAP="$ROOT/third_party/lcevc-ltm-wrapper"
BUILD="${BUILD:-$ROOT/build/lcevc-ltm}"
OUT="${OUT:-$ROOT/build/YUViewApp/decoder}"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
JOBS="${JOBS:-$(nproc)}"
LTM_REV_EXPECTED=452a9869b66c72a187b77d51cb7e5c3e9f86d38d

die() { echo "ERROR: $*" >&2; exit 2; }

[[ -f "$LTM_SRC/decoder/src/Decoder.cpp" ]] || die "LTM source not found at $LTM_SRC.
  git submodule update --init third_party/ltm   (do NOT init its nested base-codec submodules)"
LTM_REV="$(git -C "$LTM_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
[[ "$LTM_REV" == "$LTM_REV_EXPECTED" ]] || echo "WARNING: LTM at $LTM_REV, wrapper verified against $LTM_REV_EXPECTED" >&2

[[ -f "/opt/rh/$TOOLSET/enable" ]] || die "$TOOLSET is not installed (system g++ 8 cannot build LTM)"
set +u; source "/opt/rh/$TOOLSET/enable"; set -u

echo "== 1. patched copies =="
# The library is compiled from a copy of decoder/, util/ and src/ so the submodule is never touched.
PATCHED="$BUILD/patched"
rm -rf "$PATCHED" "$BUILD/obj" && mkdir -p "$PATCHED" "$BUILD/obj"
cp -r "$LTM_SRC/decoder" "$LTM_SRC/util" "$LTM_SRC/src" "$PATCHED/"
patch -s -p1 -d "$PATCHED" < "$WRAP/patches/0001-library-build-hooks.patch" || die "patch did not apply"
find "$PATCHED" -name '*.orig' -delete
# LTM checks bitstream bounds with assert(), which would abort() the host application on a malformed
# stream. Route every assert() in the copied sources to bda_ltm_assert.h, which throws instead (the
# check itself is kept, so valid streams decode exactly as with ModelDecoder).
grep -rlE '(^|[^_[:alnum:]])assert\(' "$PATCHED" --include='*.cpp' --include='*.hpp' --include='*.h' \
    | xargs -r sed -i -E 's/(^|[^_[:alnum:]])assert\(/\1BDA_LTM_ASSERT(/g'

# LTM relies on transitive standard includes that newer libstdc++ no longer provides
# (same list as lcevc-encoder-ai scripts/ref/build_ltm_upstream.sh).
printf '#include <%s>\n' cstdint cstring cstdlib cstdio climits limits algorithm functional memory > "$BUILD/ltm_fixups.h"

echo "== 2. compile =="
SRCS=()
for f in Decoder Add Conform Convert Deblocking Deserializer Dimensions Dithering EntropyDecoder Expand HuffmanDecoder \
         InverseQuantize InverseTransformDD InverseTransformDDS InverseTransformDDS_1D InverseTransformDD_1D \
         PredictedResidual ScanEnhancement TemporalDecode Upsampling; do
    SRCS+=("$PATCHED/decoder/src/$f.cpp")
done
for f in BitstreamStatistic BitstreamUnpacker Buffer Component Diagnostics Image LcevcMd5 Misc Packet Surface \
         YUVReader YUVWriter; do
    SRCS+=("$PATCHED/util/src/$f.cpp")
done
SRCS+=("$PATCHED/src/Types.cpp" "$WRAP/bda_lcevc_ltm.cpp")

CXXFLAGS=(
    -std=c++11 -g -O3 -fomit-frame-pointer -funroll-loops
    -fPIC -fvisibility=hidden -fvisibility-inlines-hidden
    -include "$BUILD/ltm_fixups.h" -include "$WRAP/bda_ltm_assert.h"
    -DBITSTREAM_DEBUG=0 -DUSE_SEI_NALU=0
    -DBDA_LTM_LIBRARY -DDIAGNOSTICS_ERR_THROWS -DBDA_LTM_SRC_REV="\"${LTM_REV:0:8}\""
    -I"$WRAP" -I"$PATCHED/util/include" -I"$PATCHED/decoder/include" -I"$PATCHED/src"
    -I"$LTM_SRC/encoder/include" -I"$LTM_SRC/common/include"
)
printf '%s\n' "${SRCS[@]}" | xargs -P "$JOBS" -I{} bash -c '
    src="$1"; shift
    # <grandparent dir>_<file>.o keeps names unique (decoder_X, util_X, patched_Types, third_party_bda_lcevc_ltm).
    obj="'"$BUILD"'/obj/$(basename "$(dirname "$(dirname "$src")")")_$(basename "$src").o"
    g++ "$@" -c "$src" -o "$obj" || { echo "FAILED: $src" >&2; exit 255; }
' _ {} "${CXXFLAGS[@]}"

echo "== 3. link =="
mkdir -p "$OUT"
# libstdc++ headers force default visibility on std:: template instantiations; a version script keeps
# them (and everything else that is not bda_ltm_*) local so they cannot clash with the host's libstdc++.
printf '{\n  global: bda_ltm_*;\n  local: *;\n};\n' > "$BUILD/exports.map"
g++ -shared -o "$BUILD/libbda-lcevc-ltm.so" "$BUILD"/obj/*.o \
    -Wl,-soname,libbda-lcevc-ltm.so -Wl,-z,defs -Wl,--exclude-libs,ALL -Wl,--version-script="$BUILD/exports.map" \
    -static-libstdc++ -static-libgcc -lm -ldl -lpthread
# Deployed copy without debug info (the build copy keeps it for gdb).
objcopy --strip-debug "$BUILD/libbda-lcevc-ltm.so" "$OUT/libbda-lcevc-ltm.so"

echo "== 4. test CLI =="
g++ -std=c++11 -O2 -I"$WRAP" "$WRAP/test_ltm_wrapper.cpp" -o "$BUILD/test_ltm_wrapper" -ldl \
    -static-libstdc++ -static-libgcc

echo "== 5. check =="
SO="$OUT/libbda-lcevc-ltm.so"
ldd "$SO" | grep -q 'libstdc++' && die "$SO depends on libstdc++ (static link flags not applied)"
ldd "$SO" | grep -q 'not found' && die "$SO has unresolved dependencies"
exported="$(nm -D --defined-only "$SO" | awk '$2 ~ /^[TDBVW]$/ {print $3}' | sort)"
leaked="$(echo "$exported" | grep -v '^bda_ltm_' | grep -v '^_\(init\|fini\)$' || true)"
[[ -z "$leaked" ]] || die "unexpected exported symbols: $(echo "$leaked" | head -5 | tr '\n' ' ')"
echo "   exports: $(echo "$exported" | tr '\n' ' ')"
echo "   $SO ($(stat -c %s "$SO") bytes, LTM ${LTM_REV:0:8})"
echo "OK"
