#!/usr/bin/env bash
# bin/ 배포 번들 생성 — 같은 Rocky 8 머신에 폴더째 복사해서 실행하기 위한 파일 모음.
#
# 왜 필요한가: build/YUViewApp 은 그대로는 이식되지 않는다.
#   * YUView 의 DT_RUNPATH 가 /home/knight2/Qt/6.5.3/gcc_64/lib 로 박혀 있다 (Qt 는 동적 링크다).
#   * build/YUViewApp/ffmpeg/*.so.NN 은 ~/opt/ffmpeg-7.1 을 가리키는 심볼릭 링크다 → 다른 머신에서 깨진다.
#   * Qt 는 플랫폼 플러그인(libqxcb.so) 을 Qt 설치 prefix 에서 찾는다 → 번들 + qt.conf 가 필요하다.
# static build 가 아니므로 "추가 lib 없이 동작" 은 **번들에 넣어서** 달성한다.
# 대신 /lib64 의 시스템 라이브러리(120개, X11/GL/glibc 등)는 같은 Rocky 이면 그대로 쓴다.
#
# 번들에 넣는 파일은 실제로 실행 중 로드된 것만 골랐다 (/proc/PID/maps 로 확인).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPDIR="$ROOT/build/YUViewApp"
OUT="${OUT:-$ROOT/bin}"
QT_DIR="${QT_DIR:-$HOME/Qt}"
QT_VERSION="${QT_VERSION:-6.5.3}"
QT="$QT_DIR/$QT_VERSION/gcc_64"
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$HOME/opt/ffmpeg-7.1}"

die() { echo "ERROR: $*" >&2; exit 1; }

[[ -x "$APPDIR/YUView" ]]                     || die "YUView 가 없습니다. ./scripts/build.sh 먼저 실행."
[[ -d "$QT/lib" ]]                            || die "Qt $QT_VERSION 이 없습니다 ($QT)."
[[ -e "$APPDIR/decoder/libdav1d-internals.so" ]] || die "decoder/libdav1d-internals.so 가 없습니다."
[[ -d "$FFMPEG_PREFIX/lib" ]]                 || die "FFmpeg 이 없습니다 ($FFMPEG_PREFIX). ./scripts/setup-ffmpeg.sh 참조."

rm -rf "$OUT"
mkdir -p "$OUT"/{lib,plugins,ffmpeg,decoder}

echo "== 1. 실행 파일 =="
cp "$APPDIR/YUView" "$OUT/"

echo "== 2. Qt / ICU 공유 라이브러리 =="
# 실행 중 실제로 로드된 것들. XcbQpa/DBus/Svg 는 YUView 가 직접 링크하지 않지만
# 플랫폼 플러그인과 이미지 플러그인이 끌어온다.
QT_LIBS=(Core Gui Widgets OpenGL Xml Concurrent Network DBus Svg XcbQpa)
for name in "${QT_LIBS[@]}"; do
    real="$QT/lib/libQt6$name.so.$QT_VERSION"
    [[ -f "$real" ]] || die "없음: $real"
    cp "$real" "$OUT/lib/"
    # 바이너리는 SONAME (libQt6Core.so.6) 으로 찾는다.
    ln -sf "libQt6$name.so.$QT_VERSION" "$OUT/lib/libQt6$name.so.6"
done
for icu in icudata icui18n icuuc; do
    real="$QT/lib/lib$icu.so.56.1"
    [[ -f "$real" ]] || die "없음: $real"
    cp "$real" "$OUT/lib/"
    ln -sf "lib$icu.so.56.1" "$OUT/lib/lib$icu.so.56"
done

echo "== 3. Qt 플러그인 =="
# platforms 는 필수(없으면 "could not load the Qt platform plugin xcb" 로 죽는다).
# 나머지는 아이콘/이미지/TLS/입력기/테마용. platformthemes, platforminputcontexts 는
# 대상 머신에 GTK3/ibus 가 없으면 Qt 가 조용히 건너뛴다.
PLUGIN_DIRS=(platforms xcbglintegrations imageformats iconengines tls
             platforminputcontexts platformthemes)
