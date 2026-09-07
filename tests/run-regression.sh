#!/usr/bin/env bash
# bd_analyzer 회귀 테스트 — AV1 분석 경로와 디코더 전환 안정성
#
# 여기 있는 테스트는 전부 **실제로 발생했던 크래시/행/오작동**에서 나왔다.
# 각 테스트 파일 상단 주석에 어떤 버그를 막는지 적혀 있다. → docs/ai/40-tasks/TASK-0003
#
# 테스트는 GUI 없이 upstream YUView 의 프로덕션 클래스를 직접 구동한다
# (offscreen Qt 플랫폼). libYUViewLib.a 를 링크하므로 ./scripts/build.sh 가 먼저 돌아야 한다.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/third_party/yuview/upstream/YUViewLib/src"
LIB="$ROOT/build/YUViewLib"
APPDIR="$ROOT/build/YUViewApp"
QT_DIR="${QT_DIR:-$HOME/Qt}"
QT_VERSION="${QT_VERSION:-6.5.3}"
QT="$QT_DIR/$QT_VERSION/gcc_64"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
OUT="${OUT:-$ROOT/build/tests}"
DATA="${DATA:-$ROOT/tests/data}"
# 대부분의 테스트는 몇 초면 끝난다. 넉넉히 주되, 행을 반드시 잡아야 한다
# (수정 전 디코더 전환 버그는 크래시가 아니라 무한 대기로도 나타났다).
TIMEOUT="${TIMEOUT:-120}"

fail_count=0
pass_count=0
skip_count=0

die() { echo "ERROR: $*" >&2; exit 2; }

[[ -f "$LIB/libYUViewLib.a" ]] || die "libYUViewLib.a 없음. 먼저 ./scripts/build.sh 를 실행하세요."
[[ -x "$QT/bin/qmake" ]]       || die "Qt $QT_VERSION 없음 ($QT). ./scripts/setup-toolchain.sh 참조."
[[ -d /opt/rh/$TOOLSET ]]      || die "$TOOLSET 없음. 시스템 gcc 8.5 로는 이 코드가 빌드되지 않습니다."
[[ -e "$APPDIR/ffmpeg/libavformat.so.61" ]] || \
    die "FFmpeg 라이브러리가 배포되지 않았습니다. ./scripts/setup-ffmpeg.sh 를 실행하세요."

mkdir -p "$OUT" "$DATA"

# ---------------------------------------------------------------- 테스트 데이터
# 리포지토리에 바이너리를 넣지 않는다. 없으면 ffmpeg 로 생성한다.
# testsrc2 는 프레임마다 타임코드가 찍혀서 디코딩 결과 정합성을 눈으로도 확인할 수 있다.
ensure_stream() {
    local out="$DATA/test.ivf"
    if [[ -s "$out" ]]; then echo "$out"; return 0; fi
    local ff
    ff="$(command -v ffmpeg || echo /usr/local/bin/ffmpeg)"
    [[ -x "$ff" ]] || return 1
    "$ff" -hide_banner -loglevel error -y \
          -f lavfi -i "testsrc2=size=176x144:rate=25:duration=1" \
          -c:v libaom-av1 -cpu-used 8 -g 10 -pix_fmt yuv420p "$out" >/dev/null 2>&1 || return 1
    [[ -s "$out" ]]
    echo "$out"
}

STREAM="$(ensure_stream)" || {
    echo "SKIP ALL: test.ivf 이 없고 ffmpeg 로 생성할 수도 없습니다 (libaom-av1 필요)."
    exit 0
}
echo "테스트 스트림: $STREAM"
echo

# ------------------------------------------------------------------- 빌드/실행
# src/me 는 Qt-free 다. 단위 테스트도 Qt / libYUViewLib 없이 빌드해서 그 사실을 실제로 검증한다
# (Qt 헤더가 하나라도 섞여 들어오면 여기서 컴파일이 깨진다).
compile_unit() {
    local src="$1" bin="$2"; shift 2
    scl enable "$TOOLSET" -- bash -c "
        g++ -std=gnu++2a -O1 -g -Wall -Wextra -I'$ROOT/src' \
            '$src' $* -o '$bin'"
}

