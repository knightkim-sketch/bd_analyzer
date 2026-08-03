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

echo "== 4. FFmpeg (dlopen 대상) =="
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

echo "== 5. dav1d analyzer 디코더 (dlopen 대상) =="
cp "$APPDIR/decoder/libdav1d-internals.so" "$OUT/decoder/"

echo "== 6. qt.conf =="
# Qt 가 플러그인을 Qt 빌드 시점의 prefix 대신 실행 파일 옆에서 찾게 한다.
cat > "$OUT/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = plugins
Libraries = lib
EOF

echo "== 7. 런처 =="
# YUView 의 DT_RUNPATH 는 빌드 머신의 Qt 를 가리키지만, LD_LIBRARY_PATH 가
# DT_RUNPATH 보다 먼저 검색되므로 번들 라이브러리가 이긴다.
cat > "$OUT/YUView.sh" <<'EOF'
#!/usr/bin/env bash
# YUView 실행 래퍼. 번들된 Qt / FFmpeg 을 쓰도록 경로를 잡아준다.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$HERE/lib:$HERE/ffmpeg${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
exec "$HERE/YUView" "$@"
EOF
chmod +x "$OUT/YUView.sh"

cp "$ROOT/docs/deploy/bin-README.md" "$OUT/README.md" 2>/dev/null || true

echo
echo "완료: $OUT ($(du -sh "$OUT" | cut -f1))"
echo "실행: $OUT/YUView.sh [파일]"
