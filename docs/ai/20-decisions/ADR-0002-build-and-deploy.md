---
title: ADR-0002 빌드 툴체인과 다중 머신 배포 방식
status: proposed
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
---

## 배경

요구: **로컬에서 빌드해서 여러 머신에 쉽게 설치해서 사용**.

빌드 머신 실측 (2026-07-27):
```
Rocky Linux 8.10      glibc 2.28      gcc 8.5.0      cmake 3.26.5      ninja 있음
Qt: 없음. dnf에 qt6* 패키지 자체가 없음 (RHEL/Rocky 8 + EPEL 8 모두)
/opt/rh: gcc-toolset-9 설치됨 / dnf로 gcc-toolset-10~15 설치 가능
podman 4.9.4 있음, docker 없음
python 3.6.8 (기본) + python3.11 설치됨
```

upstream은 qmake 전용, Qt 6, C++20.

## 검토한 선택지

### 컴파일러
GCC 8.5는 **두 가지 독립된 이유로 불가**:
1. `std::filesystem`이 GCC 8에서는 `-lstdc++fs` 필요 → YUView 빌드 파일에 없음 → 링크 실패 (GCC 9에서 libstdc++로 통합)
2. Qt 6.3+ 는 GCC 9 이상, Qt 6.7+ 는 사실상 GCC 11+ 요구

C++20 *언어* 기능은 문제가 아니다 — YUView가 실제로 쓰는 건 designated initializer 뿐이고 GCC 8도 `-std=c++2a`에서 지원한다. 막는 건 표준 라이브러리와 Qt.

→ **`gcc-toolset-13`** (gcc-toolset-9는 Qt 6.5 빌드에 아슬아슬, 13이 여유 있고 dnf로 바로 설치됨)

### Qt
| 후보 | 판정 |
|---|---|
| **Qt 6.5.3 LTS, aqtinstall prebuilt** | ✅ **채택.** Qt 공식 prebuilt Linux 바이너리는 **6.8부터 glibc > 2.28 요구**. 우리 glibc는 정확히 2.28. 6.5.x `gcc_64`는 RHEL8 세대 기준선으로 빌드되어 동작한다. YUView의 유일한 6.7 게이트(`TypedefQtDeprecated.h:37`)는 `#else` 분기가 있어 무해. 필요 모듈이 전부 qtbase 안에 있어 `-m qtbase`면 충분 |
| Qt 6.9 (CI 버전) | ❌ glibc 2.28에서 prebuilt 실행 불가. 소스 빌드는 수 시간 |
| Qt 5.15 (dnf) | ❌ CI 검증 없음, Qt5 분기 방치, `QT += opengl` 의미가 Qt6와 다름(`QOpenGLWidget`이 `QtOpenGLWidgets`로 이동). C++20 때문에 gcc-toolset은 여전히 필요 → 얻는 게 없음 |
| conan / vcpkg | ❌ 둘 다 Qt6를 소스 빌드. aqtinstall 대비 이점 없음 |

⚠️ 시스템 `pip3`(Python 3.6)는 aqtinstall 3.0.4에 고정되어 최신 Qt를 모른다. **`/usr/bin/python3.11`의 venv 사용.**

### 빌드 시스템: qmake 유지 vs CMake 이식
| | qmake 유지 | CMake 이식 |
|---|---|---|
| 리스크 | 낮음 (CI 검증된 경로) | 중간 |
| 우리 코드 추가 | glob이 자동 인식 (upstream 트리 안에 넣을 때만) | 명시적 |
| upstream 리베이스 | 빌드 파일 충돌 없음 | 충돌 없음 (별도 파일) |
| CPack / 패키징 | 없음 | ✅ RPM/DEB/AppImage 통합 |
| IDE / compile_commands.json | 약함 | ✅ |
| 작업량 | 0 | ~50줄 (glob + AUTOMOC/AUTOUIC/AUTORCC) + `YUVIEW_VERSION`/`YUVIEW_HASH` 수동 주입 |

## 결정

**단계적 접근.**

**Phase 0 — qmake 그대로 부트스트랩.** upstream을 손대지 않고 먼저 빌드가 되는 것을 확인한다. 이게 안 되면 나머지 논의는 무의미하다.
```bash
# 컴파일러 (필수). gcc-toolset-9 는 실측으로 탈락 -> TASK-0001
sudo dnf install -y gcc-toolset-13 gcc-toolset-13-gcc-c++
# 링크 (필수). Qt6Gui.prl 이 -lGL 을 요구하는데 libGL.so 심볼릭은 devel 에만 있다
sudo dnf install -y mesa-libGL-devel
# GUI 실행에만 필요 (헤드리스 빌드는 불필요). Qt 는 xcb-util 계열을 번들하지 않는다 (실측)
sudo dnf install -y xcb-util-wm xcb-util-image xcb-util-keysyms \
                    xcb-util-renderutil xcb-util-cursor   # cursor 는 EPEL

python3.11 -m venv ~/.venv-aqt && ~/.venv-aqt/bin/pip install -U aqtinstall
# 주의: qtbase 는 모듈이 아니라 기본 패키지다. '-m qtbase' 는 에러가 난다.
# 기본 설치가 YUView 요구 모듈 7개를 모두 포함한다 (검증 완료).
~/.venv-aqt/bin/aqt install-qt linux desktop 6.5.3 gcc_64 --outputdir ~/Qt

scl enable gcc-toolset-13 -- bash -c '
  export PATH=~/Qt/6.5.3/gcc_64/bin:$PATH
  mkdir -p build && cd build && qmake .. && make -j$(nproc)'
```

