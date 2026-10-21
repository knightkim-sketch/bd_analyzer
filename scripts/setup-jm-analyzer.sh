#!/usr/bin/env bash
# Build libbda-jm-analyzer.so: the JM 19.0 H.264/AVC reference decoder + C wrapper with macroblock data.
#
#   bash scripts/setup-jm-analyzer.sh
#     -> build/YUViewApp/decoder/libbda-jm-analyzer.so  (dlopen target, API: third_party/jm-analyzer/bda_jm.h)
#     -> build/jm-analyzer/test_jm_analyzer             (verification CLI, see test_jm_analyzer.cpp)
#     -> build/jm-analyzer/ldecod, ldecod-trace         (JM's own decoder, unmodified / TRACE=1, reference)
#
# Notes
#   - Source: third_party/jm (git submodule, vcgit.hhi.fraunhofer.de/jvet/JM @ tag JM-19.0 = ef2362d1).
#     Override with JM_SRC=... to build from another checkout of the same commit.
#   - The library is compiled from a copy of ldecod/ and lcommon/ (never the submodule) patched with
#     patches/0001-library-hooks.patch: bytes come from bda_jm_push() instead of a file, output pictures
#     and per-macroblock data go to the wrapper instead of a YUV file. Every patched line is guarded by
#     BDA_JM_LIBRARY. bda_jm_lib.h is force-included: printf/fprintf(stdout|stderr) are silenced,
#     exit()/error()/assert() unwind to the wrapper instead of terminating the host.
#   - Same C flags as JM's ldecod Makefile (-std=gnu99 -O3 -ffloat-store -fno-strict-aliasing
#     -fsigned-char) plus -fcommon (JM defines globals in headers; gcc >= 10 defaults to -fno-common), so
#     the library output is bit-exact with the ldecod CLI built here. libstdc++/libgcc are static; only
#     the bda_jm_* API functions are exported.
#   - ldecod / ldecod-trace are built from an unpatched copy; the TRACE variant only flips TRACE to 1 in
#     ldecod/inc/defines.h (it writes trace_dec.txt into the current directory).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLSET="${TOOLSET:-gcc-toolset-13}"

# Re-exec once in a clean environment under the toolset so an RDA shell's gcc/libstdc++ cannot leak in.
if [[ -z "${BDA_JM_CLEAN_ENV:-}" ]]; then
    [[ -f "/opt/rh/$TOOLSET/enable" ]] || { echo "ERROR: $TOOLSET is not installed" >&2; exit 2; }
    exec env -i HOME="$HOME" USER="${USER:-}" PATH="/usr/bin:/bin:/usr/local/bin" \
        BDA_JM_CLEAN_ENV=1 JM_SRC="${JM_SRC:-}" BUILD="${BUILD:-}" OUT="${OUT:-}" JOBS="${JOBS:-}" \
        TOOLSET="$TOOLSET" scl enable "$TOOLSET" -- bash "$0" "$@"
fi

JM_SRC="${JM_SRC:-$ROOT/third_party/jm}"
WRAP="$ROOT/third_party/jm-analyzer"
BUILD="${BUILD:-$ROOT/build/jm-analyzer}"
OUT="${OUT:-$ROOT/build/YUViewApp/decoder}"
JOBS="${JOBS:-$(nproc)}"
JM_REV_EXPECTED=ef2362d159339d583fc18cea2de4d4bed5a1417c
EXPORTS=(bda_jm_api_version bda_jm_version_string bda_jm_create bda_jm_destroy bda_jm_set_logging
         bda_jm_push bda_jm_pull bda_jm_last_error)

die() { echo "ERROR: $*" >&2; exit 2; }

[[ -f "$JM_SRC/ldecod/src/ldecod.c" ]] || die "JM source not found at $JM_SRC.
  git submodule update --init third_party/jm"
JM_REV="$(git -C "$JM_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
[[ "$JM_REV" == "$JM_REV_EXPECTED" ]] || echo "WARNING: JM at $JM_REV, wrapper verified against $JM_REV_EXPECTED (JM-19.0)" >&2
gcc --version | head -1 | grep -q ' 13\.' || die "gcc 13 expected ($TOOLSET), got: $(gcc --version | head -1)"

# Same as ldecod/Makefile (CFLAGS + FLAGS, OPT=3); -w: third-party warnings are not ours to fix.
JM_CFLAGS=(-std=gnu99 -ffloat-store -fno-strict-aliasing -fsigned-char -w -D__USE_LARGEFILE64 -D_FILE_OFFSET_BITS=64
           -O3 -fcommon)

# compile_all OBJDIR CFLAGS... -- SOURCES...: parallel compilation, one object per source.
compile_all() {
    local objdir="$1"; shift
    local flags=() srcs=()
    while [[ "$1" != "--" ]]; do flags+=("$1"); shift; done
    shift; srcs=("$@")
    mkdir -p "$objdir"; rm -f "$objdir/FAILED"
    for src in "${srcs[@]}"; do
        while (( $(jobs -rp | wc -l) >= JOBS )); do wait -n || true; done
        { gcc "${flags[@]}" -c "$src" -o "$objdir/$(basename "$(dirname "$(dirname "$src")")")_$(basename "$src" .c).o" \
            || { echo "FAILED: $src" >&2; touch "$objdir/FAILED"; }; } &
    done
    wait
    [[ ! -e "$objdir/FAILED" ]] || die "compilation failed"
}

