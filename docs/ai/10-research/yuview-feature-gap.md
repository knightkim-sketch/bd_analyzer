---
title: 기능 A~D 대비 YUView 갭 분석
status: active
created: 2026-07-27
updated: 2026-07-27
author: claude-opus-5
verified: yes
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 요약 매트릭스

| 기능 | 상태 | 난이도 | 성격 |
|---|---|---|---|
| **A. 비트스트림 분석** — 신택스 트리 / NAL·OBU / 비트레이트·HRD | ✅ 있음 (완성도 높음) | — | — |
| **A'. 블록 단위 오버레이** (예측/MV/파티션) | ⚠️ 있으나 **파서가 아닌 외부 디코더 라이브러리 산출**. AVC는 MV만 | 상 | **침습적** |
| **A''. slice_data / CTU / 매크로블록 신택스 파싱** | ❌ 없음 (전 코덱 헤더까지만) | 상 | 신규 |
| **A'''. raw `.obu` (AV1) 직접 분석** | ❌ 없음 | 하 | 추가적 |
| **B. YUV 분석** — 포맷/픽셀검사/diff/split view | ✅ 있음 (매우 강력) | — | — |
| **B'. PSNR** | ⚠️ diff 아이템의 텍스트 한 줄로만 존재. 누적/플롯/export 없음 | 중 | 추가적 |
| **B''. SSIM** | ❌ 없음 (0건) | 중 | 추가적 |
| **B'''. 히스토그램 / 웨이브폼 / 벡터스코프** | ❌ 없음 (0건) | 중 | 추가적 |
| **C. YUV 생성기** | ❌ 개념 자체가 없음 | 중 | **추가적** |
| **D. 이미지 필터** | ❌ 없음. 단 훅 지점은 깨끗함 | 중 | **추가적** |

> `YUViewLib.pro:9-12`가 `$$files(src/*.cpp, true)` 재귀 glob + `$$files(ui/*.ui, false)`
> → **새 소스/UI 파일 추가에 빌드 파일 수정이 전혀 필요 없다.** C/D 확장에 결정적으로 유리.

---

## A) 비트스트림 분석

### 파서 계층
| 클래스 | 위치 | 상위 |
|---|---|---|
| `parser::Parser` | `parser/Parser.h:57` | QObject |
| `parser::ParserAnnexB` | `parser/ParserAnnexB.h:56` | Parser |
| `ParserAnnexBAVC` | `parser/AVC/ParserAnnexBAVC.h:71` | ParserAnnexB |
| `ParserAnnexBHEVC` | `parser/HEVC/ParserAnnexBHEVC.h:59` | ParserAnnexB |
| `ParserAnnexBVVC` | `parser/VVC/ParserAnnexBVVC.h:81` | ParserAnnexB |
| `ParserAnnexBMpeg2` | `parser/Mpeg2/ParserAnnexBMpeg2.h:48` | ParserAnnexB |
| **`ParserAV1OBU`** | `parser/AV1/ParserAV1OBU.h:44` | **Parser** (AnnexB 아님) |
| `ParserAVFormat` | `parser/AVFormat/ParserAVFormat.h:50` | Parser |

스펙 신택스 함수 하나당 클래스 하나 (코덱당 ~60파일). AV1: `sequence_header_obu`, `uncompressed_header`, `frame_header_obu`, `tile_info`, `quantization_params`, `segmentation_params`, `cdef_params`, `lr_params`, `loop_filter_params`, `global_motion_params`, `film_grain_params`, `color_config`, `superres_params`. HEVC는 `Extensions/`(multilayer/3D/SCC/range) + SEI 10종. VVC는 APS/ALF/LMCS/picture-header.