> ✅ **Qt 6.5.3 설치 검증 완료 (2026-07-27)** — `~/Qt/6.5.3/gcc_64`, `qmake -query QT_VERSION` → `6.5.3`.
> 요구 모듈 8개(Core/Gui/Widgets/OpenGL/OpenGLWidgets/Xml/Concurrent/Network) 전부 존재.
> glibc 2.28 머신에서 prebuilt 가 정상 동작 → **6.5 LTS 선택이 실증됨**.
- **out-of-tree 빌드 필수** — `.qmake.conf`가 `top_builddir=$$shadowed($$PWD)`, `YUViewApp.pro`가 `PRE_TARGETDEPS += $$top_builddir/YUViewLib/libYUViewLib.a` 하드코딩
- `CONFIG+=UNITTESTS` 생략 — googletest 서브모듈이 SSH URL이고 미초기화
- `.git` + 태그 유지 (없으면 `YUVIEW_VERSION`이 `0`)

**Phase 1 — CMake로 이식.** Phase 0이 성공한 직후. 이유: CPack으로 RPM/DEB/AppImage/tar.gz를 한 명령으로 뽑는 게 "여러 머신에 쉽게 설치"의 핵심이고, 우리 `src/` 트리를 upstream과 물리적으로 분리하려면 glob에 의존하지 않는 빌드가 필요하다.
- `cmake/YUViewLib.cmake` — upstream 소스를 glob + `AUTOMOC`/`AUTOUIC`/`AUTORCC`, `YUVIEW_VERSION`/`YUVIEW_HASH` 를 `add_compile_definitions`로 주입
- 우리 타겟: `bda_core`, `bda_bitstream`, `bda_yuv`, `bda_generator`, `bda_filter`, `bda_app`, `bda_cli`

### 배포: **RHEL 8에서 빌드한 AppImage를 1차 산출물로**

이게 핵심 통찰이다: **우리 빌드 머신의 glibc 2.28이 거의 모든 타겟보다 오래됐다.** AppImage 이식성의 표준 관행이 "가장 오래된 배포판에서 빌드"인데, 여기서는 낡은 RHEL 8이 오히려 이점이다.

```bash
curl -L https://github.com/probonopd/linuxdeployqt/releases/download/continuous/\
linuxdeployqt-continuous-x86_64.AppImage -o linuxdeployqt.AppImage && chmod +x $_

make INSTALL_ROOT=appdir install
./linuxdeployqt.AppImage appdir/usr/local/share/applications/*.desktop \
    -appimage -bundle-non-qt-libs
```

⚠️ **gcc-toolset의 `libstdc++`는 시스템 것보다 새롭다.** 대책 없이 배포하면 타겟에서 `GLIBCXX_3.4.26 not found`.
→ `QMAKE_LFLAGS += -static-libstdc++ -static-libgcc` (CMake에서는 `target_link_options`) 또는 linuxdeployqt가 toolset `libstdc++.so.6`를 번들하게 할 것. **정적 링크 쪽을 기본으로 한다** (번들보다 실패 모드가 적음).

**보조 산출물**
| 형태 | 용도 |
|---|---|
| **AppImage** | 1차. 파일 하나 복사 → 실행. 타겟에 `fuse-libs` 필요 |
| **`.rpm` (CPack)** | RHEL/Rocky 8 사내 머신에 `dnf install`. 데스크톱 통합(.desktop/MIME/아이콘)이 깔끔 |
| **`.tar.gz`** | 루트 권한 없는 머신 |
| **Flatpak** (podman으로 빌드) | 이기종 배포판 대응. upstream 매니페스트가 libde265 + ffmpeg 7.1.2를 알아서 빌드해줌 |

**podman의 역할**: 로컬 빌드 대체가 아니라 **(a) Flatpak 굽기, (b) Ubuntu 22.04로 CI 재현** 용도. ubuntu 컨테이너 산출물은 glibc≥2.35라 우리 RHEL8 머신에서 못 돈다.

**배포 시 동봉**: `LICENSE.GPL3` (GPLv3 §6 대비, upstream CI도 그렇게 함), libde265 번들 시 `libde265License.txt`.

## 결과

- 첫 빌드까지 반나절 수준, 위험 낮음
- 하나의 AppImage로 사내 리눅스 머신 대부분 커버
- gcc-toolset 의존이 빌드 머신에만 국한됨 (정적 링크 덕분)

**감수하는 것**
- Qt 6.5.3 고정 → upstream이 6.7+ 전용 API를 쓰기 시작하면 패치 필요 (현재 그런 지점 1곳, `#else` 있음)
- CMake 이식 유지보수 (~50줄 + upstream 파일 목록 변화 추종)

## 재검토 트리거
- 빌드 머신이 RHEL 9+ / glibc 2.34+ 로 올라가면 → Qt 6.8/6.9 사용 가능, 단 AppImage 이식성은 나빠짐
- Windows / macOS 지원 요구가 생기면 → CMake 이식이 선행 조건
