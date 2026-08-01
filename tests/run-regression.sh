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
compile() {
    local src="$1" bin="$2"
    scl enable "$TOOLSET" -- bash -c "
        g++ -std=gnu++2a -O1 -g -fPIC \
            -I'$SRC' -I'$LIB' \
            -I'$QT/include' -I'$QT/include/QtCore' -I'$QT/include/QtGui' \
            -I'$QT/include/QtWidgets' -I'$QT/include/QtXml' -I'$QT/include/QtConcurrent' \
            -I'$QT/include/QtNetwork' -I'$QT/include/QtOpenGL' \
            '$src' -o '$bin' \
            -L'$LIB' -lYUViewLib \
            '$QT/lib/libQt6Widgets.so' '$QT/lib/libQt6OpenGL.so' '$QT/lib/libQt6Gui.so' \
            '$QT/lib/libQt6Xml.so' '$QT/lib/libQt6Concurrent.so' '$QT/lib/libQt6Network.so' \
            '$QT/lib/libQt6Core.so' \
            -lpthread -lGL -static-libstdc++ -static-libgcc"
}

# 테스트는 반드시 build/YUViewApp 에서 실행해야 한다: YUView 는 ffmpeg 를
# applicationDirPath()/ffmpeg/ 에서, 디코더를 .../decoder/ 에서 dlopen 한다.
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
else
    echo "  SKIP  03-dav1d-block-statistics (libdav1d-internals.so 없음)"
    ((skip_count++))
    run_test "$ROOT/tests/regression/05-decoder-default-and-fallback.cpp" "$STREAM" FFMpeg
fi
run_test "$ROOT/tests/regression/06-ffmpeg-library-unload.cpp"        "$STREAM"

echo
echo "== 디코더 전환 안정성 (크래시 + 행 회귀) =="
run_test "$ROOT/tests/regression/07-decoder-switch-slot.cpp"          "$STREAM"
run_test "$ROOT/tests/regression/08-decoder-switch-stress.cpp"        "$STREAM"

echo
echo "-------------------------------------------"
printf "PASS %d   FAIL %d   SKIP %d\n" "$pass_count" "$fail_count" "$skip_count"
echo "로그: $OUT"
[[ $fail_count -eq 0 ]]