run_unit_test() {
    local src="$1"; shift
    local name log bin rc
    name="$(basename "$src" .cpp)"
    bin="$OUT/$name"
    log="$OUT/$name.log"

    if ! compile_unit "$src" "$bin" "$@" > "$log" 2>&1; then
        echo "  FAIL  $name (컴파일 실패, $log)"
        ((fail_count++)); return
    fi
    if timeout "$TIMEOUT" "$bin" > "$log" 2>&1; then
        echo "  PASS  $name"; ((pass_count++))
    else
        rc=$?
        echo "  FAIL  $name (종료 코드 $rc, $log)"
        ((fail_count++))
    fi
}

# ME 통합 테스트용: 우리 src/me + src/integration 을 함께 컴파일한다. 앱 빌드에서는 .pro 가
# 같은 파일들을 가져가지만(패치 0026), 테스트는 libYUViewLib.a 에 그것들이 들어있다고 가정하지
# 않는다 - 그러면 .pro 배선이 깨져도 테스트가 통과해 버린다.
BDA_ME_SRCS="$ROOT/src/me/MePlane.cpp $ROOT/src/me/MeCost.cpp $ROOT/src/me/SvtIntegerMe.cpp \
             $ROOT/src/me/OdysseyOpenLoopMe.cpp $ROOT/src/me/MeEstimatorFactory.cpp \
             $ROOT/src/integration/MeStatisticsAdapter.cpp $ROOT/src/integration/MeFrameSource.cpp \
             $ROOT/src/integration/MeRunner.cpp"

compile() {
    local src="$1" bin="$2"; shift 2
    scl enable "$TOOLSET" -- bash -c "
        g++ -std=gnu++2a -O1 -g -fPIC \
            -I'$SRC' -I'$LIB' -I'$ROOT/src' \
            -I'$QT/include' -I'$QT/include/QtCore' -I'$QT/include/QtGui' \
            -I'$QT/include/QtWidgets' -I'$QT/include/QtXml' -I'$QT/include/QtConcurrent' \
            -I'$QT/include/QtNetwork' -I'$QT/include/QtOpenGL' \
            '$src' $* -o '$bin' \
            -L'$LIB' -lYUViewLib \
            '$QT/lib/libQt6Widgets.so' '$QT/lib/libQt6OpenGL.so' '$QT/lib/libQt6Gui.so' \
            '$QT/lib/libQt6Xml.so' '$QT/lib/libQt6Concurrent.so' '$QT/lib/libQt6Network.so' \
            '$QT/lib/libQt6Core.so' \
            -lpthread -lGL -static-libstdc++ -static-libgcc"
}

# 테스트는 반드시 build/YUViewApp 에서 실행해야 한다: YUView 는 ffmpeg 를
# applicationDirPath()/ffmpeg/ 에서, 디코더를 .../decoder/ 에서 dlopen 한다.
# run_test 와 같지만 우리 src/me + src/integration 을 함께 컴파일한다.
run_test_bda() {
    local src="$1"; shift
    local name log bin rc
    name="$(basename "$src" .cpp)"
    bin="$OUT/$name"
    log="$OUT/$name.log"

    if ! compile "$src" "$bin" $BDA_ME_SRCS > "$log" 2>&1; then
        echo "  FAIL  $name (컴파일 실패, $log)"
        ((fail_count++)); return
    fi

    ( cd "$APPDIR" && QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH="$QT/lib" \
        timeout "$TIMEOUT" "$bin" "$@" ) >> "$log" 2>&1
    rc=$?

    if   [[ $rc -eq 0   ]]; then echo "  PASS  $name"; ((pass_count++))
    elif [[ $rc -eq 124 ]]; then echo "  FAIL  $name (${TIMEOUT}s 타임아웃 = 행. $log)"; ((fail_count++))
    elif [[ $rc -gt 128 ]]; then echo "  FAIL  $name (시그널 $((rc-128)) = 크래시. $log)"; ((fail_count++))
    else                         echo "  FAIL  $name (exit $rc, $log)"; ((fail_count++))
    fi
}

