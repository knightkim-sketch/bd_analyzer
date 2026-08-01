#!/usr/bin/env bash
# bd_analyzer out-of-tree 빌드
#
# Phase 0: upstream YUView 를 qmake 로 그대로 빌드 (검증된 경로)
# Phase 1: CMake 이식 후 이 스크립트를 교체 -> ADR-0002
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM="$ROOT/third_party/yuview/upstream"
BUILD="${BUILD:-$ROOT/build}"
QT_VERSION="${QT_VERSION:-6.5.3}"
QT_DIR="${QT_DIR:-$HOME/Qt}"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
JOBS="${JOBS:-$(nproc)}"

[[ -f "$UPSTREAM/YUView.pro" ]] || {
    echo "upstream 이 없습니다. 먼저:"
    echo "  git submodule add https://github.com/IENT/YUView.git third_party/yuview/upstream"
    echo "  git -C third_party/yuview/upstream checkout a72eb3488097313511e60ed70db4af6071cbe9fe"
    exit 1
}

# upstream 패치 적용 (idempotent: 이미 적용된 것은 건너뜀)
shopt -s nullglob
for p in "$ROOT"/third_party/yuview/patches/*.patch; do
    if git -C "$UPSTREAM" apply --check "$p" 2>/dev/null; then
        echo "patch: $(basename "$p")"
        git -C "$UPSTREAM" apply "$p"
    fi
done
shopt -u nullglob

# out-of-tree 필수: .qmake.conf 가 top_builddir=$$shadowed($$PWD),
# YUViewApp.pro 가 PRE_TARGETDEPS += $$top_builddir/YUViewLib/libYUViewLib.a 를 하드코딩
mkdir -p "$BUILD"

# UNITTESTS 는 생략: googletest 서브모듈이 SSH URL 이고 미초기화
exec scl enable "$TOOLSET" -- bash -c "
    set -euo pipefail
    export PATH='$QT_DIR/$QT_VERSION/gcc_64/bin:\$PATH'
    cd '$BUILD'
    # -r (recursive) is required, not optional: YUViewLib.pro globs its sources with
    # \$\$files(src/*.cpp, true), and that glob is only re-evaluated when the sub-project Makefile is
    # regenerated. Without -r, make leaves YUViewLib/Makefile alone (the .pro did not change) and
    # newly added source files are silently never compiled - it fails at link time instead.
    qmake -r \
        QMAKE_LFLAGS+='-static-libstdc++ -static-libgcc' \
        '$UPSTREAM/YUView.pro'
    make -j$JOBS
    echo
    echo '빌드 완료: $BUILD/YUViewApp/YUView'
"
