---
title: YUView 빌드 시스템 / 라이선스 / 배포 분석
status: active
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 1. 라이선스 — GPLv3-or-later + **OpenSSL 링크 예외만**

저작권자: **Institut für Nachrichtentechnik, RWTH Aachen University (2015)** — 단독 보유자.
`YUViewLib/src` + `YUViewApp/src` 541개 파일 중 **526개**가 GPLv3+OpenSSL 예외 헤더 보유.
나머지 15개는 전부 `decoder/externalHeader/` 아래 서드파티 API 헤더.

`packaging/linux/de.rwth_aachen.ient.YUView.appdata.xml:7` → SPDX `GPL-3.0-or-later`.

### 예외 조항 전문 (`LICENSE.GPL3:7-30`, 각 소스 헤더에도 동일)
> In addition, as a special exception, the copyright holders give permission to link the code of portions of this program **with the OpenSSL library** under certain conditions...
> **You must obey the GNU General Public License in all respects for all of the code used other than OpenSSL.**

- **CLA 없음.** `CONTRIBUTING.md` 없음. `HACKING.md`는 코딩 컨벤션만.
- Qt LGPL/GPL 호환용 "additional permission" (§7 추가 조항) **없음**.
- 일반적인 링크 예외 **없음**.

### 세 가지 시나리오 (라이선스 텍스트 그대로의 보고 — 법률 자문 아님)

**(a) 폐쇄소스 제품에 YUViewLib 링크**
`YUViewLib`는 `staticlib`(`YUViewLib.pro:3-4`) → 바이너리에 정적 링크됨.
GPLv3 §5(c)(`LICENSE.GPL3:240~`)는 배포되는 수정 저작물 **전체**를 GPLv3로 라이선스할 것을 요구하고, §6(`:277`)은 비소스 배포 시 Corresponding Source 제공을 요구한다.
예외 조항 범위는 명시적으로 OpenSSL 한정이며 우리 독자 코드에는 아무것도 부여하지 않는다.
→ **텍스트상 결론: 정적 링크 바이너리를 배포하면 결합 저작물 전체 소스를 GPLv3로 제공할 의무가 발생.** in-tree 듀얼 라이선스 옵션 없음. RWTH Aachen만이 예외를 부여할 수 있다.

**(b) GPL 호환 제품으로 fork**
허용됨. §5 조건: (i) 수정 표시 + 날짜, (ii) 전체를 GPLv3로, (iii) 라이선스 고지 보존, (iv) 인터랙티브 UI에 Appropriate Legal Notices 유지.
"or (at your option) any later version" → GPL-3.0-**or-later**로 재배포 가능.
§10(`:478`)에 의해 하위 수령자는 원 라이선서로부터 직접 자동 라이선스를 받으며, 추가 제약을 부과할 수 없다.
OpenSSL 예외 문구는 우리 수정 파일로 **전파해도 되고 삭제해도 된다**(선택).
⚠️ `externalHeader/`의 서드파티 헤더는 우리가 재라이선스할 수 없음 (§3 참조).

**(c) 사내 여러 머신에 바이너리 배포**
GPLv3 §0(`:105`) 정의: *"To **convey** a work means any kind of propagation that enables other parties to make or receive copies."*
§2(`:186`): *"You may make, run and propagate covered works that you **do not convey**, without conditions."*
- 라이선스 텍스트에 사내 사용 예외 조항은 없고, "other parties"의 정의도 없다. 동일 법인 내 직원 배포가 convey인지는 텍스트만으로 결정되지 않는다.
- **실무적으로는 무의미한 논점**: convey로 간주하더라도 §6은 바이너리 옆에 소스 tarball을 같이 두면 충족된다. 사내 배포에서 이건 사실상 비용 0.
- 사용 대수, 상업적 사용에 대한 제약은 없다.

### 번들 서드파티 (`YUViewLib/src/decoder/externalHeader/`)
구현체는 vendor 되어 있지 않고 **헤더(선언)만** — 런타임 `dlopen`용.

| 경로 | 라이선스 |
|---|---|
| `libde265/de265.h`, `de265_internals.h` | LGPL-3.0-or-later (struktur AG / Dirk Farin) |
| `dav1d/*.h`, `blockData.h` | BSD-2-Clause (VideoLAN / Two Orioles) |
| `vvdec/*.h` | **Clear BSD** (Fraunhofer HHI) — 특허권 명시적 불허 |
| `libHMDecoder.h`, `libVTMDecoder.h` | BSD-3-Clause (ITU/ISO/IEC) — 특허권 불허 고지 포함 |

