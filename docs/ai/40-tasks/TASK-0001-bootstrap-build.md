---
title: TASK-0001 빌드 부트스트랩 (Phase 0)
status: todo
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: no
---

## 목표

수정하지 않은 upstream YUView가 이 머신에서 빌드되고 실행되는 것을 확인한다.
**이게 안 되면 나머지 모든 계획이 무의미하다.**

## 단계

- [ ] `git init` + 첫 커밋 (현재 디렉토리 구조 + 문서)
- [ ] upstream 서브모듈 추가 및 커밋 고정
  ```bash
  git submodule add https://github.com/IENT/YUView.git third_party/yuview/upstream
  git -C third_party/yuview/upstream checkout a72eb3488097313511e60ed70db4af6071cbe9fe
  ```
  ⚠️ 서브모듈의 `submodules/googletest`는 SSH URL이므로 재귀 초기화하지 말 것 (`CONFIG+=UNITTESTS` 미사용)
- [ ] `sudo dnf install -y gcc-toolset-13 gcc-toolset-13-gcc-c++`
- [ ] `sudo dnf install -y mesa-libGL-devel libxkbcommon-x11-devel libxkbcommon-devel libX11-devel libXi-devel at-spi2-atk-devel fuse-libs`
- [ ] `./scripts/setup-toolchain.sh` (Qt 6.5.3 qtbase)
- [ ] `./scripts/build.sh`
- [ ] 실행 확인 — X11/Wayland 없으면 `QT_QPA_PLATFORM=offscreen` 으로 최소 기동만 확인
- [ ] YUV 파일 하나 열어보기 (B 기능 동작 확인)
- [ ] AV1/H.264/HEVC 스트림 하나씩 열어 Bitstream Analysis 탭 확인 (A 기능 동작 확인)

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
