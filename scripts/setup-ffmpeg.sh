#!/usr/bin/env bash
# FFmpeg 공유 라이브러리 설치 (AV1 / 컨테이너 분석용)
#
# 근거: docs/ai/10-research/ffmpeg-integration.md
#
#   - YUView 는 FFmpeg 를 빌드 타임에 링크하지 않는다. QLibrary(dlopen) 로 런타임에
#     열고 심볼을 이름으로 resolve 한다 (FFmpegLibraryFunctions.cpp:185,270).
#     -> 정적 라이브러리(.a)는 원리적으로 사용 불가. 반드시 .so 여야 한다.
#   - AV1 분석은 FFmpeg 없이는 전면 불가: ParserAV1OBU 로 가는 유일한 경로가
#     ParserAVFormat(=libavformat demux) 이다. InputFormat enum 에 raw OBU 분기가 없다
#     (FileSource.h:48-55, ParserAVFormat.cpp:597).
#   - 버전 상한은 FFmpeg 7.1: 지원 조합 목록의 최신값이 (59,61,61,5) 이고
#     (FFmpegVersionHandler.cpp:104-110), FFmpeg 8 은 avcodec 62 에서 av_init_packet 을
#     제거해 필수 심볼 바인딩이 깨진다.
#   - rpmfusion 의 ffmpeg-libs(4.4.8) 는 지원 조합에 들어가지만 채택하지 않는다:
#     sudo 필요 + 시스템 패키지 48개를 끌어와 AppImage 번들이 비대해진다.
#   - nasm 은 Rocky 8 활성 repo 에 없고 sudo 도 없으므로 소스에서 로컬 빌드한다.
#     (--disable-x86asm 로 우회하면 디코딩 성능이 크게 떨어진다)
#   - ⚠️ --enable-libdav1d 는 필수다. FFmpeg 7.1 의 native `av1` 디코더는 **hwaccel 전용**이라
#     소프트웨어 디코딩을 못 한다 (libavcodec/av1dec.c:659-668 이 hwaccel 없으면
#     AVERROR(ENOSYS) = -38 을 돌려준다. 소스 주석: "the av1 decoder doesn't support
#     native decode"). CONFIG_AV1_DECODER=yes 만 보고 된다고 착각하기 쉽다 — 실측으로 확인했다.
#     이게 없으면 YUView 의 FFmpeg 디코더로 AV1 을 열면 빈 화면이 된다.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.2}"
# upstream flatpak 매니페스트(de.rwth_aachen.ient.YUView.yaml)와 동일한 tarball
FFMPEG_SHA256="089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304"
NASM_VERSION="${NASM_VERSION:-2.16.03}"
# FFmpeg 에 내장할 dav1d. YUView 의 블록 통계용 fork(0.2.2)와는 **완전히 다른 것**이다.
# 여기 것은 최신 stable 이고 static 으로 libavcodec.so 안에 흡수된다 (외부 .so 가 늘지 않는다).
# FFmpeg 7.1 의 최소 요건은 dav1d >= 0.5.0 이라 0.2.2 fork 로는 대체할 수 없다.
DAV1D_VERSION="${DAV1D_VERSION:-1.4.3}"

PREFIX="${PREFIX:-$HOME/opt/ffmpeg-${FFMPEG_VERSION%.*}}"
TOOLS="${TOOLS:-$HOME/opt/tools}"
WORK="${WORK:-${TMPDIR:-/tmp}/bd-ffmpeg-build}"
MESON_VENV="${MESON_VENV:-$HOME/.venv-meson}"
DAV1D_PREFIX="${DAV1D_PREFIX:-$WORK/dav1d-$DAV1D_VERSION-install}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# YUView 의 탐색 경로 중 하나가 applicationDirPath()+"/ffmpeg/" 다
# (FFmpegVersionHandler.cpp:368). 여기에 놓으면 설정 파일을 건드릴 필요가 없다.
DEPLOY="${DEPLOY:-$ROOT/build/YUViewApp/ffmpeg}"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$WORK"

echo "== 1. nasm $NASM_VERSION =="
if [[ -x "$TOOLS/bin/nasm" ]]; then
    echo "   OK: $("$TOOLS/bin/nasm" -v)"
elif command -v nasm >/dev/null; then
    TOOLS="$(dirname "$(dirname "$(command -v nasm)")")"
    echo "   OK (시스템): $(nasm -v)"
else
    cd "$WORK"
    [[ -f nasm-$NASM_VERSION.tar.xz ]] || \
        curl -sSLO "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VERSION/nasm-$NASM_VERSION.tar.xz"
    rm -rf "nasm-$NASM_VERSION" && tar xf "nasm-$NASM_VERSION.tar.xz"
    cd "nasm-$NASM_VERSION"
    ./configure --prefix="$TOOLS" >/dev/null
    make -j"$JOBS" >/dev/null && make install >/dev/null
    echo "   설치됨: $("$TOOLS/bin/nasm" -v)"
fi

echo "== 2. dav1d $DAV1D_VERSION (static, FFmpeg 에 내장할 것) =="
# meson 은 python3.11 venv 에 설치한다 (시스템 python3.6 으로는 불가).
if [[ -e "$DAV1D_PREFIX/lib64/libdav1d.a" || -e "$DAV1D_PREFIX/lib/libdav1d.a" ]]; then
    echo "   이미 빌드됨: $DAV1D_PREFIX"
