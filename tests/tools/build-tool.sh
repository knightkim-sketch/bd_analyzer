#!/usr/bin/env bash
# Build one of the tools in tests/tools/ against the already-built libYUViewLib.a.
#
# Same toolchain and include set as tests/run-regression.sh - these tools drive the same
# production classes, and mixing system gcc 8.5 with gcc-toolset-13 links but then segfaults.
#
#   ./tests/tools/build-tool.sh dump-obu-headers
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/third_party/yuview/upstream/YUViewLib/src"
LIB="$ROOT/build/YUViewLib"
QT_DIR="${QT_DIR:-$HOME/Qt}"
QT_VERSION="${QT_VERSION:-6.5.3}"
QT="$QT_DIR/$QT_VERSION/gcc_64"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
OUT="${OUT:-$ROOT/build/tools}"

# Tools live in two places: tests/tools/ for things that inspect a parse, tools/cli/ for headless
# drivers of the GUI's own features. Both build against libYUViewLib.a the same way.
name="${1:?usage: build-tool.sh <name without .cpp>}"
src=""
for d in "$ROOT/tests/tools" "$ROOT/tools/cli"; do
    [[ -f "$d/$name.cpp" ]] && { src="$d/$name.cpp"; break; }
done
[[ -n "$src" ]] || { echo "no such tool: $name.cpp (looked in tests/tools/ and tools/cli/)" >&2; exit 2; }
[[ -f "$LIB/libYUViewLib.a" ]] || { echo "run ./scripts/build.sh first" >&2; exit 2; }

mkdir -p "$OUT"
scl enable "$TOOLSET" -- bash -c "
    g++ -std=gnu++2a -O1 -g -fPIC \
        -I'$SRC' -I'$LIB' -I'$ROOT/src' \
        -I'$QT/include' -I'$QT/include/QtCore' -I'$QT/include/QtGui' \
        -I'$QT/include/QtWidgets' -I'$QT/include/QtXml' -I'$QT/include/QtConcurrent' \
        -I'$QT/include/QtNetwork' -I'$QT/include/QtOpenGL' \
        '$src' -o '$OUT/$name' \
        -L'$LIB' -lYUViewLib \
        '$QT/lib/libQt6Widgets.so' '$QT/lib/libQt6OpenGL.so' '$QT/lib/libQt6Gui.so' \
        '$QT/lib/libQt6Xml.so' '$QT/lib/libQt6Concurrent.so' '$QT/lib/libQt6Network.so' \
        '$QT/lib/libQt6Core.so' \
        -lpthread -lGL -static-libstdc++ -static-libgcc"

echo "built $OUT/$name"
echo "run it from build/YUViewApp - YUView dlopens ffmpeg from applicationDirPath()/ffmpeg/"
