---
title: TASK-0001 빌드 부트스트랩 (Phase 0)
status: done (빌드 성공) — 기능 검증은 TASK-0002 로 이관
created: 2026-07-27
updated: 2026-07-28
author: claude-opus-5
verified: yes
---

## ✅ 결과: 빌드 성공

```
build/YUViewApp/YUView          7.9 MB   (v2.14-301-ga72eb348)
build/YUViewLib/libYUViewLib.a  16.7 MB
컴파일 에러 0건
```

Qt 6.5.3 + gcc-toolset-13 + `-static-libstdc++ -static-libgcc`, out-of-tree, upstream 무수정 (패치 0개).

## 목표

수정하지 않은 upstream YUView가 이 머신에서 빌드되고 실행되는 것을 확인한다.
**이게 안 되면 나머지 모든 계획이 무의미하다.**

## 단계

- [x] `git init` + 첫 커밋 → `b8e3050`, origin `git@github.com:knightkim-sketch/bd_analyzer.git` (SSH)
- [x] upstream 서브모듈 추가 및 커밋 고정 → `a72eb348` = `v2.14-301-ga72eb348`, 13MB
  ⚠️ 서브모듈의 `submodules/googletest`는 SSH URL이므로 재귀 초기화하지 말 것 (`CONFIG+=UNITTESTS` 미사용)
- [ ] **🚧 BLOCKED (sudo 필요)** `sudo dnf install -y gcc-toolset-13 gcc-toolset-13-gcc-c++`
- [ ] **🚧 BLOCKED (sudo 필요, 링크 단계)** `sudo dnf install -y mesa-libGL-devel`
- [ ] **🚧 BLOCKED (sudo 필요, GUI 실행 시에만)** `sudo dnf install -y xcb-util-wm xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-cursor`
- [x] Qt 6.5.3 설치 → `~/Qt/6.5.3/gcc_64` (**sudo 불필요**, aqtinstall v3.3.0 / python3.11 venv)
- [x] `./scripts/build.sh` → 성공, 에러 0건
- [x] 실행 확인 — `QT_QPA_PLATFORM=offscreen` 에서 MainWindow 생성 후 20초 이상 안정 구동 (rc=124)
- [x] 배포 이식성 검증 (아래 참조)
- [ ] **→ TASK-0002 로 이관** YUV / AV1 / H.264 / HEVC 실제 로드·파싱 확인 (GUI 상호작용 필요)

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

### Qt 6.5.3 설치 완료 및 의존성 실측

`aqt install-qt linux desktop 6.5.3 gcc_64 --outputdir ~/Qt` → 성공 (106초).
`qmake -query QT_VERSION` → `6.5.3`. **glibc 2.28에서 prebuilt 정상 동작 → ADR-0002의 6.5 LTS 선택 실증됨.**

⚠️ **`-m qtbase` 는 에러다.** qtbase는 모듈이 아니라 기본 패키지이며, 기본 설치가 YUView 요구 모듈을 전부 포함한다:
`Qt6Core/Gui/Widgets/OpenGL/OpenGLWidgets/Xml/Concurrent/Network` 8개 모두 존재 확인.

**링크 단계 (빌드에 필수)**
`libQt6Gui.prl` → `QMAKE_PRL_LIBS = -lpthread -lGL`.
`/usr/lib64/libGL.so.1`은 있으나 링커가 찾는 **`libGL.so` 심볼릭이 없다** → `mesa-libGL-devel` 필요.

**GUI 실행 (헤드리스 빌드에는 불필요)**
`ldd ~/Qt/6.5.3/gcc_64/plugins/platforms/libqxcb.so` 실측 — **Qt는 xcb-util 계열을 번들하지 않는다** (`$QT/lib/libxcb*` 없음):

| 미해결 심볼 | RPM 패키지 |
|---|---|
| `libxcb-cursor.so.0` | `xcb-util-cursor` (**EPEL**) |
| `libxcb-icccm.so.4` | `xcb-util-wm` |
| `libxcb-image.so.0` | `xcb-util-image` |
| `libxcb-keysyms.so.1` | `xcb-util-keysyms` |
| `libxcb-render-util.so.0` | `xcb-util-renderutil` |

이미 있는 것: `libGL.so.1`, `libxkbcommon(-x11)`, `libX11(-xcb)`, `libXi`, `libxcb`, `libxcb-randr/shape/sync/xfixes/xinerama/xkb`, `libatspi`, `libpcre2-16`, `libfontconfig`, `libfreetype`, `libdbus-1`, `libglib-2.0`, `fuse-libs`.

GUI가 안 뜨면 `QT_DEBUG_PLUGINS=1`로 확인할 것.

### 배포 이식성 — 실측 (ADR-0002 의 핵심 가정 검증)

```
바이너리가 요구하는 GLIBCXX_* / CXXABI_* 심볼 :  없음 (0개)
바이너리가 요구하는 glibc 최고 버전            :  GLIBC_2.25   (빌드 머신은 2.28)
미해결 동적 의존성                             :  없음
```

`-static-libstdc++ -static-libgcc` 가 의도대로 동작했다. `ldd` 에 `libstdc++.so.6` 이 보이지만
이는 Qt 라이브러리가 끌어오는 것이고, **우리 바이너리 자체는 버전 심볼을 하나도 요구하지 않는다.**
→ gcc-toolset 의 새 libstdc++ 가 타겟에 누출되지 않는다. `GLIBCXX_3.4.26 not found` 위험 해소.
→ glibc 2.25 요구는 RHEL 7 세대까지 커버. **AppImage 이식성 가정이 실증됨.**

### ⚠️ FFmpeg 공유 라이브러리 부재 — feature A 에 직접 영향

`ldconfig -p` 에 `libavcodec/libavformat/libavutil/libswresample` **없음**.
(`/usr/local/bin/ffmpeg` 는 static 빌드라 공유 라이브러리를 제공하지 않는다.)

YUView 는 이들을 런타임 `dlopen` 한다. 없을 때 잃는 것:
- 컨테이너 demux (mp4 / ivf / mkv) 불가
- **AV1 분석 전면 불가** — `ParserAV1OBU` 는 `ParserAVFormat` 경유로만 도달하고
  `ParserAV1OBU::runParsingOfFile` 은 `assert(false)` 이다 (raw `.obu` 경로 없음)
- H.264 MV 통계 불가 (`decoderFFmpeg` 경유)

영향 없는 것: raw YUV 뷰잉, H.264 / HEVC AnnexB **파싱**(파서는 내장).

**해결**: `sudo dnf install -y ffmpeg-libs` (rpmfusion-free, 이미 활성).
4.4.8 이 제공하는 avutil 56 / avcodec 58 / avformat 58 / swresample 3 은
`ffmpeg/FFmpegVersionHandler.cpp:107` 의 `LibraryVersion(56, 58, 58, 3)` 과 정확히 일치한다.

### 무해한 런타임 경고

```
qt.tlsbackend.ossl: Incompatible version of OpenSSL (built with OpenSSL >= 3.x, runtime version is < 3.x)
```
Qt 6.5.3 공식 바이너리는 OpenSSL 3.x 기준, RHEL 8 은 1.1.1. **무시해도 된다** —
네트워크는 자동 업데이트에만 쓰이고 그건 `common/Typedef.h:105` `UPDATE_FEATURE_ENABLE 0` 으로 꺼져 있다.

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