→ fork 시 이 파일들의 고지는 그대로 유지해야 하며, Clear-BSD/ITU 특허 면책이 따라온다.

## 2. 빌드 시스템 — **qmake 전용, CMake 없음**

`README.md`: *"We use qmake for the project so on all supported platforms you just have to install qt and run `qmake` and `make`."*

### CI (`.github/workflows/Build.yml`) — 4개 잡, 전부 qmake

| 잡 | 러너 | Qt 출처 | 명령 |
|---|---|---|---|
| `build-unix-native` | ubuntu 22.04/24.04 (x64+arm) | `apt install qt6-base-dev` (→6.2.4 / 6.4.2) | `mkdir build && cd build && qmake6 CONFIG+=UNITTESTS .. && make -j$(nproc)` |
| `build-mac-native` | macos-15 (arm+intel) | `brew install qt` | `qmake6 CONFIG+=UNITTESTS .. && make -j$(sysctl -n hw.logicalcpu)` |
| `build-linux-mac` (릴리스) | ubuntu-22.04, macos-15 | **커스텀 prebuilt** `qtBase-6-9-0-${os}.zip` (ChristianFeldmann/YUViewQt 릴리스) | `qmake CONFIG+=UNITTESTS .. && make -j4` |
| `build-windows` | windows-2022 / MSVC | 같은 zip | `qmake CONFIG+=UNITTESTS .. && jom` |

- 테스트: `QT_QPA_PLATFORM=offscreen ./YUViewUnitTest/YUViewUnitTest`
- `actions/checkout@v6` + `submodules: true` + `git fetch --prune --unshallow` (`git describe --tags` 때문)
- `.github/workflows/flatpak.yml` — 별도 잡, `flatpak-builder@v6`, 매니페스트 `de.rwth_aachen.ient.YUView.yaml`

### 낡은 것
- `snapcraft.yaml` — **stale**: `version: 2.14`, `qt-version: qt5`, `qtbase5-dev`. 어떤 워크플로도 참조 안 함. 현재 Qt6 소스를 빌드 못 함
- `deployment/versioning.py:9` `qtver='5.13'` — Windows versioninfo용 죽은 상수
- 루트의 `YUView.desktop` — install 되지 않고 `packaging/linux/` 버전과 내용이 다름

## 3. 정확한 의존성 목록

- **Qt 모듈**: `core gui widgets opengl xml concurrent network` — **전부 qtbase**. 추가 모듈 불필요
- **Qt 최소 버전 선언 없음.** `lessThan(QT_MAJOR_VERSION, 6)` 가드조차 없음. 실질 하한 Qt 6.2
- **C++20** (`CONFIG += c++20` ×4). 실사용 기능은 designated initializers 뿐 (`video/rgb/ConversionFunctions.h:79,84`, `ConversionDifferenceRGB.cpp:147`). concepts/`<=>`/`std::span`/`std::format`/ranges/coroutines **전부 미사용**
- **C++17 `<filesystem>`** 5개 헤더에서 사용 (`filesource/FileSource.h`, `parser/Parser.h`, `filesource/FrameFormatGuess.h`, `dataSource/DataSourceLocalFile.h`, 테스트 1개).
  ⚠️ **`-lstdc++fs`가 어떤 빌드 파일에도 없다** → GCC 8에서 링크 실패 (§5)
- **서브모듈 1개**: googletest. URL이 **SSH**(`git@github.com:`) → HTTPS 사용자는 override 필요. 현재 체크아웃에서 디렉토리 비어 있음. `CONFIG+=UNITTESTS` 일 때만 필요
- **코덱 라이브러리는 전부 런타임 optional `dlopen`.** FFmpeg조차 빌드 타임에 링크되지 않음 (`ffmpeg/FFmpegLibraryFunctions.cpp`가 심볼을 동적 resolve, 부재 허용). README: *"There are no further dependent libraries."*
- **빌드 타임 네트워크**: `git describe --tags` (태그 필요, tarball 빌드 시 버전만 `0`으로 degrade), googletest 서브모듈(테스트 시)
- **런타임 네트워크**: `handler/UpdateHandler.cpp` 자동 업데이트 → `common/Typedef.h:105` `#define UPDATE_FEATURE_ENABLE 0` 로 꺼져 있고 Windows 전용. **Linux에서는 비활성.** 이게 OpenSSL 예외가 존재하는 이유

## 4. 배포 아티팩트