### ⚠️ 파싱 깊이 한계 — 전 코덱이 슬라이스/프레임 **헤더까지만**
- `parser/AVC/slice_rbsp.cpp:53` — `// slice_data( )` 주석일 뿐, 미구현
- `parser/AV1/frame_obu.cpp:62` — `//tile_group_obu( sz );` 주석 처리됨
- `parser/AV1/ParserAV1OBU.cpp:72-108` — `OBU_TEMPORAL_DELIMITER`/`SEQUENCE_HEADER`/`FRAME_HEADER`/`FRAME`만 처리. `OBU_TILE_GROUP`, `OBU_METADATA`, `OBU_TILE_LIST` 미처리
- HEVC/VVC `slice_segment_layer_rbsp.cpp`(60줄), `slice_layer_rbsp.cpp`(56줄) — 헤더 전용

### 신택스 트리 표현
- `parser/common/TreeItem.h:41` — shared_ptr 트리, 5컬럼 **name / value / coding / code / meaning** (`getData()` `:114`) + `error` 플래그 + `streamIndex`
- Qt 어댑터 `parser/common/PacketItemModel.h:40` (`QAbstractItemModel`) + `:75` `FilterByStreamIndexProxyModel`, 5컬럼 하드코딩(`:54`)
- UI `ui/widgets/BitstreamAnalysisWidget.h:34`, 탭 4개 (`ui/bitstreamAnalysisWidget.ui:22,46,115,156`): **Stream Info / Packet Analysis / Bitrate Plot / HRD**
- QtConcurrent 백그라운드 파싱 (`BitstreamAnalysisWidget.cpp:255`), 진행률, 취소, **500프레임 제한** (`Parser.h:50` `PARSER_FILE_FRAME_NR_LIMIT`)
- 파서 생성 switch: `BitstreamAnalysisWidget.cpp:261-272` — **raw AV1 OBU 분기 없음.** `InputFormat` enum(`filesource/FileSource.h:48-62`)에 `AnnexBHEVC/AnnexBAVC/AnnexBVVC/Libav` 4개뿐. `ParserAV1OBU::runParsingOfFile`은 `assert(false)` (`ParserAV1OBU.h:58-62`)

### 비트 위치 추적
- `parser/common/SubByteReader.h:47` — `posInBufferBytes`, `posInBufferBits`, `numEmuPrevZeroBytes`, `nrBitsRead()`, `byte_aligned()`, `more_rbsp_data()`. emulation prevention 제거 내장
- `parser/common/SubByteReaderLogging.h:53` — 모든 read를 감싸 TreeItem 생성, `code` 컬럼에 **실제 비트 문자열**, coding 타입 `u(v)/ue(v)/se(v)/leb128(v)/ns(n)/su(n)` (`SubByteReaderLogging.cpp:44,188-301`)
- ⚠️ **신택스 요소별 절대 비트 오프셋은 저장하지 않는다.** 코드 문자열 길이와 리더의 진행 위치만. NAL 단위 바이트 위치는 있음 (`ParserAnnexB.cpp:87-103`: Start code size / Payload size / Start·End pos)
- 검증 기능: `reader::Options`의 `withCheckEqualTo/Greater/Smaller/Range`, `withMeaning*` (`SubByteReaderLoggingOptions.h:71-105`) — 위반 시 트리 아이템 착색

### 비트 코스트 통계
- `parser/common/BitratePlotModel.h:41` — AU별 `BitrateEntry{dts,pts,duration,bitrate,keyframe,frameType}`, decode/display 순 정렬, 러닝 평균, 스트림별
- `parser/common/HRDPlotModel.h` + `AVC/HRD.cpp` — CPB fullness 플롯
- ❌ **신택스 요소별/카테고리별 비트 회계 없음** ("MV vs residual vs header 비율"). slice_data 파서를 직접 만들어야 나온다

### 블록 단위 통계 — 파서가 아니라 디코더가 만든다
`grep -rn "Stat" parser/` → **0건.** 통계 파이프라인은 전적으로 디코더 주도:
- `decoder/decoderBase.h:128-132,153,175` — `enableStatisticsRetrieval(stats::StatisticsData*)`, `fillStatisticList()`, `internalsSupported`
- `statistics/StatisticsData.h:46` — `map<typeID, FrameTypeData>`; `statistics/FrameTypeData.h:68-108` — `StatsItemValue`, `StatsItemVector`, `StatsItemAffineTF`, `StatsItemPolygonValue/Vector`
- 렌더링 `stats::paintStatisticsData()` (`playlistItemCompressedVideo.cpp:597` 에서 호출), 벡터 화살표 `StatisticsDataPainting.cpp:95,763-791`