echo "== 1. patched copy =="
PATCHED="$BUILD/patched"
rm -rf "$PATCHED" "$BUILD/obj" && mkdir -p "$PATCHED"
cp -r "$JM_SRC/ldecod" "$JM_SRC/lcommon" "$PATCHED/"
patch -s -p1 -d "$PATCHED" < "$WRAP/patches/0001-library-hooks.patch" || die "patch did not apply"
find "$PATCHED" -name '*.orig' -delete
# A failed assert() would abort() the host application on a malformed stream. Route every assert() in
# the copied sources to BDA_JM_ASSERT (bda_jm_lib.h), which unwinds to the wrapper instead.
grep -rlE '(^|[^_[:alnum:]])assert[[:space:]]*\(' "$PATCHED" --include='*.c' --include='*.h' \
    | xargs -r sed -i -E 's/(^|[^_[:alnum:]])assert[[:space:]]*\(/\1BDA_JM_ASSERT(/g'

echo "== 2. compile library =="
LIB_SRCS=()
for f in "$PATCHED"/ldecod/src/*.c "$PATCHED"/lcommon/src/*.c; do
    [[ "$(basename "$f")" == decoder_test.c ]] || LIB_SRCS+=("$f")   # decoder_test.c holds ldecod's main()
done
compile_all "$BUILD/obj" "${JM_CFLAGS[@]}" -g -fPIC -fvisibility=hidden -DBDA_JM_LIBRARY -include "$WRAP/bda_jm_lib.h" \
    -I"$PATCHED/ldecod/inc" -I"$PATCHED/lcommon/inc" -- "${LIB_SRCS[@]}"
g++ -std=c++14 -g -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wno-narrowing \
    -DBDA_JM_LIBRARY -DBDA_JM_SRC_REV="\"${JM_REV:0:8}\"" -D__USE_LARGEFILE64 -D_FILE_OFFSET_BITS=64 \
    -I"$WRAP" -I"$PATCHED/ldecod/inc" -I"$PATCHED/lcommon/inc" \
    -c "$WRAP/bda_jm.cpp" -o "$BUILD/obj/wrapper_bda_jm.o"

echo "== 3. link =="
mkdir -p "$OUT"
# Explicit export list: the hooks JM calls are named bda_jm_hook_* and must stay local.
{ echo "{"; echo "  global:"; printf '    %s;\n' "${EXPORTS[@]}"; echo "  local: *;"; echo "};"; } > "$BUILD/exports.map"
g++ -shared -o "$BUILD/libbda-jm-analyzer.so" "$BUILD"/obj/*.o \
    -Wl,-soname,libbda-jm-analyzer.so -Wl,-z,defs -Wl,--exclude-libs,ALL -Wl,--version-script="$BUILD/exports.map" \
    -static-libstdc++ -static-libgcc -lm -ldl -lpthread
# Deployed copy without debug info (the build copy keeps it for gdb).
objcopy --strip-debug "$BUILD/libbda-jm-analyzer.so" "$OUT/libbda-jm-analyzer.so"

echo "== 4. test CLI =="
g++ -std=c++14 -O2 -I"$WRAP" "$WRAP/test_jm_analyzer.cpp" -o "$BUILD/test_jm_analyzer" -ldl -lpthread \
    -static-libstdc++ -static-libgcc

echo "== 5. reference ldecod (unpatched; normal and TRACE=1) =="
for variant in ldecod ldecod-trace; do
    SRC="$BUILD/src-$variant"
    rm -rf "$SRC" && mkdir -p "$SRC"
    cp -r "$JM_SRC/ldecod" "$JM_SRC/lcommon" "$SRC/"
    if [[ "$variant" == ldecod-trace ]]; then
        sed -i -E 's/^(# define TRACE +)0/\11/' "$SRC/ldecod/inc/defines.h"
        grep -q '^# define TRACE           1' "$SRC/ldecod/inc/defines.h" || die "could not enable TRACE"
    fi
    compile_all "$SRC/obj" "${JM_CFLAGS[@]}" -I"$SRC/ldecod/inc" -I"$SRC/lcommon/inc" -- \
        "$SRC"/ldecod/src/*.c "$SRC"/lcommon/src/*.c
    gcc -o "$BUILD/$variant" "$SRC"/obj/*.o -lm
done

echo "== 6. check =="
SO="$OUT/libbda-jm-analyzer.so"
# Every JM assert must have been rerouted (a remaining one would abort() the host on a broken stream).
for o in "$BUILD"/obj/*.o; do
    nm -u "$o" | grep -q '__assert_fail' && die "$(basename "$o") still calls assert()"
done
ldd "$SO" | grep -q 'libstdc++' && die "$SO depends on libstdc++ (static link flags not applied)"
ldd "$SO" | grep -q 'not found' && die "$SO has unresolved dependencies"
exported="$(nm -D --defined-only "$SO" | awk '$2 ~ /^[TDBVW]$/ {print $3}' | sort)"
expected="$(printf '%s\n' "${EXPORTS[@]}" | sort)"
[[ "$exported" == "$expected" ]] || die "unexpected exported symbols: $(echo "$exported" | tr '\n' ' ')"
echo "   exports: $(echo "$exported" | tr '\n' ' ')"
echo "   $SO ($(stat -c %s "$SO") bytes, JM ${JM_REV:0:8})"
echo "   $BUILD/ldecod, $BUILD/ldecod-trace, $BUILD/test_jm_analyzer"
echo "OK"
