---
title: YUView 아키텍처 / 라이브러리 재사용성 분석
status: active
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe (2026-04-28, v2.14-301-ga72eb348)
---

## 결론 요약

| 질문 | 답 |
|---|---|
| CMake 빌드가 있는가 | **없다. qmake 전용.** 저장소 전체에서 `CMakeLists.txt`는 `tools/standardTextToCode/DummyReaderAndBuildFiles/` 하나뿐이며 본체와 무관 |
| YUViewLib가 진짜 라이브러리 타겟인가 | qmake `staticlib` (`libYUViewLib.a`). SHARED 옵션 없음 |
| `find_package(YUViewLib)` 가능한가 | **불가능.** `install(EXPORT)`, `*Config.cmake`, 헤더 install, `.pc` 전부 없음. `YUViewLib.pro`에 `INSTALLS` 자체가 없음 |
| 플러그인 시스템이 있는가 | **전혀 없다.** `QPluginLoader` / `Q_PLUGIN_METADATA` / `Q_DECLARE_INTERFACE` / `dlopen` 검색 결과 0건 |
| 새 포맷/아이템 추가가 open-closed인가 | **아니다.** 모든 확장 축이 라이브러리 내부 중앙 `if/else` 체인 수정을 요구 |
| 임베드 가능성 | **낮음 (2/10)** |

## 1. 빌드 구조

- `YUView.pro` — `TEMPLATE = subdirs`, `SUBDIRS = YUViewLib YUViewApp`, `YUViewApp.depends = YUViewLib`
- `YUViewLib/YUViewLib.pro:1` — `QT += core gui widgets opengl xml concurrent network` (라이브러리 자체가 Widgets/OpenGL/Network를 끌고 온다. headless 서브셋 없음)
- `YUViewLib.pro:9-12` — glob 기반: `SOURCES += $$files(src/*.cpp, true)`, `FORMS += $$files(ui/*.ui, false)`
  → **새 소스/`.ui` 파일 추가 시 빌드 파일 수정 불필요.** 우리 fork 전략에 유리한 지점
- `YUViewApp/src/yuviewapp.cpp` — 전체 **52줄**. 앱 계층이 사실상 비어 있고 모든 앱 정책이 라이브러리 안에 있다

### 외부에서 소비할 때의 함정

1. **생성 헤더가 public 헤더로 샌다.** 21개 public 헤더가 `#include "ui_*.h"` — shadow build dir에만 존재.
   - `playlistitem/playlistItem.h:47` → `ui_playlistItem.h`
   - `video/FrameHandler.h:45` → `ui_FrameHandler.h`
   - `video/yuv/videoHandlerYUV.h:40`, `video/rgb/videoHandlerRGB.h:40`
   - `statistics/StatisticUIHandler.h:44`, `playlistitem/playlistItemCompressedVideo.h:41`

   즉 `playlistItem.h` 하나 include 하려면 YUView의 `.ui`에 `uic`를 돌려야 한다.
   반대로 `parser/`, `filesource/`, `common/` 은 `ui_*.h` 의존이 **없다** (검증 완료).

2. **static lib 안의 Qt 리소스.** `Q_INIT_RESOURCE(images)`/`(docs)` 호출이 `ui/Mainwindow.cpp:53-54` 한 곳뿐.
   `MainWindow`를 생성하지 않으면 `.qrc` 오브젝트가 아카이브에서 안 끌려나와 `:/img/...` 조회가 조용히 실패한다.

3. **include 경로 2개 필수** (`YUViewUnitTest/YUViewUnitTest.pro:14-20` 참고): `$$top_srcdir/YUViewLib/src` + `$$top_builddir/YUViewLib`.

4. `YUVIEW_VERSION` / `YUVIEW_HASH` 는 qmake가 `git describe`/`git rev-parse`로 주입. `common/Typedef.h:108,113`에 `#ifndef` 폴백 있음.

## 2. 디코더 로딩 — "플러그인"에 가장 가까운 것