생산자별 커버리지:
| 코덱 | 디코더 | 제공 통계 |
|---|---|---|
| **AV1** | `decoderDav1d.cpp:586-780` (채움 `:923-1010`) | **~24종** — pred mode, seg id, skip, intra pred mode luma/chroma, palette size, angle delta, CfL alpha, ref frame 0/1, compound type, wedge idx, inter mode, DRL idx, interintra, motion mode, **MV 0/1**, transform depth |
| **HEVC** | `decoderLibde265.cpp:540-610`, `decoderHM` | CTB/CB/PB/TU 트리, MV, ref POC, intra dir |
| **VVC** | `decoderVTM` | 있음 / `decoderVVDec`는 `fillStatisticList` 없음 |
| **H.264** | `decoderFFmpeg.cpp:430-439,279-296,474` | **4종만** — Source ±, Motion Vector ± (`AV_FRAME_DATA_MOTION_VECTORS`, `flags2 +export_mvs`) |

> ❗ **H.264 파티션/intra-mode/CU-tree 오버레이는 존재하지 않는다.** 그리고 AV1/HEVC의 풍부한 통계는 **YUView가 fork한 dav1d/libde265 빌드**를 요구한다 (upstream 바이너리 불가). 이게 A 기능의 최대 리스크.

파일에서 통계 import도 가능: `playlistItemStatisticsFile` + `StatisticsFileCSV` / `StatisticsFileVTMBMS`.

---

## B) YUV 분석

### 있는 것 (매우 충실)
- **YUV 포맷** (`video/yuv/PixelFormatYUV.h`): subsampling 444/422/420/440/410/411/400(`:160-167`); packed order YUV/YVU/AYUV/YUVA/VUYA/UYVY/VYUY/YUYV/YVYU(`:137-146`); plane order(`:182`); 임의 `bitsPerSample` + endianness; chroma siting(`:255`); planar-interleaved; V210(`:116`). 파일명/크기 기반 추론 `PixelFormatYUVGuess.cpp`, 커스텀 포맷 다이얼로그
- **RGB 경로 대칭**: `video/rgb/PixelFormatRGB.h` + CMYK/Bayer 확장 (`playlistItemRawFile.cpp:102-112`)
- **색공간 변환**: BT.601/709/2020 × limited/full (`PixelFormatYUV.h:71-77`), chroma interpolation(`:89`)
- **컴포넌트별 MathParameters{scale, offset, invert}** (`PixelFormatYUV.h:94-107`, `ConversionSettings` `videoHandlerYUV.h:66-74`) — **현존하는 유일한 픽셀 변환 메커니즘**
- **픽셀 검사**: `FrameHandler::getPixelValues`(`video/FrameHandler.cpp:473`), `videoHandlerYUV::getPixelValues`(`:2696`), 고배율 raw 값 오버프린트 `drawPixelValues`(`videoHandlerYUV.cpp:2815`), zoom box(`ui/views/SplitViewWidget.h:239-262`)
- **차분**: `playlistItemDifference`(`:38`) + `videoHandlerDifference`(`:46`) — `amplificationFactor`, `markDifference`, **`reportFirstDifferencePosition`** (CTU 순 계층 스캔으로 첫 차이 블록 보고, `:81-95`). YUV 도메인 네이티브 diff `videoHandlerYUV.cpp:3531`
- **비교 UI**: split view DISABLED/SIDE_BY_SIDE/COMPARISON(`SplitViewWidget.h:140-163`), 연동 pan/zoom, 별도 창, overlay 아이템, resample 아이템
- 캐싱: `video/caching/VideoCache.cpp`, LoadingThread/Worker, 더블 버퍼링

