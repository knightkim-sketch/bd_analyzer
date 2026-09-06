#!/usr/bin/env bash
# 배포 패키지 생성 — RPM (설치형) 과 tar.gz (root 권한 없는 머신용).
#
# 대상: Rocky Linux 8 / RHEL 8 x86_64. 번들된 libQt6Core 가 GLIBC_2.28 을 요구하므로
# CentOS 7.x 는 대상이 아니다 (ADR-0002, docs/deploy/package-README.md).
#
# 페이로드는 기본적으로 커밋된 bin/ 번들을 그대로 쓴다 — 실제로 실행해서 검증한
# 산출물이 그것이기 때문이다. --refresh 를 주면 build/YUViewApp 에서 다시 만든다
# (이때는 Qt 와 FFmpeg prefix 가 있는 빌드 머신이어야 한다).
#
# 사용법:
#   ./scripts/package.sh                # rpm + tar.gz
#   ./scripts/package.sh rpm            # rpm 만
#   ./scripts/package.sh tar            # tar.gz 만
#   ./scripts/package.sh all --refresh  # bin/ 을 다시 만든 뒤 둘 다
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="$ROOT/bin"
WORK="${WORK:-$ROOT/build/pkg}"
DIST="${DIST:-$ROOT/build/dist}"
UPSTREAM="$ROOT/third_party/yuview/upstream"

VERSION="${BDA_VERSION:-0.2.0}"
RELEASE="${BDA_RELEASE:-1.git$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"

what="all"; refresh=0
for arg in "$@"; do
    case "$arg" in
        rpm|tar|all) what="$arg" ;;
        --refresh)   refresh=1 ;;
        *) echo "알 수 없는 인자: $arg" >&2; exit 2 ;;
    esac
done

die() { echo "ERROR: $*" >&2; exit 1; }

if (( refresh )); then
    echo "== bin/ 번들 재생성 =="
    "$ROOT/scripts/make-bin-bundle.sh"
fi

[[ -x "$BUNDLE/YUView" ]] || die "번들이 없습니다 ($BUNDLE/YUView). ./scripts/package.sh --refresh 또는 scripts/make-bin-bundle.sh 를 먼저 실행하세요."

NAME="bd-analyzer-$VERSION"
STAGE="$WORK/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE" "$DIST"

echo "== 1. 페이로드 스테이징 =="
# 번들의 실체를 그대로 옮긴다. -a 로 심볼릭 링크(SONAME 링크)와 퍼미션을 보존한다.
for item in YUView qt.conf check-deps.sh lib syslib ffmpeg plugins decoder; do
    [[ -e "$BUNDLE/$item" ]] || die "번들에 없음: $item"
    cp -a "$BUNDLE/$item" "$STAGE/"
done
# 런처는 packaging/common 이 원본이다. bin/YUView.sh 는 폴더 복사 방식 전용이라 넣지 않는다.
install -m755 "$ROOT/packaging/common/bd-analyzer" "$STAGE/bd-analyzer"
# 번들의 check-deps.sh 는 YUView.sh 를 안내한다. 패키지의 런처 이름으로 바꿔 준다.
sed -i 's|\./YUView\.sh \[파일\]|./bd-analyzer [파일]|' "$STAGE/check-deps.sh"

echo "== 2. 데스크톱 통합 파일 =="
install -Dm644 "$ROOT/packaging/common/bd-analyzer.desktop" \
               "$STAGE/share/applications/bd-analyzer.desktop"
# upstream 아이콘을 그대로 쓴다. IENT-YUView-16.png 은 실제로 32x32 라서 제외했다.
for size in 32 64 128 256 512; do
    src="$UPSTREAM/YUViewLib/images/IENT-YUView-$size.png"
    [[ -f "$src" ]] || die "아이콘 없음: $src"
    install -Dm644 "$src" "$STAGE/share/icons/$size/bd-analyzer.png"
done

echo "== 3. 라이선스 / 문서 =="
# GPLv3 §6 대비 — upstream CI 도 배포물에 동봉한다.
install -m644 "$UPSTREAM/LICENSE.GPL3" "$STAGE/LICENSE.GPL3"
install -m644 "$ROOT/docs/deploy/package-README.md" "$STAGE/README.md"

TARBALL="$DIST/$NAME-linux-x86_64.tar.gz"
echo "== 4. tar.gz =="
tar czf "$TARBALL" -C "$WORK" "$NAME"
echo "   $TARBALL ($(du -h "$TARBALL" | cut -f1))"

if [[ "$what" == "tar" ]]; then
    echo; echo "완료: $TARBALL"
    exit 0
fi

echo "== 5. rpm =="
command -v rpmbuild >/dev/null || die "rpmbuild 이 없습니다. sudo dnf install rpm-build"
TOP="$WORK/rpmbuild"
rm -rf "$TOP"
mkdir -p "$TOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
cp "$TARBALL" "$TOP/SOURCES/"
rpmbuild -bb "$ROOT/packaging/rpm/bd-analyzer.spec" \
    --define "_topdir $TOP" \
    --define "bda_version $VERSION" \
    --define "bda_release $RELEASE"

find "$TOP/RPMS" -name '*.rpm' -exec cp {} "$DIST/" \;
echo
echo "완료:"
for f in "$DIST/$NAME"*; do echo "   $f ($(du -h "$f" | cut -f1))"; done
