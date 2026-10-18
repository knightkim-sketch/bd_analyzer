#!/usr/bin/env bash
# Build V-Nova LCEVCdec (MPEG-5 Part 2 decoder SDK) as shared libraries for playback.
#
#   bash scripts/setup-lcevcdec.sh
#     -> $PREFIX/lib/liblcevc_dec_api.so, liblcevc_dec_pipeline_cpu.so (+ headers)
#     -> copies the runtime .so files into build/YUViewApp/decoder/
#
# Notes
#   - Source: github.com/v-novaltd/LCEVCdec, tag $LCEVCDEC_TAG (latest 4.x at the time of writing).
#     Licence is BSD-3-Clause-Clear: NO patent licence is granted. Playback backend only;
#     the analysis/reference backend is LTM (scripts/setup-lcevc-ltm.sh).
#   - Only the libraries are built (VN_SDK_EXECUTABLES=OFF): no FFmpeg / gtest / json deps.
#   - System g++ 8 is too old (needs GCC >= 9); gcc-toolset-13 is used. libstdc++/libgcc are linked
#     statically so the bundle does not need the toolset runtime.
#   - Network is required for the first clone. If it fails, the script stops (no silent fallback).
set -euo pipefail

LCEVCDEC_TAG="${LCEVCDEC_TAG:-4.3.0}"
LCEVCDEC_URL="${LCEVCDEC_URL:-https://github.com/v-novaltd/LCEVCdec.git}"
SRC="${SRC:-$HOME/opt/src/LCEVCdec}"
BUILD="${BUILD:-$SRC/build-bda}"
PREFIX="${PREFIX:-$HOME/opt/lcevcdec}"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
JOBS="${JOBS:-$(nproc)}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/build/YUViewApp/decoder}"

die() { echo "ERROR: $*" >&2; exit 2; }

echo "== 1. source ($LCEVCDEC_TAG) =="
if [[ ! -d "$SRC/.git" ]]; then
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 1 --branch "$LCEVCDEC_TAG" "$LCEVCDEC_URL" "$SRC" \
        || die "clone failed (network?). Nothing was built."
fi
have_tag="$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)"
[[ "$have_tag" == "$LCEVCDEC_TAG" ]] || die "$SRC is at '${have_tag:-unknown}', expected $LCEVCDEC_TAG"
echo "   $(git -C "$SRC" rev-parse HEAD)"

echo "== 2. configure + build =="
[[ -f "/opt/rh/$TOOLSET/enable" ]] || die "$TOOLSET is not installed"
command -v ninja >/dev/null || die "ninja not found"
# Python >= 3.9 is used by the CMake scripts; prefer python3.11 when the default is older.
PY="$(command -v python3.11 || command -v python3)"

# Configure and build in a clean environment so an RDA/other toolchain on PATH cannot leak in.
# RUNPATH: upstream forces $ORIGIN/../lib (cmake/modules/Platform/Linux.cmake). The bundle keeps both
# libraries side by side in decoder/, so $ORIGIN is prepended through the linker flags. The backslash
# keeps the literal "$ORIGIN" through the ninja -> /bin/sh command line.
env -i HOME="$HOME" USER="${USER:-}" PATH="/usr/bin:/bin:/usr/local/bin" \
    SRC="$SRC" BUILD="$BUILD" PREFIX="$PREFIX" PY="$PY" JOBS="$JOBS" TOOLSET="$TOOLSET" \
    bash -s <<'INNER'
set -euo pipefail
source "/opt/rh/$TOOLSET/enable"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DPython3_EXECUTABLE="$PY" \
    -DBUILD_SHARED_LIBS=ON \
    -DVN_SDK_EXECUTABLES=OFF -DVN_SDK_UNIT_TESTS=OFF -DVN_SDK_JSON_CONFIG=OFF \
    -DVN_SDK_PIPELINE_CPU=ON -DVN_SDK_PIPELINE_VULKAN=OFF \
    -DVN_SDK_SYSTEM_INSTALL=OFF -DVN_SDK_SAMPLE_SOURCE=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS='-static-libstdc++ -static-libgcc -Wl,-rpath,\$ORIGIN' \
    > "$BUILD.cmake.log" 2>&1 || { tail -40 "$BUILD.cmake.log"; exit 1; }
cmake --build "$BUILD" -j "$JOBS" > "$BUILD.build.log" 2>&1 || { tail -40 "$BUILD.build.log"; exit 1; }
cmake --install "$BUILD" > "$BUILD.install.log" 2>&1 || { tail -20 "$BUILD.install.log"; exit 1; }
INNER

echo "== 3. deploy to $OUT =="
mkdir -p "$OUT"
LIBDIR="$PREFIX/lib"
[[ -d "$LIBDIR" ]] || LIBDIR="$PREFIX/lib64"
for name in liblcevc_dec_api liblcevc_dec_pipeline_cpu; do
    so="$(find "$LIBDIR" -maxdepth 1 -name "$name.so*" -type f | sort | head -1)"
    [[ -n "$so" ]] || die "$name.so not found under $LIBDIR"
    # Copy the real file under its plain .so name plus every soname symlink target name.
    cp -p "$so" "$OUT/$name.so"
    soname="$(objdump -p "$so" | awk '/SONAME/{print $2}')"
    [[ -n "$soname" && "$soname" != "$name.so" ]] && cp -p "$so" "$OUT/$soname"
    echo "   $OUT/$name.so  (soname ${soname:-none})"
done

echo "== 4. check =="
for so in "$OUT"/liblcevc_dec_api.so "$OUT"/liblcevc_dec_pipeline_cpu.so; do
    if ldd "$so" | grep -q 'libstdc++'; then
        die "$so still depends on libstdc++ (static link flags not applied)"
    fi
    # No LD_LIBRARY_PATH here: the libraries must find each other through RUNPATH alone.
    ldd "$so" | grep -q 'not found' && die "$so has unresolved dependencies"
    objdump -p "$so" | grep -E 'RUNPATH|RPATH' | grep -q '\$ORIGIN:\|\$ORIGIN$' \
        || die "$so RUNPATH does not contain \$ORIGIN"
done
nm -D --defined-only "$OUT/liblcevc_dec_api.so" | grep -q ' LCEVC_CreateDecoder$' \
    || die "LCEVC_CreateDecoder is not exported"
echo "OK"