플러그인 시스템은 아니지만 런타임 `QLibrary` 로딩 메커니즘이 있다.

- `decoder/decoderBase.h:88-176` — `decoderBase` 순수가상 계약 (`decodeNextFrame`, `pushData`, `getRawFrameData`, `fillStatisticList(stats::StatisticsData&)`)
- `decoder/decoderBase.h:180-200` — `decoderBaseSingleLib` + `QLibrary library`, 훅 `resolveLibraryFunctionPointers()`, `getLibraryNames()`
- `decoder/decoderBase.cpp:100-178` — 로딩 로직. `QSettings` 그룹 `"Decoders"` / 키 `"SearchPath"` + 7개 탐색 경로(현재 디렉토리, `applicationDirPath()`, `decoder/`, 시스템 경로)의 카티션 곱

**ABI는 디코더마다 하드코딩된 C 심볼 목록.** 각 서브클래스가 함수 포인터 POD 구조체를 갖고 이름으로 resolve. 예: `decoderLibde265.h:45` `struct LibraryFunctionsDe265`, resolve는 `decoderLibde265.cpp:141-231`.
`optional=true` 심볼(`de265_internals_*`)로 patched/stock 라이브러리 간 graceful degradation.

| 엔진 | 파일 | 라이브러리 이름 | 헤더 출처 |
|---|---|---|---|
| libde265 | `decoderLibde265.cpp:1015` | `libde265-internals`, `libde265` | **YUView fork 전용** `de265_internals.h` |
| dav1d | `decoderDav1d.cpp:575` | `libdav1d-internals`, `libdav1d` | upstream + **fork 추가** `blockData.h` |
| HM | `decoderHM.cpp:128` | `libHMDecoder` | **YUView 자작 C API** (패치된 HM 필요) |
| VTM | `decoderVTM.cpp:132` | `libVTMDecoder` | **YUView 자작 C API** |
| VVDec | `decoderVVDec.cpp:173` | `libvvdecLib` | 진짜 upstream API |
| FFmpeg | `ffmpeg/FFmpegLibraryFunctions.cpp:186-261` | 버전별 4개 lib | upstream 미러 |

> ⚠️ 블록 단위 통계(A 기능의 핵심)는 **libde265/dav1d의 YUView fork 빌드**에 의존한다. upstream 바이너리로는 안 나온다.

8번째 디코더를 추가하려면 라이브러리 안의 **5개 파일 + `.ui` 폼** 수정 필요.

## 3. 확장 지점별 수정 비용

### playlistItem
`playlistItem.h:54` — `class playlistItem : public QObject, public QTreeWidgetItem`.
**`QTreeWidgetItem`을 상속한다 = 모델이 곧 뷰.** `QTreeWidget` 밖에서 쓸 수 없다.

중앙 레지스트리는 `playlistitem/playlistItems.cpp` 261줄 한 파일에 손으로 유지되는 목록 4개:
- `:154-190` 확장자 → 아이템 타입 4-way `if`
- `:203-244` 사용자 선택 다이얼로그 (하드코딩 문자열 6개, 인덱스 기반 `if` 체인 — `:240`에 `types[3]`/`types[4]` 오프바이원 버그 잠복)
- `:254-281`, `:282-301` 필터 문자열 (중복)
- `:318-373` XML 태그 12개 → named ctor

새 playlist item 추가 = `playlistItems.cpp` 4개 함수 수정 (+`.ui`는 glob이 자동 처리).

### videoHandler
```
video::FrameHandler (FrameHandler.h:56)
└─ video::videoHandler (videoHandler.h:47)
   ├─ videoHandlerYUV / videoHandlerRGB / videoHandlerDifference / videoHandlerResample
```
가상 함수 표면은 좋음. **하지만 디스패치가 하드코딩 2-way enum switch**: `video::RawFormat {Invalid, YUV, RGB}` (`video/PixelFormat.h:41-46`).
`getYUVVideo()`/`getRGBVideo()`/`dynamic_cast` 호출처 **14곳**. 세 번째 핸들러 추가 시 전부 수정.