else
    command -v python3.11 >/dev/null || { echo "   python3.11 없음"; exit 1; }
    [[ -d $MESON_VENV ]] || python3.11 -m venv "$MESON_VENV"
    "$MESON_VENV/bin/pip" install -q --upgrade pip meson
    cd "$WORK"
    [[ -f dav1d-$DAV1D_VERSION.tar.gz ]] || \
        curl -sSLo "dav1d-$DAV1D_VERSION.tar.gz" \
             "https://github.com/videolan/dav1d/archive/refs/tags/$DAV1D_VERSION.tar.gz"
    rm -rf "dav1d-$DAV1D_VERSION" && tar xf "dav1d-$DAV1D_VERSION.tar.gz"
    cd "dav1d-$DAV1D_VERSION"
    # -Db_staticpic=true 가 핵심: shared 인 libavcodec.so 에 흡수시키려면 PIC 이어야 한다.
    PATH="$TOOLS/bin:$PATH" "$MESON_VENV/bin/meson" setup build \
        --prefix="$DAV1D_PREFIX" --default-library=static --buildtype=release \
        -Db_staticpic=true -Denable_tools=false -Denable_tests=false >/dev/null
    PATH="$TOOLS/bin:$PATH" "$MESON_VENV/bin/meson" compile -C build >/dev/null
    "$MESON_VENV/bin/meson" install -C build >/dev/null
    echo "   빌드됨: $DAV1D_PREFIX"
fi
DAV1D_PC="$(dirname "$(find "$DAV1D_PREFIX" -name dav1d.pc | head -1)")"

echo "== 3. FFmpeg $FFMPEG_VERSION (shared, libdav1d 내장) =="
if [[ -e "$PREFIX/lib/libavformat.so" ]]; then
    echo "   이미 설치됨: $PREFIX"
else
    cd "$WORK"
    [[ -f ffmpeg-$FFMPEG_VERSION.tar.xz ]] || \
        curl -sSLO "https://www.ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    echo "$FFMPEG_SHA256  ffmpeg-$FFMPEG_VERSION.tar.xz" | sha256sum -c -
    rm -rf "ffmpeg-$FFMPEG_VERSION" && tar xf "ffmpeg-$FFMPEG_VERSION.tar.xz"
    cd "ffmpeg-$FFMPEG_VERSION"

    # 설정은 upstream flatpak 매니페스트를 따른다. gnutls 만 제외 —
    # 우리는 네트워크 스트림을 열지 않고, gnutls-devel 은 sudo 를 요구한다.
    # 인코더/먹서를 끄면 libavcodec 이 12MB 대로 줄어든다 (png 만 남김).
    # 시스템 gcc 8.5 로 빌드한다: FFmpeg 은 순수 C 이므로 gcc-toolset 이 불필요하고,
    # 오히려 toolset 의 libstdc++ 누출 문제를 원천적으로 피한다.
    # --enable-libdav1d 없이 빌드하면 AV1 소프트웨어 디코딩이 불가하다 (파일 상단 주석 참조).
    PATH="$TOOLS/bin:$PATH" PKG_CONFIG_PATH="$DAV1D_PC" ./configure \
        --prefix="$PREFIX" \
        --enable-shared --disable-static \
        --disable-doc --disable-programs \
        --disable-encoders --disable-muxers --enable-encoder=png \
        --enable-libdav1d --pkg-config-flags=--static
    grep -q '^CONFIG_LIBDAV1D=yes' ffbuild/config.mak || {
        echo "   ERROR: libdav1d 가 활성화되지 않았다. AV1 디코딩이 안 된다."; exit 1; }
    PATH="$TOOLS/bin:$PATH" make -j"$JOBS"
    make install
    echo "   설치됨: $PREFIX"
fi

# 내장 확인: 외부 libdav1d.so 의존이 생기면 AppImage 번들 가정이 깨진다.
if readelf -d "$PREFIX/lib/libavcodec.so" | grep -q "libdav1d\.so"; then
    echo "   WARNING: libavcodec 이 외부 libdav1d.so 를 요구한다 (static 내장 실패)"
fi

echo "== 4. 배포: $DEPLOY =="
# YUView 는 lib<name>.so.<major> 형태의 정확한 파일명을 찾는다
# (FFmpegLibraryFunctions.cpp:222). soname 심볼릭이 반드시 필요하다.
# build/ 는 gitignore 대상이라 clean 빌드 후에는 이 단계를 다시 실행해야 한다.
mkdir -p "$DEPLOY"
for lib in libavutil libswresample libavcodec libavformat; do
    soname="$(basename "$(readlink -f "$PREFIX/lib/$lib.so")")"   # 예) libavutil.so.59.39.100
    major="${soname#"$lib".so.}"; major="${major%%.*}"            # 예) 59
    ln -sfn "$PREFIX/lib/$soname" "$DEPLOY/$lib.so.$major"
    printf "   %-20s -> %s\n" "$lib.so.$major" "$soname"
done

cat <<EOF

완료.

  설치 위치 : $PREFIX
  배포 위치 : $DEPLOY  (심볼릭 링크)

확인:
  YUView 실행 -> Help > About / 설정의 FFmpeg 경로, 또는 컨테이너 파일(.ivf/.mp4)을
  열어 Bitstream Analysis 탭에 OBU 트리가 나오는지 본다.

주의:
  - 패키징(AppImage/RPM) 시에는 심볼릭이 아니라 실제 파일을 번들해야 한다.
  - clean 빌드(rm -rf build) 후에는 이 스크립트를 다시 실행해 3단계를 복구할 것.
EOF