### 없는 것
- **SSIM** — 0건
- **PSNR** — `video/yuv/videoHandlerYUV.cpp:119-126` `formatMSEandPSNR()`, `:3781-3794`에서 Y/U/V/Avg 출력. 저장소 전체 `psnr` 매치가 7건. **diff 아이템의 텍스트 정보 행일 뿐, 프레임별 누적/플롯/CSV export 없음.** 그릴 때마다 일회성 재계산
- **히스토그램 / 웨이브폼 / 벡터스코프 / RGB parade** — 0건 (`decoderTarga.cpp:468` 주석 하나가 유일한 매치)
- 플레인 단독 보기(`ComponentDisplayMode`)는 있으나 통계 스코프는 없음

> 재사용 가능한 기반: `ui/views/PlotModel.h` + `PlotViewWidget`, `BitstreamAnalysisWidget`의 dockable 위젯 패턴.

---

## C) YUV 생성기 — **개념 자체가 부재**

`testpattern|colou?rbar|generator|synthes|gradient|perlin|noise` 전수 검색 → 전부 오탐 (`ColorMapper` 그라디언트, ffmpeg `noise_reduction` 필드, AV1 `film_grain_params`).

베낄 수 있는 선례:
- **`playlistItemText`** (`playlistitem/playlistItemText.h:43`) — **파일 없는 완전 합성 아이템.** `drawItem()`에서 직접 렌더 (`playlistItemText.cpp:206`). videoHandler도 프레임 수도 없으므로 *시퀀스* 생성기의 베이스로는 부족하지만, **"파일 없는 아이템" 패턴이 지원됨을 증명**한다 (생성 `PlaylistTreeWidget.cpp:298`, 영속화 태그 `playlistItems.cpp:247`)
- **`playlistItemResample`** (`:40`) — `maxItemCount=1`인 `playlistItemContainer`(`playlistItemResample.cpp:56`) + `videoHandlerResample`. **생성 비디오 아이템의 올바른 구조 모델**: `playlistItem` + 파일을 읽는 대신 `currentFrameRawData`/`currentImage`를 합성해 채우는 `videoHandler` 서브클래스
- `playlistItemImageFileSequence` — 디스크의 번호 매겨진 파일을 읽음. 생성기가 아님

⚠️ **YUV 쓰기 경로도 없다** (writer 부재). 생성기 출력 저장은 신규 구현 필요.

---

## D) 이미지 필터 — 없음, 그러나 훅이 깨끗함

`\bfilter\b|convolution|sharpen|denoise|blur` 검색(`src/video`, `src/playlistitem`) → `QFileDialog` name filter 문자열뿐.

### 가장 가까운 이웃
- **`videoHandlerResample`** (`video/videoHandlerResample.h:45`) — **이미 사실상 일회용 이미지 필터**다. 자식 `FrameHandler`에서 `getCurrentFrameAsImage().scaled(...)` 해서 결과 저장 (`videoHandlerResample.cpp:87-110`). 프레임 인덱스 리매핑(`mapFrameIndex` `:184`)으로 cut/subsample도 함.
  → **이 클래스를 일반화하면 그게 D다.**
- `videoHandlerDifference` — 동일한 2-입력 패턴
- YUV Math (`MathParameters`) — `convertYUVToImage` 안에서 적용되는 퇴화된 점 필터

### 프레임 변환 훅 지점