### parser
```
parser::Parser (Parser.h:57)
├─ parser::ParserAnnexB (ParserAnnexB.h:56) → AVC / HEVC / VVC / Mpeg2
└─ parser::ParserAVFormat  (컨테이너 demux → AnnexB 또는 ParserAV1OBU에 위임)
```
**가장 깨끗한 서브시스템**이지만 API 경계가 GUI에 묶여 있다:
- `Parser.h:74` `virtual vector<QTreeWidgetItem*> getStreamInfo() = 0` — 출력 계약이 QtWidgets 타입
- `Parser.h:65-67` `QAbstractItemModel*` / `BitratePlotModel*` / `HRDPlotModel*` 를 파서가 소유
- `ParserAnnexB.h:131` `parseAnnexBFile(..., QWidget *mainWindow = nullptr)`

**AV1은 `ParserAnnexB`가 아니다.** `ParserAV1OBU`는 `ParserAVFormat` 경유로만 도달 (`ParserAVFormat.cpp:597-598`). raw `.obu` 입력 경로 없음.

포맷 → 파서 매핑이 **4곳에 중복**: `playlistItemCompressedVideo.cpp:125-133`, `:153-172`, `ui/widgets/BitstreamAnalysisWidget.cpp:261-272`, `parser/AVFormat/ParserAVFormat.cpp:591-598`.
새 비트스트림 포맷 추가 = 라이브러리 내부 **약 7개 파일** 수정.

### statistics — 가장 재사용성 높음
- `statistics/StatisticsData.h:47-83` — `std::map` + `std::mutex`. **QObject 아님, 위젯 없음.** 거의 순수 C++
- 렌더링은 자유 함수: `StatisticsDataPainting.h:42` `paintStatisticsData(QPainter*, StatisticsData&, int, double)` — 직접 호출 가능. 단 오버라이드는 불가(가상 아님)
- `StatisticUIHandler`는 `setStatisticsData(&data)`로 깔끔히 분리됨

## 4. 전역 상태 / 임베드 장애물

### 라이브러리가 QApplication을 소유
`ui/YUViewApplication.h:37` — `class YUViewApplication : public QApplication` **가 라이브러리 안에 있다.**
생성자(`YUViewApplication.cpp:50-132`)가 `main()`이 할 일을 전부 함: 앱 이름/조직명 하드코딩(`:53-56`, 모든 `QSettings`가 여기에 묶임), 인자 파싱(`:63`), 단일 인스턴스 IPC(`:66-81`), `MainWindow w; w.show(); exec();`(`:105-131`).

### QSettings 91곳 / 32파일, 전부 기본 생성자
최악은 **코어 non-UI 기반 클래스의 멤버**: `video/FrameHandler.h:148` `QSettings settings;`
→ 모든 `videoHandlerYUV`/`RGB` 인스턴스가 앱 아이덴티티에 묶인 `QSettings`를 들고 다닌다.
호스트 앱이 YUView의 org/app 이름을 사칭하지 않으면 디코더 경로 설정 등이 조용히 기본값으로 떨어진다.

### MainWindow 앰비언트 룩업
`playlistItemCompressedVideo.cpp:142,526` → `MainWindow::getMainWindow()` (`ui/Mainwindow.cpp:228-238`, `QApplication::topLevelWidgets()` 순회 + `dynamic_cast`).
`nullptr` 처리는 되어 있으나 링크 타임에 `MainWindow`/`VideoCache`/`UpdateHandler`를 끌고 온다.

