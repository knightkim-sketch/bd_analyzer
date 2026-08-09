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
