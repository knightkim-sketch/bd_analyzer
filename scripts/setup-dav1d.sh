#!/usr/bin/env bash
# dav1d analyzer 디코더 빌드 (YUView 가 dlopen 하는 libdav1d-internals.so)
#
# 업스트림은 ChristianFeldmann/dav1d 의 태그 0.2.1.0.analyze (커밋 0a7f210) 이다.
# 이 리포에서 추가한 것: Av1Block 의 bitstream_start_bit / bitstream_end_bit 와
# dav1d_analyzer_block_data_size(). third_party/dav1d/README.md 참조.
#
# ⚠️ 이 라이브러리와 YUViewLib 는 Av1Block 레이아웃을 공유한다. YUViewLib.pro 의
#    YUVIEW_DAV1D_AV1BLOCK_HAS_BITSTREAM_RANGE 와 반드시 짝이 맞아야 하며, 맞지 않으면
#    decoderDav1d 가 로딩을 거부한다 (틀린 stride 로 읽어 쓰레기 통계를 내는 것보다 낫다).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAV1D="$ROOT/third_party/dav1d/upstream"
BUILD="${DAV1D_BUILD:-$DAV1D/build}"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
JOBS="${JOBS:-$(nproc)}"
OUT="$ROOT/build/YUViewApp/decoder"

die() { echo "ERROR: $*" >&2; exit 2; }

[[ -f "$DAV1D/meson.build" ]] || die "dav1d 소스가 없습니다. 먼저:
  git clone --branch 0.2.1.0.analyze https://github.com/ChristianFeldmann/dav1d.git '$DAV1D'
  그 다음 third_party/dav1d/*.patch 를 적용하세요 (README.md 참조)."

# meson 은 시스템에 없다. pip --user 로 넣는다 (python3.11).
if ! command -v meson >/dev/null 2>&1; then
    export PATH="$HOME/.local/bin:$PATH"
fi
command -v meson >/dev/null 2>&1 || die "meson 이 없습니다: pip3 install --user 'meson==1.2.3'"
command -v ninja >/dev/null 2>&1 || die "ninja 가 없습니다."

# build_asm=false: nasm 이 시스템에 없다. AV1 디코딩 정확도에는 영향이 없다
# (ffmpeg 의 libdav1d 1.x 와 출력 md5 가 일치하는 것을 확인했다). 속도만 느려진다.
if [[ ! -f "$BUILD/build.ninja" ]]; then
    scl enable "$TOOLSET" -- meson setup "$BUILD" "$DAV1D" \
        --buildtype release \
        -Dbuild_asm=false -Dbuild_tools=false -Dbuild_tests=false \
        -Ddefault_library=shared
fi

scl enable "$TOOLSET" -- ninja -C "$BUILD" -j "$JOBS"

SO="$(find "$BUILD/src" -maxdepth 1 -name 'libdav1d.so.*' -type f | head -1)"
[[ -n "$SO" ]] || die "libdav1d.so 를 찾지 못했습니다."

mkdir -p "$OUT"
cp "$SO" "$OUT/libdav1d-internals.so"
echo
echo "설치: $OUT/libdav1d-internals.so"
echo "  <- $SO"
echo
echo "확인: nm -D --defined-only 로 dav1d_analyzer_block_data_size 가 보여야 한다."
nm -D --defined-only "$OUT/libdav1d-internals.so" | grep analyzer_block_data_size \
    || die "dav1d_analyzer_block_data_size 심볼이 없습니다. 패치가 적용되지 않았습니다."