run_test() {
    local src="$1"; shift
    local name log bin rc
    name="$(basename "$src" .cpp)"
    bin="$OUT/$name"
    log="$OUT/$name.log"

    if ! compile "$src" "$bin" > "$log" 2>&1; then
        echo "  FAIL  $name (컴파일 실패, $log)"
        ((fail_count++)); return
    fi

    ( cd "$APPDIR" && QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH="$QT/lib" \
        timeout "$TIMEOUT" "$bin" "$@" ) >> "$log" 2>&1
    rc=$?

    if   [[ $rc -eq 0   ]]; then echo "  PASS  $name"; ((pass_count++))
    elif [[ $rc -eq 124 ]]; then echo "  FAIL  $name (${TIMEOUT}s 타임아웃 = 행. $log)"; ((fail_count++))
    elif [[ $rc -gt 128 ]]; then echo "  FAIL  $name (시그널 $((rc-128)) = 크래시. $log)"; ((fail_count++))
    else                         echo "  FAIL  $name (exit $rc, $log)"; ((fail_count++))
    fi
}

echo "== ME 코어 단위 테스트 (Qt 없이 빌드) =="
run_unit_test "$ROOT/tests/unit/me-plane-and-cost.cpp" \
              "$ROOT/src/me/MePlane.cpp" "$ROOT/src/me/MeCost.cpp"
run_unit_test "$ROOT/tests/unit/me-svt-integer.cpp" \
              "$ROOT/src/me/MePlane.cpp" "$ROOT/src/me/MeCost.cpp" "$ROOT/src/me/SvtIntegerMe.cpp"
run_unit_test "$ROOT/tests/unit/me-odyssey-openloop.cpp" \
              "$ROOT/src/me/MePlane.cpp" "$ROOT/src/me/MeCost.cpp" "$ROOT/src/me/OdysseyOpenLoopMe.cpp"

# ME 결과가 upstream 통계 오버레이(기존 MV drawer)로 실제로 들어가는지. 별도 drawer 를 만들지
# 않기로 한 결정이 성립하는지를 여기서 확인한다.
run_test_bda "$ROOT/tests/regression/22-me-statistics-overlay.cpp"

# raw YUV 프레임 두 장 -> ME 결과. ME 패널이 워커 스레드에서 부르는 경로 전체.
run_test_bda "$ROOT/tests/regression/23-me-runner.cpp"
echo

echo "== AV1 분석 경로 =="
run_test "$ROOT/tests/regression/01-av1-obu-parsing.cpp"              "$STREAM"
run_test "$ROOT/tests/regression/02-obu-packet-split.cpp"             "$STREAM"
run_test "$ROOT/tests/regression/04-ffmpeg-av1-decode.cpp"            "$STREAM"

echo
echo "== 디코더 선택 / 라이브러리 수명 =="
# libdav1d 가 있으면 Dav1d 가, 없으면 FFMpeg 으로 폴백되어야 한다.
if [[ -e "$APPDIR/decoder/libdav1d-internals.so" ]]; then
    run_test "$ROOT/tests/regression/05-decoder-default-and-fallback.cpp" "$STREAM" Dav1d
    run_test "$ROOT/tests/regression/03-dav1d-block-statistics.cpp"       "$STREAM"
    run_test "$ROOT/tests/regression/09-block-info-query.cpp"             "$STREAM"
    run_test "$ROOT/tests/regression/10-superblock-grid-size.cpp"          "$STREAM"
    run_test "$ROOT/tests/regression/11-block-partition-tiling.cpp"        "$STREAM"
else
    echo "  SKIP  03-dav1d-block-statistics (libdav1d-internals.so 없음)"
    ((skip_count++))
    echo "  SKIP  10-superblock-grid-size (libdav1d-internals.so 없음)"
    ((skip_count++))
    echo "  SKIP  11-block-partition-tiling (libdav1d-internals.so 없음)"
    ((skip_count++))
    run_test "$ROOT/tests/regression/05-decoder-default-and-fallback.cpp" "$STREAM" FFMpeg
fi
run_test "$ROOT/tests/regression/06-ffmpeg-library-unload.cpp"        "$STREAM"