| 훅 | 위치 | 비고 |
|---|---|---|
| **최선/권장** | `video/videoHandlerResample.cpp:87` (특히 `:103-104`) | 이미 "자식에서 pull → QImage 변환 → publish" 패턴. `videoHandlerFilter`로 복제 |
| 베이스 로드 | `video/videoHandler.cpp:334` `loadFrame` (선언 `videoHandler.h:117`, virtual) | 임의 핸들러의 오버라이드 지점 |
| **YUV 도메인 변환** (RGB 변환 전) | `video/yuv/videoHandlerYUV.cpp:3174` — `loadRawYUVData()`(`:3185`)와 `convertYUVToImage()`(`:3192`) 사이 | **네이티브 비트뎁스의 진짜 YUV 샘플을 필터할 수 있는 유일한 지점** |
| 캐시 경로 쌍둥이 (위와 반드시 동시 수정) | `videoHandlerYUV.cpp:3214` `loadFrameForCaching` (`convertYUVToImage` `:3237`), 베이스 `videoHandler.cpp:368` | 워커 스레드에서 실행. 빠뜨리면 라이브 뷰는 필터, 캐시 뷰는 원본 |
| 변환 코어 | `videoHandlerYUV.cpp:2271` `convertYUVToImage()` | 자유 함수. `ConversionSettings`(`videoHandlerYUV.h:66`) 확장으로 값싼 점 연산 추가 가능 |
| 그리기 시점 | `video/videoHandler.cpp:178` `drawFrame` (`:214` paint) | **비권장** — 캐시와 픽셀 검사값을 우회 |
| **before/after 비교 — 이미 공짜** | `ui/views/SplitViewWidget.h:140-163` | 원본 아이템을 왼쪽, 필터 아이템을 오른쪽에. 연동 zoom/pan + zoom box 무상 획득 |

---

## C·D 구현 시 손대야 할 5곳

| # | 파일 | 변경 | 성격 |
|---|---|---|---|
| 1 | **신규** `video/videoHandlerFilter.{h,cpp}`, `video/videoHandlerGenerator.{h,cpp}`, `playlistitem/playlistItem{Filter,Generator}.*`, `ui/playlistItem{Filter,Generator}.ui` | `videoHandlerResample`/`playlistItemResample` 복제 후 `.scaled()`를 필터 커널로 교체. 생성기는 `videoHandler` 서브클래스가 `currentFrameRawData`를 합성 | **추가적.** qmake glob이 자동 인식 → **빌드 파일 수정 0** |
| 2 | `playlistitem/playlistItems.cpp:218-289` (`loadPlaylistItem`) | `else if (tag == "playlistItemFilter"/"playlistItemGenerator")` 2개 추가. 필터는 Resample처럼 `parseChildren = true`(`:269-273`) | 추가적, ~10줄 |
| 3 | `ui/widgets/PlaylistTreeWidget.cpp:350` (`addResampleItem` 템플릿) + `:451-454` (컨텍스트 메뉴) + `PlaylistTreeWidget.h:109-112` | `addFilterItem()`/`addGeneratorItem()` 슬롯 + `menu.addAction()` 2줄 | 추가적, ~50줄 복붙 |
| 4 | `ui/Mainwindow.cpp:275-281` | `addActionToMenu(fileMenu, "&Add Filter"/"&Add Generator", ...)` 2줄 | 추가적, 2줄 |
| 5 | `images/images.qrc` + 신규 PNG. YUV 도메인 필터링 시 추가로 `videoHandlerYUV.cpp:3174` + `:3214` + `ConversionSettings`(`videoHandlerYUV.h:66-74`) | 아이콘 패턴은 `playlistItemResample.cpp:51` `functionsGui::convertIcon(":img_resample.png")`. YUV 도메인 훅은 3900줄 파일의 대칭 호출 2곳 + `currentFrameRawData`/`currentImageSetMutex` 스레드 안전성 | 아이콘 추가적 / YUV 훅 **경미하게 침습적** (클래스 재작성은 없음) |

## 종합 판단

- **C와 D는 추가적(additive)이다.** YUView 아키텍처가 이미 "무에서 프레임을 만드는 아이템"(`playlistItemText`)과 "자식 프레임을 변환하는 아이템"(`playlistItemResample`)을 지원한다. 코어 클래스 재작성 불필요
- **B의 스코프/메트릭도 추가적**이다. `PlotModel`/`PlotViewWidget`과 dockable 위젯 패턴을 그대로 재사용
- **전체 스펙에서 유일하게 진짜 침습적인 작업은 A의 블록 레이어**다: `parser/AVC|HEVC|AV1`에 실제 slice_data/tile_group 파싱을 추가하고 이를 `stats::StatisticsData`로 라우팅하는 것. 오늘 그 데이터 경로(`decoderBase.h:130`)는 **디코더 소유**이고 **parser→statistics 연결선이 아예 없다**