| 플랫폼 | 아티팩트 | 레시피 |
|---|---|---|
| Linux | **AppImage** (linuxdeployqt) | `Build.yml` job `build-linux-mac` |
| Linux | **Flatpak** (flathub `de.rwth_aachen.ient.YUView`) | `de.rwth_aachen.ient.YUView.yaml` + `flatpak.yml` |
| Linux | Snap — 선언만, stale | `snapcraft.yaml` |
| Linux | `make install` (FHS) | `YUViewApp.pro:28-58` |
| macOS | `.app` zip (macdeployqt) | `Build.yml` |
| Windows | zip (windeployqt) + **MSI** (WiX 3.14) | `deployment/wix/YUView.wxs` |
| — | **`.deb`/`.rpm` 레시피는 in-tree에 없음** (Debian이 downstream 관리) | — |

### Linux AppImage 정확한 레시피 (`Build.yml`, ubuntu-22.04)
```bash
# 1. prebuilt Qt 6.9.0 qtbase (소스 트리 밖)
curl -L .../QtBase-6.9.0/qtBase-6-9-0-ubuntu-22.04.zip -o Qt.zip && unzip -qa Qt.zip
export PATH=$PWD/Qt/bin:$PATH

# 2. linuxdeployqt
curl -L .../linuxdeployqt-continuous-x86_64.AppImage -o linuxdeployqt.AppImage && chmod +x linuxdeployqt.AppImage

# 3. Qt xcb 플랫폼 플러그인용 시스템 라이브러리 (+ AppImage용 libfuse2)
sudo apt-get install libgl1-mesa-dev libxkbcommon-x11-0 libpcre2-16-0 '^libxcb.*-dev' \
  libx11-xcb-dev libglu1-mesa-dev libxi-dev libxkbcommon-dev libxkbcommon-x11-dev libatspi2.0-dev libfuse2

# 4. (선택) HEVC internals 디코더를 바이너리 옆에 배치
curl -L .../ChristianFeldmann/libde265/releases/download/v1.1/libde265.so -o libde265-internals.so

# 5. 빌드
mkdir build && cd build && qmake CONFIG+=UNITTESTS .. && make -j4

# 6. staging + 번들
make INSTALL_ROOT=appdir install
../linuxdeployqt.AppImage \
  YUViewApp/appdir/usr/local/share/applications/de.rwth_aachen.ient.YUView.desktop \
  -appimage -bundle-non-qt-libs -verbose=2
```
`make install` 타겟은 `YUViewApp.pro:28-58` 정의: 바이너리 + appdata + desktop + MIME xml + 아이콘 32/64/128/256/512.
(`icon1024.path`는 `INSTALLS`에 있으나 `.files`가 없음 — 무해한 no-op)

## 5. 우리 환경(Rocky Linux 8.10 / GCC 8.5 / Qt 없음) 실현 가능성

### 실측 프로브 결과

```
gcc 8.5.0 (RH 8.5.0-28)   glibc 2.28   cmake 3.26.5   ninja 있음   Rocky Linux 8.10
/opt/rh/            → gcc-toolset-9 설치되어 있음 (g++ 9.2.1)
dnf available       → gcc-toolset-10/11/12/13/14/15 전부 설치 가능 (appstream)
docker              → 없음
podman              → /bin/podman 4.9.4-rhel,  buildah 1.33.14
python3             → 3.6.8 (기본) + python3.11 설치되어 있음
dnf list 'qt6*'     → 비어 있음. RHEL/Rocky 8 + EPEL 8에 Qt6 없음
dnf list qt5-qtbase-devel → 5.15.3-8.el8_10 사용 가능
```

### 판정: **GCC 8.5로는 불가능. 독립된 두 개의 블로커.**

**블로커 1 — `std::filesystem` 링크 실패 (확실)**
GCC 8의 `std::filesystem`은 별도 `libstdc++fs.a`에 있고 `-lstdc++fs`를 명시해야 한다. GCC 9부터 `libstdc++`에 통합.
YUView는 5개 헤더에서 `<filesystem>`을 쓰는데 `-lstdc++fs`가 **어떤 빌드 파일에도 없다** → undefined reference.
(패치는 사소: `qmake LIBS+=-lstdc++fs`)

**블로커 2 — Qt 6 헤더 vs GCC 8 (본질적)**
Qt 6.0–6.2는 GCC 8 지원, **Qt 6.3부터 GCC 9 이상**, Qt 6.7+는 사실상 GCC 11+ 요구 (Qt 자체가 C++20으로 이동).
게다가 RHEL/Rocky 8에는 Qt6 dev 패키지가 아예 없고, 시스템 컴파일러로 Qt6를 직접 빌드하는 것도 불가 (Qt 6.9는 GCC 11+ 필요).