# compressed stream 위의 ME: org YUV/Y4M 첨부, 디코더 컨테이너와의 분리, 디코드 루프 없음,
# 스트림 MV 와 재현 MV 가 한 pane 에. org 파일은 테스트가 직접 만든다.
run_test_bda "$ROOT/tests/regression/29-me-on-compressed-stream.cpp"  "$STREAM"

echo
echo "== raw YUV 픽셀 분석 (블록 통계 / 캐시 / 히스토그램 / convolution) =="
# 이 테스트는 raw YUV 파일을 쓴다. 없으면 ffmpeg 로 만든다.
RAWYUV="$DATA/test_176x144_yuv420p.yuv"
if [[ ! -s "$RAWYUV" ]]; then
    ff="$(command -v ffmpeg || echo /usr/local/bin/ffmpeg)"
    [[ -x "$ff" ]] && "$ff" -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=176x144:rate=25:duration=1" \
        -pix_fmt yuv420p "$RAWYUV" >/dev/null 2>&1
fi
if [[ -s "$RAWYUV" ]]; then
    run_test "$ROOT/tests/regression/12-raw-yuv-pixel-analysis.cpp"    "$RAWYUV" "$STREAM"
    # original YUV 를 붙였을 때의 64x64 블록 SSE (Load Org YUV 버튼이 하는 일).
    run_test "$ROOT/tests/regression/18-org-yuv-block-sse.cpp"         "$RAWYUV"
    # playlist 가 정상 종료를 넘어 유지되는지 + File 메뉴 on/off 스위치.
    run_test "$ROOT/tests/regression/21-playlist-saved-across-sessions.cpp" "$RAWYUV"
    # ME 패널과 그것이 구동하는 raw YUV 아이템 (통계 컨테이너, 임의 프레임 읽기, 백그라운드 실행).
    run_test_bda "$ROOT/tests/regression/24-me-panel.cpp"                   "$RAWYUV"
else
    echo "  SKIP  12-raw-yuv-pixel-analysis (raw YUV 생성 실패)"
    ((skip_count++))
    echo "  SKIP  18-org-yuv-block-sse (raw YUV 생성 실패)"
    ((skip_count++))
    echo "  SKIP  21-playlist-saved-across-sessions (raw YUV 생성 실패)"
    ((skip_count++))
    echo "  SKIP  24-me-panel (raw YUV 생성 실패)"
    ((skip_count++))
fi
# 이름에 해상도가 없고 크기로도 포맷을 추측할 수 없는 raw YUV. 열자마자 bad_alloc 으로
# abort 하던 회귀 (videoHandlerYUV::getFormatAsString 의 빈 optional 역참조).
# 테스트가 자기 파일을 만들므로 RAWYUV 유무와 무관하다.
run_test "$ROOT/tests/regression/20-raw-yuv-unknown-format.cpp"

# 같은 파일이 playlist 에 두 번 담기지 않는지 (앱 시작 시 로드 + 종료 시 스냅샷).
# 자기 클립을 만들므로 RAWYUV 유무와 무관하다.
run_test "$ROOT/tests/regression/28-playlist-no-duplicate-files.cpp"

# 히스토그램/블록 통계가 표시 중인 프레임을 따라가는지 (전체 창을 띄워서 검사한다).
run_test "$ROOT/tests/regression/17-frame-info-follows-frame.cpp"      "$STREAM"

# ME 오버레이: item 을 바꿔도 abort 하지 않는지, MV 선이 실제로 그려지는지, 통계 행이 종류별로
# 나뉘는지. 테스트가 자기 클립을 만들므로 RAWYUV 유무와 무관하다.
run_test_bda "$ROOT/tests/regression/26-me-overlay-item-switch.cpp"

# raw item 위의 블록을 클릭했을 때 그 블록의 재현 MV 가 Block Info 패널에 뜨는지.
# 자기 클립을 만들므로 RAWYUV 유무와 무관하다.
run_test_bda "$ROOT/tests/regression/27-me-block-info-on-click.cpp"

# 디코딩 캐시가 실행한 디렉토리에 생기는지 (바이너리 옆이 아니라). 자식 프로세스를 띄운다.
run_test "$ROOT/tests/regression/30-cache-in-working-directory.cpp"