### 전역 네임스페이스 오염 — `common/Typedef.h` (namespace 없음)
거의 모든 헤더가 transitively include:
```cpp
:57   #define INT_INVALID -1              // 무조건
:55   #define INT_MAX ...                 // <climits> 매크로 재정의
:141  template <typename T> using vector = std::vector<T>;          // ::vector !!
:146  template <typename T, size_t N> using array = std::array<T,N>; // ::array !!
:179  struct Ratio; :185 struct Size; :204 struct Offset;           // 전역
:251  enum recacheIndicator                                          // unscoped, 전역
```
그리고 `ParserAnnexB.h:56`가 `vector<QTreeWidgetItem*>`를 반환 — **public 파서 API가 `::vector`로 표기되어 있다.**

기타 전역 상태: `playlistItem.h:315` `static unsigned idCounter` (thread-safe 아님), `videoHandlerYUV.cpp:89` `static unsigned char clp_buf[]`, 헤더 안의 non-const `static CodingEnum<...>` 테이블 약 8개 (TU마다 사본).

### 네임스페이스 위생
- 잘 됨: `parser::*`, `video::{yuv,rgb}`, `stats`, `decoder`, `FFmpeg`, `datasource`, `functions`
- 전부 전역: 모든 `playlistItem*`, `FileSource`, `MainWindow`, `SettingsDialog`, `SplitViewWidget`, `PlaylistTreeWidget` 등

### 임베드 가능성 평가

| 사용 방식 | 점수 | 이유 |
|---|---|---|
| `YUViewApplication app(argc,argv)` 통째로 | 8/10 | 한 줄이면 되지만 그건 그냥 YUView 앱임 |
| `MainWindow`를 위젯으로 임베드 | 3/10 | 앱 전역 이벤트필터 설치, VideoCache 소유, 플레이리스트 자동저장 |
| `playlistItem`/videoHandler 단독 재사용 | 2/10 | `QTreeWidgetItem` 상속, `ui_*.h`, `QSettings` 멤버 |
| **`parser::*` 단독 재사용** | **6/10** | 최선. `QTreeWidgetItem` 출력 계약 + `QGuiApplication` 필요, 윈도우/설정은 불필요 |
| **`stats::StatisticsData`** | **7/10** | 거의 순수 C++ |
| `decoder::*` headless | 5/10 | live `QCoreApplication` 필요, `decoderBase.h:38-39`가 videoHandler 헤더 → `ui_*.h` 를 끌고 옴 |
| 완전 headless | 1/10 | 라이브러리 `.pro`의 `QT += widgets opengl`이 무조건적 |

> `HACKING.md:159-162`의 프로젝트 목표: *"궁극적으로 Qt는 GUI/Widgets에만 의존하고 parser/decoder/testing은 Qt-free로 컴파일되어야 한다"* — 아직 희망사항. 그 방향의 유일한 흔적인 `dataSource/IDataSource.h:45-61`(순수 C++, 325 LOC)은 **YUViewLib 내 소비자가 0명**이고 유닛테스트만 씀.

## 5. 서브시스템별 LOC

`YUViewLib/src` 총 **98,543 LOC** / 540 파일 (294 `.h`, 246 `.cpp`) + `.ui` 25개. `YUViewApp` 52 LOC. 테스트 4,263 LOC.

```
parser/          323파일  38,354  38.9%   HEVC 9.7k / VVC 9.6k / AVC 6.0k / AV1 5.4k
                                          common 2.8k (SubByteReader, TreeItem, BitratePlotModel)
                                          Mpeg2 1.7k / Subtitles 1.3k / AVFormat 1.3k
video/            36     13,272  13.5%   yuv 5.8k / rgb 3.1k / root 2.4k / caching 1.9k
ui/               40     12,234  12.4%   ← 재사용 불가
decoder/          31      9,966  10.1%   실제 디코더 6.4k + externalHeader
playlistitem/     26      7,191   7.3%   playlistItemCompressedVideo.cpp 하나가 52KB
ffmpeg/           30      7,021   7.1%
statistics/       20      5,008   5.1%   ← 재사용성 최상
filesource/        8      1,964   2.0%
common/           15      1,798   1.8%
handler/           8      1,410   1.4%   앱 생명주기 글루
dataSource/        3        325   0.3%   신규, 미사용
```

