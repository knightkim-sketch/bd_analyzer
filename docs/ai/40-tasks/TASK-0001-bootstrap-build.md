---
title: TASK-0001 빌드 부트스트랩 (Phase 0)
status: in-progress (sudo 대기)
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: partial
---

## 목표

수정하지 않은 upstream YUView가 이 머신에서 빌드되고 실행되는 것을 확인한다.
**이게 안 되면 나머지 모든 계획이 무의미하다.**

## 단계

- [x] `git init` + 첫 커밋 → `b8e3050`, origin `git@github.com:knightkim-sketch/bd_analyzer.git` (SSH)
- [x] upstream 서브모듈 추가 및 커밋 고정 → `a72eb348` = `v2.14-301-ga72eb348`, 13MB
  ⚠️ 서브모듈의 `submodules/googletest`는 SSH URL이므로 재귀 초기화하지 말 것 (`CONFIG+=UNITTESTS` 미사용)
- [ ] **🚧 BLOCKED (sudo 필요)** `sudo dnf install -y gcc-toolset-13 gcc-toolset-13-gcc-c++`
- [ ] **🚧 BLOCKED (sudo 필요, GUI 실행 시에만)** `sudo dnf install -y libxcb-util xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm xcb-util-cursor`
- [ ] `./scripts/setup-toolchain.sh` (Qt 6.5.3 qtbase — **sudo 불필요**)
- [ ] `./scripts/build.sh`
- [ ] 실행 확인 — X11/Wayland 없으면 `QT_QPA_PLATFORM=offscreen` 으로 최소 기동만 확인
- [ ] YUV 파일 하나 열어보기 (B 기능 동작 확인)
- [ ] AV1/H.264/HEVC 스트림 하나씩 열어 Bitstream Analysis 탭 확인 (A 기능 동작 확인)

## 실측 검증 결과 (2026-07-27)

### gcc-toolset-9 로는 불가 — 실측으로 확인

이미 설치된 `gcc-toolset-9`(g++ 9.2.1)로 sudo 없이 진행 가능한지 시험했다. **실패.**

```
/tmp/cxx20test.cpp:7:48: error: call to non-'constexpr' function
  '_IIter std::find_if(_IIter, _IIter, _Predicate)'
```

- ❌ `constexpr std::find_if` — libstdc++의 constexpr `<algorithm>` (P0202R3)은 **GCC 10부터**.
  YUView `common/EnumMapper.h:44-68,100-113`가 정확히 이 패턴을 쓴다
- ✅ designated initializers — GCC 9 통과
- ✅ `std::filesystem` — GCC 9는 `-lstdc++fs` 불필요 (GCC 8만 필요)

→ **`gcc-toolset-10` 이상 필수.** ADR-0002의 13 권고 유지.
대체 컴파일러 경로도 없음: Environment Modules에 컴파일러 모듈 없음, conda/spack 없음, clang 없음, `/opt/rh`에 gcc-toolset-9 하나뿐.

### Qt 런타임 라이브러리 현황 (빌드에는 무관, GUI 실행 시 필요)

`ldconfig -p` 실측. **빌드는 `-devel` 패키지 없이 prebuilt Qt로 가능**하고, 아래는 xcb 플랫폼 플러그인이 로드될 때만 필요하다.

```
OK       libGL libxkbcommon libxkbcommon-x11 libX11 libX11-xcb libXi libxcb
         libxcb-randr/shape/sync/xfixes/xinerama/xkb libatspi libpcre2-16
         libfontconfig libfreetype libdbus-1 libglib-2.0
MISSING  libxcb-icccm.so.4  libxcb-image.so.0  libxcb-keysyms.so.1
         libxcb-render-util.so.0  libxcb-cursor.so.0
```

Qt 6 공식 바이너리는 `libxcb-*` util 계열을 자체 번들하는 경우가 많으므로 **Qt 설치 후 재확인 필요** (미검증).
`libxcb-cursor`는 Qt 6.5+에서 요구되며 번들되지 않는 편이다. GUI가 안 뜨면 `QT_DEBUG_PLUGINS=1`로 확인할 것.

## 예상 실패 지점

| 증상 | 원인 / 대처 |
|---|---|
| `undefined reference to std::filesystem::*` | gcc-toolset이 활성화 안 됨. `scl enable` 확인. 정 안 되면 `qmake LIBS+=-lstdc++fs` |
| Qt 헤더 컴파일 에러 (concepts 등) | Qt 버전이 6.5.3이 아님. `qmake -v` 확인 |
| `YUVIEW_VERSION` 이 0 | 서브모듈에 태그 없음. `git -C ... fetch --tags` |
| 실행 시 `xcb` 플랫폼 플러그인 로드 실패 | §2의 dnf 패키지 누락. `QT_DEBUG_PLUGINS=1` 로 확인 |
| `libYUViewLib.a` 를 못 찾음 | in-source 빌드했음. `build/` 지우고 out-of-tree로 재시도 |

## 완료 조건

`build/YUViewApp/YUView` 가 생성되고, YUV 파일과 AV1/AVC/HEVC 스트림을 각각 하나씩 열 수 있다.

## 다음

→ TASK-0002 AppImage 패키징 및 다른 머신에서 실행 검증
→ TASK-0003 A 기능 리스크 조사 (fork된 dav1d/libde265 internals 빌드, H.264 블록 정보 출처)
