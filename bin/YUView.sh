#!/usr/bin/env bash
# YUView 실행 래퍼. 번들된 Qt / FFmpeg / X11 헬퍼를 쓰도록 경로를 잡아준다.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# LD_LIBRARY_PATH 에 들어간 것은 /lib64 보다 먼저 검색되므로, syslib 의 X11/xcb
# 라이브러리는 대상 머신에 같은 것이 있어도 번들 쪽이 쓰인다. 같은 Rocky 8 계열이면
# ABI 가 같아 문제되지 않고, 없는 머신에서도 그대로 뜨는 쪽을 택했다.
export LD_LIBRARY_PATH="$HERE/lib:$HERE/ffmpeg:$HERE/syslib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
exec "$HERE/YUView" "$@"