- `ui/`+`handler/`+`playlistitem/` 제거 시 "재사용 코어" ≈ **78k LOC**
- "비트스트림 분석만" 슬라이스 (`parser/`+`filesource/`+`common/`) ≈ **42k LOC**

## 6. 언어 / Qt 요구사항

### C++20 — 필수 선언 (`YUViewLib.pro:5`, `YUViewApp.pro:5`, `YUViewUnitTest.pro:9` 모두 `CONFIG += c++20`)

실제로 쓰이는 C++20 기능은 **딱 두 가지**:
| 기능 | 사용 | 위치 |
|---|---|---|
| Designated initializers | **YES** | `video/rgb/ConversionFunctions.h:79,84`, `ConversionDifferenceRGB.cpp:147` |
| `constexpr` `std::find_if` | **YES** | `common/EnumMapper.h:44-68,100-113` |
| concepts / ranges / `<=>` / `std::span` / `consteval` | 없음 | 0건 |

C++17은 광범위: `std::optional`(58파일), `string_view`(25), `std::filesystem`(18), structured bindings.
→ **C++17로 백포트하는 것은 기술적으로 사소하다.** 단 그 순간 fork 확정.

### Qt 6 (CI는 6.9.0 고정)
`find_package(Qt...)` 없음(CMake가 없으므로). 라이브러리 요구 모듈:
```
QT += core gui widgets opengl xml concurrent network     # 전부 qtbase 안에 있음
```
qtdeclarative / qtmultimedia / qtsvg / qtcharts **불필요** → qtbase만 있으면 된다.

**최소 버전 선언이 소스 어디에도 없다.** CI로부터 추론: Qt 6.2 ~ 6.9.
- `.github/workflows/Build.yml:23` `apt-get install qt6-base-dev`
- `Build.yml:72,159` 커스텀 빌드 `qtBase-6-9-0-*.zip`
- 유일한 6.7 게이트: `common/TypedefQtDeprecated.h:37` (`QCheckBox::checkStateChanged` vs `stateChanged`) — `#else` 분기 있음 → **Qt 6.5로 내려도 컴파일된다**
- Qt5 폴백 분기가 아직 남아 있으나(`FileSource.cpp:111`, `videoHandlerYUV.cpp:2304` 등) CI 검증 없음. `QT += opengl` 의미가 Qt5/6에서 다르므로 (Qt6는 `QOpenGLWidget`이 `QtOpenGLWidgets`로 이동) `.pro`는 사실상 Qt6 전용

## 7. 권고

비트스트림 분석이 목표라면 **YUViewLib를 라이브러리로 소비하려 하지 말 것.** 대신:

1. `YUViewLib/src/{parser,filesource,common}` (~42k LOC)를 우리 CMake 타겟으로 vendor. `ui_*.h` 의존 없음 (검증 완료)
2. `QTreeWidgetItem` 출력 계약을 받아들이거나, `parser/common/TreeItem.h`를 직접 순회하는 shim 작성 (`TreeItem`이 진짜 내부 표현, `QTreeWidgetItem`은 경계에서만 생성)
3. YUView 헤더 include 전에 `Typedef.h`의 전역 매크로(`INT_MAX`, `INT_INVALID`)와 `::vector`/`::array`/`::Size` 를 인지하고 격리
4. 디코더는 `decoder::*`를 무시하고 libde265/dav1d/vvdec를 직접 링크. YUView의 부가가치는 *-internals 통계 추출뿐이고, 그건 **fork된** libde265/dav1d 빌드와 **자작** `libHMDecoder`/`libVTMDecoder` C API를 요구한다 (upstream에 없음)
5. **fork를 핀 고정할 것.** 안정 API도, 버전 정책도, deprecation 정책도 없다. upstream 리베이스마다 §3의 중앙 `if/else` 체인에서 충돌한다
