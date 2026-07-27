---
title: 빌드 / 배포 / 라이선스 제약
status: active
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
---

## 빌드 머신 (실측 2026-07-27)

```
Rocky Linux 8.10        glibc 2.28        gcc 8.5.0 (시스템)
cmake 3.26.5            ninja 있음        podman 4.9.4 (docker 없음)
python 3.6.8 (기본) + python3.11 설치됨
Qt: 없음. dnf에 qt6* 패키지가 존재하지 않음 (RHEL/Rocky 8 + EPEL 8)
/opt/rh: gcc-toolset-9 / dnf로 gcc-toolset-10~15 설치 가능
디스크: /home/knight2 5.5T (3.1T 여유)
```

## 하드 제약

| 제약 | 근거 | 결과 |
|---|---|---|
| **시스템 gcc 8.5로는 빌드 불가** | ① `std::filesystem`이 GCC 8에서 `-lstdc++fs` 필요한데 YUView 빌드 파일에 없음 ② Qt 6.3+ 는 GCC 9+, 6.7+ 는 사실상 GCC 11+ | `gcc-toolset-13` 필수 |
| **Qt 6.8+ prebuilt 사용 불가** | 공식 Linux 바이너리가 glibc > 2.28 요구. 우리는 정확히 2.28 | **Qt 6.5.3 LTS 고정** |
| **Qt6 배포 패키지 없음** | RHEL/Rocky 8 + EPEL 8에 부재 | aqtinstall (python3.11 venv — 시스템 pip3는 Python 3.6이라 aqtinstall 3.0.4에 고정됨) |
| **out-of-tree 빌드 필수** | `.qmake.conf`의 `top_builddir=$$shadowed($$PWD)`, `YUViewApp.pro`의 `PRE_TARGETDEPS` 하드코딩 | `mkdir build && cd build && qmake ..` |
| **gcc-toolset libstdc++ 누출** | toolset의 libstdc++가 시스템보다 새로움 | `-static-libstdc++ -static-libgcc` 또는 번들. 안 하면 타겟에서 `GLIBCXX_3.4.26 not found` |
| **GPLv3-or-later 전염** | 예외는 OpenSSL 링크뿐. CLA 없음. 단독 저작권자 RWTH Aachen | 폐쇄소스 배포 불가. 사내 배포는 소스 tarball 동봉으로 충족 |

## 연성 제약 / 함정

- `submodules/googletest`가 **SSH URL**(`git@github.com:`)이고 미초기화 → `CONFIG+=UNITTESTS` 쓰려면 HTTPS로 override
- `.git` + 태그 유지 안 하면 `YUVIEW_VERSION`이 `0` (qmake가 `git describe --tags` 호출)
- 코덱 라이브러리(FFmpeg 포함)는 전부 **런타임 `dlopen`, 빌드 의존 아님**
- upstream 자동 업데이트는 `common/Typedef.h:105` `UPDATE_FEATURE_ENABLE 0` 으로 꺼져 있고 Windows 전용 → Linux에서 비활성
- `snapcraft.yaml`은 stale (Qt5 기준, v2.14). 참조 금지
- 라이브러리가 요구하는 Qt 모듈 `core gui widgets opengl xml concurrent network` 는 **전부 qtbase 안** → `aqt ... -m qtbase` 로 충분

## 배포 대상

사내 리눅스 머신 여러 대. 배포판/버전 이기종 가능성 있음.
→ **glibc 2.28 빌드 머신이 오히려 이점.** AppImage 이식성 최대화.
→ [ADR-0002](../20-decisions/ADR-0002-build-and-deploy.md)
