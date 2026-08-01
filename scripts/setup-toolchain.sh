#!/usr/bin/env bash
# bd_analyzer 빌드 툴체인 설치 (Rocky/RHEL 8)
#
# 근거: docs/ai/20-decisions/ADR-0002-build-and-deploy.md
#       docs/ai/40-tasks/TASK-0001-bootstrap-build.md (실측 결과)
#
#   - 시스템 gcc 8.5 불가: std::filesystem 이 -lstdc++fs 를 요구하는데 빌드 파일에 없음
#   - gcc-toolset-9 도 불가 (실측): constexpr std::find_if 가 libstdc++ GCC 10 부터.
#     YUView common/EnumMapper.h 가 이 패턴을 쓴다 -> gcc-toolset-10 이상 필수
#   - Qt 6.8+ prebuilt 불가: glibc > 2.28 요구, 우리는 정확히 2.28 -> 6.5.3 LTS 고정
#   - 시스템 pip3 는 Python 3.6 -> aqtinstall 3.0.4 에 고정됨 -> python3.11 사용
set -euo pipefail

QT_VERSION="${QT_VERSION:-6.5.3}"
QT_DIR="${QT_DIR:-$HOME/Qt}"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
VENV="${VENV:-$HOME/.venv-aqt}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

need_sudo=0

echo "== 1. gcc-toolset (>= 10) =="
if [[ -d /opt/rh/$TOOLSET ]]; then
    echo "   OK: /opt/rh/$TOOLSET"
else
    echo "   MISSING: $TOOLSET"
    echo "   -> sudo dnf install -y $TOOLSET $TOOLSET-gcc-c++"
    need_sudo=1
fi

echo "== 2. 링크 단계 의존성 =="
# Qt6Gui.prl 이 -lGL 을 요구한다. /usr/lib64/libGL.so.1 만으로는 부족하고
# devel 패키지가 제공하는 libGL.so 심볼릭이 필요하다.
if [[ -e /usr/lib64/libGL.so ]]; then
    echo "   OK: libGL.so"
else
    echo "   MISSING: libGL.so (Qt6Gui.prl 이 -lGL 요구)"
    echo "   -> sudo dnf install -y mesa-libGL-devel"
    need_sudo=1
fi

echo "== 3. GUI 실행 의존성 (xcb 플랫폼 플러그인; 헤드리스 빌드에는 불필요) =="
# Qt 공식 바이너리는 xcb-util 계열을 번들하지 않는다 (실측 확인).
declare -A XCB=(
    [libxcb-cursor.so.0]=xcb-util-cursor        # EPEL
    [libxcb-icccm.so.4]=xcb-util-wm
    [libxcb-image.so.0]=xcb-util-image
    [libxcb-keysyms.so.1]=xcb-util-keysyms
    [libxcb-render-util.so.0]=xcb-util-renderutil
)
# ldconfig -p 출력의 공백 처리는 환경마다 미묘하게 달라 오탐이 나기 쉽다.
# soname 이 캐시 어딘가에 등장하기만 하면 설치된 것으로 본다 (이 이름들은 부분일치 충돌이 없다).
LDCACHE="$(ldconfig -p 2>/dev/null)"
xcb_missing=()
for so in "${!XCB[@]}"; do
    grep -qF "$so" <<<"$LDCACHE" || xcb_missing+=("${XCB[$so]}")
done
if (( ${#xcb_missing[@]} )); then
    echo "   MISSING: ${xcb_missing[*]}"
    echo "   -> sudo dnf install -y ${xcb_missing[*]}"
    echo "      (GUI 를 띄울 때만 필요. 빌드/헤드리스 테스트는 이것 없이 진행 가능)"
else
    echo "   OK"
fi

echo "== 3b. FFmpeg 공유 라이브러리 (런타임 dlopen; 빌드 의존 아님) =="
# YUView 는 avutil/swresample/avcodec/avformat 를 런타임에 dlopen 한다.
# 없으면: 컨테이너 demux 불가, H.264 MV 통계 불가,
#         그리고 AV1 분석이 전면 불가 (ParserAV1OBU 는 ParserAVFormat 경유로만 도달).
# 별도 스크립트로 분리: 소스 빌드라 수 분 걸리고, 툴체인 설치와 수명이 다르다.
# 상세 근거 -> docs/ai/10-research/ffmpeg-integration.md
if [[ -e "$ROOT_DIR/build/YUViewApp/ffmpeg/libavformat.so.61" ]]; then
    echo "   OK: build/YUViewApp/ffmpeg/ 에 배포됨"
elif [[ -e "$HOME/opt/ffmpeg-7.1/lib/libavformat.so" ]]; then
    echo "   설치됨 ($HOME/opt/ffmpeg-7.1) 이나 배포 안 됨 -> ./scripts/setup-ffmpeg.sh"
else
    echo "   MISSING: libavformat / libavcodec / libavutil / libswresample"
    echo "   -> ./scripts/setup-ffmpeg.sh        # FFmpeg 7.1.2 shared 소스 빌드 (sudo 불필요)"
    echo "      (없어도 빌드/실행/raw YUV/H.264·HEVC AnnexB 분석은 가능. AV1 분석은 불가)"
fi

echo "== 4. aqtinstall (python3.11 venv) =="
command -v python3.11 >/dev/null || { echo "   python3.11 없음 -> sudo dnf install -y python3.11"; exit 1; }
[[ -d $VENV ]] || python3.11 -m venv "$VENV"
"$VENV/bin/pip" install -q --upgrade pip aqtinstall
echo "   OK: $($VENV/bin/aqt version 2>&1 | grep -oE 'v[0-9.]+' | head -1)"

echo "== 5. Qt $QT_VERSION =="
# 주의: qtbase 는 모듈이 아니라 기본 패키지다. '-m qtbase' 는 에러가 난다.
# 기본 설치가 YUView 요구 모듈(core gui widgets opengl xml concurrent network)을 모두 포함한다.
if [[ -x "$QT_DIR/$QT_VERSION/gcc_64/bin/qmake" ]]; then
    echo "   이미 설치됨: $QT_DIR/$QT_VERSION/gcc_64"
else
    "$VENV/bin/aqt" install-qt linux desktop "$QT_VERSION" gcc_64 --outputdir "$QT_DIR"
fi

if (( need_sudo )); then
    echo
    echo "!! 위의 sudo 항목을 먼저 처리한 뒤 ./scripts/build.sh 를 실행하세요."
    exit 1
fi

cat <<EOF

완료. 이제 ./scripts/build.sh 실행 가능.

수동으로 환경에 진입하려면:
  scl enable $TOOLSET -- bash
  export PATH=$QT_DIR/$QT_VERSION/gcc_64/bin:\$PATH
EOF