for d in "${PLUGIN_DIRS[@]}"; do
    if [[ -d "$QT/plugins/$d" ]]; then
        cp -r "$QT/plugins/$d" "$OUT/plugins/"
    else
        echo "   (건너뜀: plugins/$d 없음)"
    fi
done

echo "== 4. X11 / xcb 헬퍼 라이브러리 =="
# Qt 6.5 의 xcb 플랫폼 플러그인은 X/xcb/xkb 계열 20개를 요구한다. 그중 xcb-util-*
# (cursor, wm, image, keysyms, renderutil, util) 와 libxkbcommon-x11 은 GUI 를 쓰지 않는
# Rocky 설치에는 대개 없다. 없으면 플러그인 로드가 실패하고 즉시 abort 한다:
#   "From 6.5.0, xcb-cursor0 or libxcb-cursor0 is needed to load the Qt xcb platform plugin."
# 드라이버와 무관한 프로토콜 헬퍼라 번들에 넣어도 안전하다 (libGL 계열은 드라이버와
# 결합되어 있어 넣지 않는다 - 대상에 libglvnd 가 필요하다).
#
# 목록은 하드코딩하지 않고 플러그인에서 전이적으로 닫아서 구한다.
xlib_pattern='^lib(X11|X11-xcb|Xau|Xdmcp|Xext|Xrender|xcb|xkbcommon)'
queue=("$OUT/plugins/platforms/libqxcb.so" "$OUT/lib/libQt6XcbQpa.so.$QT_VERSION")
declare -A xlibs=()
while ((${#queue[@]})); do
    f="${queue[0]}"; queue=("${queue[@]:1}")
    [[ -e "$f" ]] || continue
    while read -r name path; do
        [[ -n "${xlibs[$name]:-}" ]] && continue
        xlibs[$name]="$path"
        queue+=("$path")
    done < <(ldd "$f" 2>/dev/null |
             sed -n 's|^\s*\(lib[^ ]*\) => \(/[^ ]*\).*|\1 \2|p' | grep -E "$xlib_pattern")
done
mkdir -p "$OUT/syslib"
for name in "${!xlibs[@]}"; do
    real="$(readlink -f "${xlibs[$name]}")"
    cp -n "$real" "$OUT/syslib/"
    # 링커는 SONAME 으로 찾는다. 실체 파일명이 다르면 심볼릭 링크를 만들어 준다.
    [[ "$(basename "$real")" == "$name" ]] || ln -sf "$(basename "$real")" "$OUT/syslib/$name"
done
echo "   ${#xlibs[@]}개 복사"

echo "== 5. FFmpeg (dlopen 대상) =="
# YUView 는 이 네 개를 applicationDirPath()/ffmpeg/ 에서 QLibrary 로 직접 로드한다.
# build/YUViewApp/ffmpeg 는 심볼릭 링크라 이식되지 않으므로 실체를 복사한다.
declare -A FFMPEG_SO=( [libavutil]=59 [libswresample]=5 [libavcodec]=61 [libavformat]=61 )
for base in "${!FFMPEG_SO[@]}"; do
    soname="$base.so.${FFMPEG_SO[$base]}"
    real="$(readlink -f "$FFMPEG_PREFIX/lib/$soname")"
    [[ -f "$real" ]] || die "없음: $FFMPEG_PREFIX/lib/$soname"
    cp "$real" "$OUT/ffmpeg/"
    ln -sf "$(basename "$real")" "$OUT/ffmpeg/$soname"
done

echo "== 6. dav1d analyzer 디코더 (dlopen 대상) =="
cp "$APPDIR/decoder/libdav1d-internals.so" "$OUT/decoder/"

echo "== 7. qt.conf =="
# Qt 가 플러그인을 Qt 빌드 시점의 prefix 대신 실행 파일 옆에서 찾게 한다.
cat > "$OUT/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = plugins
Libraries = lib
EOF

echo "== 8. 런처 / 진단 스크립트 =="
# YUView 의 DT_RUNPATH 는 빌드 머신의 Qt 를 가리키지만, LD_LIBRARY_PATH 가
# DT_RUNPATH 보다 먼저 검색되므로 번들 라이브러리가 이긴다.
cat > "$OUT/YUView.sh" <<'EOF'
#!/usr/bin/env bash
# YUView 실행 래퍼. 번들된 Qt / FFmpeg / X11 헬퍼를 쓰도록 경로를 잡아준다.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# LD_LIBRARY_PATH 에 들어간 것은 /lib64 보다 먼저 검색되므로, syslib 의 X11/xcb
# 라이브러리는 대상 머신에 같은 것이 있어도 번들 쪽이 쓰인다. 같은 Rocky 8 계열이면
# ABI 가 같아 문제되지 않고, 없는 머신에서도 그대로 뜨는 쪽을 택했다.
export LD_LIBRARY_PATH="$HERE/lib:$HERE/ffmpeg:$HERE/syslib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
exec "$HERE/YUView" "$@"
EOF
chmod +x "$OUT/YUView.sh"

# 대상 머신에서 무엇이 없는지 한 번에 알려주는 진단 스크립트.
cat > "$OUT/check-deps.sh" <<'EOF'
#!/usr/bin/env bash
# 이 번들이 현재 머신에서 실행 가능한지 점검한다. 해결되지 않는 라이브러리를 모두 보고한다.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$HERE/lib:$HERE/ffmpeg:$HERE/syslib"

problems=0
# ldd 는 "경로가 없는" 라이브러리만 잡는다. 파일이 있지만 손상/버전 불일치인 경우까지
# 잡으려면 실제로 dlopen 해 봐야 한다. python3 는 Rocky 8 기본 설치에 있다.
have_python=0
command -v python3 >/dev/null 2>&1 && have_python=1

check() {
    local f="$1" label="$2" out
    [[ -e "$f" ]] || { echo "  없음: $label ($f)"; problems=$((problems+1)); return; }

    out="$(ldd "$f" 2>/dev/null | grep 'not found')"
    if [[ -n "$out" ]]; then
        echo "  $label - 해결 안 되는 의존성:"
        echo "$out" | sed 's/^[[:space:]]*/    /'
        problems=$((problems+$(echo "$out" | wc -l)))
        return
    fi

    # 실행 파일은 dlopen 할 수 없다 ("cannot dynamically load executable"). 공유 라이브러리만.
    if [[ $have_python == 1 && "$f" == *.so* ]]; then
        out="$(python3 -c "
import ctypes, sys
try:
    ctypes.CDLL('$f')
except OSError as e:
    print(e)
    sys.exit(1)
" 2>&1)" || { echo "  $label - 로드 실패:"; echo "$out" | sed 's/^/    /'
                problems=$((problems+1)); }
    fi
}

echo "== 해결되지 않는 라이브러리 =="
check "$HERE/YUView"                            "YUView"
check "$HERE/plugins/platforms/libqxcb.so"      "Qt xcb 플랫폼 플러그인"
for f in "$HERE"/ffmpeg/lib*.so.*[0-9]; do
    [[ -L "$f" ]] || check "$f" "ffmpeg/$(basename "$f")"
done
check "$HERE/decoder/libdav1d-internals.so"     "dav1d analyzer 디코더"
[[ $problems -eq 0 ]] && echo "  (없음)"

echo
echo "== 실행 환경 =="
[[ -n "${DISPLAY:-}" ]] && echo "  DISPLAY=$DISPLAY" || echo "  DISPLAY 가 설정되지 않았습니다 (GUI 실행 불가)"
[[ -d /usr/share/X11/xkb ]] && echo "  xkb 데이터 있음" \
    || echo "  /usr/share/X11/xkb 없음 → xkeyboard-config 설치 필요 (키보드 초기화 실패)"

echo
if [[ $problems -eq 0 ]]; then
    echo "결과: 실행 가능. ./YUView.sh [파일]"
else
    echo "결과: 문제 $problems 건. README.md 의 dnf 목록을 참고하세요."
    exit 1
fi
EOF
chmod +x "$OUT/check-deps.sh"

cp "$ROOT/docs/deploy/bin-README.md" "$OUT/README.md" 2>/dev/null || true

echo
echo "완료: $OUT ($(du -sh "$OUT" | cut -f1))"
echo "실행: $OUT/YUView.sh [파일]"