# 창을 실제로 파괴한다. 종료 시 "free(): invalid pointer" 로 abort 하던 회귀 + 모든 dock 이
# View 메뉴에서 다시 켜지는지. 다른 MainWindow 테스트는 창을 일부러 leak 하므로 여기서만 잡힌다.
run_test "$ROOT/tests/regression/25-mainwindow-teardown.cpp"

echo
echo "== 프레임 비트스트림 덤프 (hexdump 패널) =="
# annexB 쪽 검사는 B 프레임이 있는 스트림이 필요하다. display order 와 coding order 가
# 갈리지 않으면 인덱스 순서 회귀를 잡을 수 없다.
H264="$DATA/test_bframes.h264"
if [[ ! -s "$H264" ]]; then
    ff="$(command -v ffmpeg || echo /usr/local/bin/ffmpeg)"
    [[ -x "$ff" ]] && "$ff" -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=176x144:rate=25:duration=1" \
        -c:v libx264 -bf 3 -g 10 -pix_fmt yuv420p -f h264 "$H264" >/dev/null 2>&1
fi
if [[ -s "$H264" ]]; then
    run_test "$ROOT/tests/regression/13-frame-bitstream-dump.cpp"       "$STREAM" "$H264"
else
    echo "  (annexB 스트림 생성 실패 - AV1 경로만 검사한다)"
    run_test "$ROOT/tests/regression/13-frame-bitstream-dump.cpp"       "$STREAM"
fi

# 블록 단위 비트 위치는 dav1d analyzer 라이브러리에서만 나온다.
if [[ -e "$APPDIR/decoder/libdav1d-internals.so" ]]; then
    run_test "$ROOT/tests/regression/14-block-bitstream-range.cpp"      "$STREAM"
    run_test "$ROOT/tests/regression/16-statistics-ui-grouping.cpp"     "$STREAM"
    # 블록 syntax pane 의 프레임 추종 + Rec/Org 뷰어 전환 + SSE/bitcount 그래프.
    # test.ivf 와 raw YUV 는 같은 testsrc2 소스라서 raw YUV 를 original 로 붙일 수 있다.
    if [[ -s "$RAWYUV" ]]; then
        run_test "$ROOT/tests/regression/19-block-syntax-and-rd-plot.cpp" "$STREAM" "$RAWYUV"
    else
        echo "  SKIP  19-block-syntax-and-rd-plot (raw YUV 없음)"
        ((skip_count++))
    fi
else
    echo "  SKIP  14-block-bitstream-range (libdav1d-internals.so 없음)"
    ((skip_count++))
    echo "  SKIP  16-statistics-ui-grouping (libdav1d-internals.so 없음)"
    ((skip_count++))
fi

# 컨테이너 없는 raw AV1 (.av1 / .obu) 도 .ivf 와 동일하게 열려야 한다.
RAWAV1="$DATA/test_raw.av1"
if [[ ! -s "$RAWAV1" ]]; then
    ff="$(command -v ffmpeg || echo /usr/local/bin/ffmpeg)"
    [[ -x "$ff" ]] && "$ff" -hide_banner -loglevel error -y -i "$STREAM" \
        -c:v copy -f obu "$RAWAV1" >/dev/null 2>&1
fi
if [[ -s "$RAWAV1" ]]; then
    run_test "$ROOT/tests/regression/15-raw-av1-extension.cpp"           "$STREAM" "$RAWAV1"
else
    echo "  SKIP  15-raw-av1-extension (raw AV1 생성 실패)"
    ((skip_count++))
fi

echo
echo "== 디코더 전환 안정성 (크래시 + 행 회귀) =="
run_test "$ROOT/tests/regression/07-decoder-switch-slot.cpp"          "$STREAM"
run_test "$ROOT/tests/regression/08-decoder-switch-stress.cpp"        "$STREAM"

echo
echo "-------------------------------------------"
printf "PASS %d   FAIL %d   SKIP %d\n" "$pass_count" "$fail_count" "$skip_count"
echo "로그: $OUT"
[[ $fail_count -eq 0 ]]
