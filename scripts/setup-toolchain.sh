#!/usr/bin/env bash
# bd_analyzer 빌드 툴체인 설치 (Rocky/RHEL 8)
#
# 근거: docs/ai/20-decisions/ADR-0002-build-and-deploy.md
#   - 시스템 gcc 8.5 불가 (std::filesystem -lstdc++fs 누락 + Qt 6.3+ 는 gcc 9+)
#   - Qt 6.8+ prebuilt 불가 (glibc > 2.28 요구, 우리는 정확히 2.28) -> 6.5.3 LTS 고정
#   - 시스템 pip3 는 Python 3.6 -> aqtinstall 3.0.4 에 고정됨 -> python3.11 사용
set -euo pipefail

QT_VERSION="${QT_VERSION:-6.5.3}"
QT_DIR="${QT_DIR:-$HOME/Qt}"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
VENV="${VENV:-$HOME/.venv-aqt}"

echo "== 1. gcc-toolset =="
if [[ ! -d /opt/rh/$TOOLSET ]]; then
    echo "   $TOOLSET 설치 필요 (sudo):"
    echo "   sudo dnf install -y $TOOLSET $TOOLSET-gcc-c++"
    exit 1
fi
echo "   OK: /opt/rh/$TOOLSET"

echo "== 2. Qt xcb 플랫폼 플러그인 런타임 의존성 =="
MISSING=()
for pkg in mesa-libGL-devel libxkbcommon-x11-devel libxkbcommon-devel \
           libX11-devel libXi-devel at-spi2-atk-devel fuse-libs; do
    rpm -q "$pkg" &>/dev/null || MISSING+=("$pkg")
done
if (( ${#MISSING[@]} )); then
    echo "   설치 필요 (sudo): sudo dnf install -y ${MISSING[*]}"
    exit 1
fi
echo "   OK"

echo "== 3. aqtinstall (python3.11 venv) =="
command -v python3.11 >/dev/null || { echo "   python3.11 없음: sudo dnf install -y python3.11"; exit 1; }
[[ -d $VENV ]] || python3.11 -m venv "$VENV"
"$VENV/bin/pip" install -q --upgrade pip aqtinstall
echo "   OK: $($VENV/bin/aqt version 2>&1 | head -1)"

echo "== 4. Qt $QT_VERSION (qtbase only) =="
# YUView 가 요구하는 모듈 core/gui/widgets/opengl/xml/concurrent/network 는 전부 qtbase 안에 있다.
if [[ -x "$QT_DIR/$QT_VERSION/gcc_64/bin/qmake" ]]; then
    echo "   이미 설치됨: $QT_DIR/$QT_VERSION/gcc_64"
else
    "$VENV/bin/aqt" install-qt linux desktop "$QT_VERSION" gcc_64 -m qtbase --outputdir "$QT_DIR"
fi

cat <<EOF

완료. 빌드 환경 진입:

  scl enable $TOOLSET -- bash
  export PATH=$QT_DIR/$QT_VERSION/gcc_64/bin:\$PATH

또는 그냥 ./scripts/build.sh 실행 (내부에서 처리).
EOF