**블로커 아님:** YUView가 실제 쓰는 C++20 기능은 designated initializer 뿐이고 GCC 8도 `-std=c++2a`에서 지원한다. 막는 건 *언어*가 아니라 *표준 라이브러리와 Qt*.

### 권장 경로 — gcc-toolset-13 + Qt 6.5 LTS (aqtinstall) + RHEL8에서 AppImage 빌드

```bash
sudo dnf install gcc-toolset-13 gcc-toolset-13-gcc-c++
sudo dnf install mesa-libGL-devel libxkbcommon-x11-devel libxkbcommon-devel \
     libX11-devel libXi-devel at-spi2-atk-devel fuse-libs

python3.11 -m venv ~/qtenv && ~/qtenv/bin/pip install -U aqtinstall
~/qtenv/bin/aqt install-qt linux desktop 6.5.3 gcc_64 -m qtbase --outputdir ~/Qt

scl enable gcc-toolset-13 bash
export PATH=~/Qt/6.5.3/gcc_64/bin:$PATH
```

**왜 6.9가 아니라 6.5 LTS인가**: Qt 공식 prebuilt Linux 바이너리는 **6.8부터 glibc가 2.28보다 높아야 한다.** 우리 glibc는 정확히 2.28 (RHEL 8 기준선). Qt 6.5.x `gcc_64`는 RHEL 8 세대 기준선으로 빌드되어 glibc 2.28에서 동작한다.
YUView 코드에서 유일한 6.7 게이트(`TypedefQtDeprecated.h:37`)는 `#else` 분기가 있으므로 문제없다.

**시스템 pip3(Python 3.6)를 쓰지 말 것** — aqtinstall 3.0.4에 고정되어 최신 Qt 릴리스를 모른다. `/usr/bin/python3.11` 사용.

**다중 머신 배포**: CI와 동일하게 AppImage를 만들되 **RHEL 8에서 빌드**한다. 우리 glibc 2.28이 거의 모든 타겟보다 오래됐으므로 이식성이 최대가 된다 — AppImage의 표준 관행이고, 여기서는 오히려 낡은 배포판이 이점.
⚠️ gcc-toolset의 `libstdc++`는 시스템 것보다 새롭다. `QMAKE_LFLAGS += -static-libstdc++ -static-libgcc` 를 주거나 linuxdeployqt가 toolset의 `libstdc++.so.6`를 번들하게 해야 한다. 아니면 타겟에서 `GLIBCXX_3.4.26 not found`.

### 대안

| 옵션 | 평가 |
|---|---|
| **podman + ubuntu:22.04 빌드** (docker 없음, podman 4.9.4 있음) | CI와 동일 환경 → 가장 빠른 "일단 되게 하기". 단 결과 바이너리는 glibc≥2.35 요구 → RHEL8 호스트에서 못 돈다. **Flatpak을 컨테이너에서 굽는 용도로 쓰면 최선** (`org.kde.Platform 6.9` 런타임 + libde265 + ffmpeg 7.1.2를 매니페스트가 알아서 빌드) |
| **flathub 기성 빌드 설치** | `flatpak install flathub de.rwth_aachen.ient.YUView`. 빌드 0. 소스 수정이 필요 없다면 최선 — 하지만 우리는 fork 할 것이므로 참고용 |
| **Qt 5.15 (RHEL repo)** | 비권장. CI 검증 없음, Qt5 분기 방치 상태, `QT += opengl` 의미가 다름(Qt6는 `QOpenGLWidget`이 `QtOpenGLWidgets`로 이동), 그래도 C++20 때문에 gcc-toolset은 여전히 필요 → 얻는 게 없음 |
| **conan / vcpkg** | 둘 다 Qt6를 소스 빌드(수 시간). aqtinstall prebuilt 대비 이점 없음. 제외 |

### 어떤 옵션이든 걸리는 함정
1. `submodules/googletest` 비어 있고 SSH URL → `CONFIG+=UNITTESTS` 생략하거나 HTTPS로 재작성
2. **out-of-tree 빌드 필수** — `.qmake.conf`가 `top_builddir=$$shadowed($$PWD)`, `YUViewApp.pro`가 `PRE_TARGETDEPS += $$top_builddir/YUViewLib/libYUViewLib.a` 하드코딩
3. `.git`과 태그 유지 안 하면 `YUVIEW_VERSION`이 `0`
4. 배포 시 `LICENSE.GPL3` 동봉 (CI도 그렇게 함), libde265 번들 시 `libde265License.txt` 추가
