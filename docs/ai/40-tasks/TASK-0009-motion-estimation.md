---
title: TASK-0009 Motion Estimation 분석 기능
status: implemented (1차 범위)
created: 2026-09-03
updated: 2026-09-06
author: claude-opus-5
verified: "빌드·회귀 30/30·헤드리스 프로브(오버레이 픽셀 카운트, 클릭 질의) 확인. 실제 X 화면 렌더링은 미검증"
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
patches: 0026, 0027, 0028, 0029, 0030
confluence: IT space — "Motion Estimation 분석 기능" 하위 3페이지 (알고리즘 정리)
---

## 배경

표시 중인 raw YUV 프레임을 current 로, frame interval 만큼 떨어진 프레임을 reference 로
삼아 ME 를 돌리고, 그 결과를 기존 통계 오버레이 위에 그린다. 목적은 재생이 아니라
**인코더가 실제로 찾은 MV 와 우리가 예상한 MV 를 같은 화면에서 비교**하는 것이다.

알고리즘 상세는 리포가 아니라 Confluence 에 있다 (착수 전 조사·교차검토 결과):

| 페이지 | 내용 |
|---|---|
| [Motion Estimation 분석 기능](https://blue-dot.atlassian.net/wiki/spaces/IT/pages/4521033823/Motion+Estimation) | 상위 페이지, 설계와 범위 |
| [SVT-AV1 Motion Estimation (v4.2.0)](https://blue-dot.atlassian.net/wiki/spaces/IT/pages/4520345744/SVT-AV1+Motion+Estimation+v4.2.0) | HME 3단 캐스케이드, check_00_center, static bypass |
| [Odyssey Open-loop ME (lookahead)](https://blue-dot.atlassian.net/wiki/spaces/IT/pages/4519919848/Odyssey+Open-loop+ME+lookahead) | meanpool half → center → bilinear upscale → VBS |
| [Odyssey Closed-loop ME (encoder)](https://blue-dot.atlassian.net/wiki/spaces/IT/pages/4520083648/Odyssey+Closed-loop+ME+encoder) | 2차 범위. 조사만 완료 |

이 문서는 **구현** 기록이다. 알고리즘을 다시 적지 않고, 코드 구조·통합 지점·실제로
발생한 버그·미해결 항목만 남긴다.

관련 커밋 (bd_analyzer `main`, 오래된 순):

```
0f31a95  Add the Qt-free motion estimation core under src/me
d94d353  Reproduce SVT-AV1 integer ME, with the HME cascade and the static bypass
03fd69c  Reproduce the odyssey open-loop ME, half-picture centre and upscaled VBS
f1fb664  Feed ME results into the existing statistics overlay, and build src/ into the lib
cec0acc  Make a running estimate cancellable, checked between superblocks
8f45a6b  Take two raw YUV frames to a frame of ME results, off the GUI thread
a3626ab  Add the Motion Estimation panel, running in a cancellable background estimate
80344ea  Show the ME overlay controls before the first result, and stop asking about the cache
8a7e066  Fix the abort on quit, and give the ME dock a menu entry
53e6d19  Make the ME overlay visible, survive an item switch, and report progress
03556d2  Say on the progress bar why frame 0 has no estimate
9fc3c9d  Show a clicked block's reproduced motion vector in the Block Info pane
```

---

## A. 구조

두 층으로 나눴다. 경계는 **Qt 의존성**이다.

```
src/me/            Qt 를 전혀 모른다. 순수 C++.
  MeTypes.h          MotionVector(1/8-pel), BlockSize, MeParams, CancelToken, ProgressToken
  MePlane.{h,cpp}    padded 8-bit plane, SVT 피라미드, odyssey meanpool/bilinear
  MeCost.{h,cpp}     SAD/SSE, odyssey rate LUT 항, common metric, search range 클램프
  SvtIntegerMe       SVT-AV1 정수 ME
  OdysseyOpenLoopMe  odyssey open-loop ME
  MeEstimatorFactory

src/integration/   양쪽을 다 아는 유일한 층.
  MeFrameSource      raw YUV 바이트 → luma plane (deeper source 는 shift down)
  MeRunner           MeRunRequest → MeFrameResult. 워커 스레드에서 호출 가능
  MeStatisticsAdapter  결과 → stats::StatisticsData 타입/데이터, 클릭 질의 행 병합
  MotionEstimationWidget  ME dock 패널
```

`src/` 는 패치 `0026` 이 `YUViewLib.pro` 에 `src/me/*.cpp`, `src/integration/*.cpp` 를
글롭으로 추가해 라이브러리에 함께 빌드된다. upstream 트리에 우리 코드를 넣지 않는다는
[ADR-0001](../20-decisions/ADR-0001-fork-vs-library.md) 원칙을 유지하기 위한 배치다.

### 왜 별도 MV drawer 를 만들지 않았나

upstream 오버레이가 이미 벡터를 그린다 (AV1 MV 가 타입 24/25). 화살촉, 줌 스케일링,
타입별 색/선굵기, 표시 토글, opacity, uiGroup 묶기, 스타일 대화상자, CSV export 가 전부
딸려 온다. 두 번째 드로어를 만들면 이걸 다시 구현해야 하고, **비교가 목적인 기능인데
줌·스타일·그리기 순서에서 두 오버레이가 어긋난다.** 그래서 ME 결과를 평범한 통계 타입으로
등록한다. 타입 ID 는 `kMeStatTypeBase = 200` 부터 (디코더 타입과 충돌 없음).

타입은 (algorithm, block size) 마다 3개:

| 종류 | 내용 | 기본 render |
|---|---|---|
| vector | MV. `vectorScale = 8` (내부가 1/8-pel) | **on** |
| native cost | 그 estimator 가 실제로 최소화한 값 (SVT=SAD, odyssey=SSE+rate²) | off |
| common SAD | 원본끼리의 순수 SAD. **알고리즘 간 비교는 이 열로만** | off |

패널의 block size 체크박스가 **어떤 타입이 존재하는지를 결정하는 유일한 스위치**다.
같은 선택을 두 군데 두면 어느 쪽이 이기는지 알 수 없다.

### raw item 에 통계 컨테이너

upstream 은 compressed / statistics-file item 에만 `StatisticsData` 를 준다. raw YUV 에는
비트스트림이 없으니 당연한 설계인데, ME 오버레이는 그게 필요하다. 패치 `0028` 이
`playlistItemRawFile` 에 `StatisticsData` + `StatisticUIHandler` 쌍을 붙였다 —
`playlistItemCompressedVideo` 가 소유하는 방식 그대로.

---

## B. 실행 모델

- 체크박스 **on = 자동 + 취소 가능한 백그라운드 실행**, off = 아무것도 하지 않음.
- `QtConcurrent::run` + `QFutureWatcher`. 프레임/파라미터/선택이 바뀌면 진행 중인 것을
  취소하고 새로 시작한다.
- **취소 토큰은 run 마다 새로 만든다** (`shared_ptr`, 워커가 값 캡처로 살려 둠).
  하나를 재사용하며 cancel→reset 하면, 아직 플래그를 안 본 워커가 되살아난다.
- 진행률은 push 가 아니라 **polling** 이다. estimator 가 superblock 단위로 atomic 에
  세고 (`ProgressToken`), MainWindow 가 100 ms 타이머로 읽는다. src/me 가 Qt 를 모르게
  두기 위한 선택.
- 취소 체크와 진행 카운트는 둘 다 **superblock 경계**에서 한다. 프레임 전환이 즉각
  느껴질 만큼 촘촘하고, superblock 하나치 SAD 옆에서는 보이지 않을 만큼 싸다.

---

## C. 실제로 발생한 버그 (전부 회귀 테스트로 고정)

### C-1. 종료 시 `free(): invalid pointer` — 패치 `0029`

`FrameInfoWidget` 이 block statistics 섹션을 **값 멤버**로 들고 있으면서
`VideoCacheInfoWidget::addSectionBelowBlockInfo()` 에 넘겼다. `QWidget::setParent()` 는
소유권을 넘기므로 Qt 가 자식으로 delete 하고 `~FrameInfoWidget` 이 멤버로 또 파괴한다.
→ 세 위젯을 힙 할당해 Qt 부모 체인이 단독 소유.

ME 작업이 아니라 패치 `0023`(다른 세션) 에서 들어온 것이고, 헤더 주석
"owned by this widget whether or not it was re-parented" 가 사실과 반대였다.
**다른 MainWindow 테스트가 창을 일부러 leak 해서 `~MainWindow` 가 한 번도 돌지 않았다** —
그래서 회귀 스위트를 그대로 통과했다. → `25-mainwindow-teardown` 이 창을 실제로 파괴한다.

### C-2. ME dock 이 보이지 않음

`mainwindow.ui` 에 `visible=false` 로 있는데 View → Dock Panels 에 등록하지 않았다.
띄울 방법이 아예 없었다. → `Ctrl+M` 액션 추가. 테스트는 **모든** dock 이 메뉴에서
켜지는지 확인한다.

### C-3. item 전환 시 assert abort — 패치 `0030`

```
StatisticUIHandler.cpp: Assertion `spacerItems[0] != nullptr' failed
```

통계 컨트롤은 properties widget 이 처음 요청될 때 lazy 생성된다. ME 는 item 이 선택되는
순간 타입을 등록하고 `updateStatisticsHandlerControls()` 를 부른다. 컨트롤이 아직 없으면
row 수 불일치 → 재생성 branch → 달아 본 적 없는 spacer 제거 → assert.
→ `ui.created()` 가 false 면 early return.

**GUI 클릭 순서로는 offscreen 재현에 실패했다.** `loadFiles` 가 properties widget 을
만들어 버려서다. API 레벨 (properties 를 요청한 적 없는 item + 타입 등록) 에서는
결정적으로 재현되며, 그 형태로 `26-me-overlay-item-switch` 에 고정했다.

### C-4. MV 선이 안 보임 — 두 원인이 겹침

1. `scaleVectorToZoom = true` 로 등록했는데 painter 가 이걸
   `width * zoomFactor / 8` 로 읽는다. 1:1 배율에서 2px → **0.25px**. 그려지긴 하는데
   안티에일리어싱으로 사라진다. → 디코더가 비트스트림 MV 를 등록하는 방식과 동일하게
   (width 2, 줌 스케일 없음) 맞춤.
2. MV / native cost / common SAD 세 종류가 **같은 `uiGroup`** 이었다. 행 하나의
   체크박스가 그 그룹 전체의 `render` 를 세팅하므로, "MV 보기" 를 켜면 블록마다 색칠된
   사각형을 그리는 cost 오버레이 둘이 같이 켜져 화살표를 덮었다. → 종류별로 행 분리.

검증: 흰 배경에 오버레이만 그린 뒤 초록 픽셀 카운트 44 → **809**.

### C-5. "No estimate yet" 에서 멈춤 — 버그가 아니라 경계

`meFrameIdx = 0` 은 정상이었고 **reference = 0 − 1 = −1** 이라 거절된 것이다. 파일을
열면 항상 frame 0 에서 시작하고 interval 기본값은 +1 이므로 **기본 상태에서 항상**
이 상태가 된다. 알고리즘상 맞는 동작이다 (첫 프레임은 참조가 없다).

문제는 전달이었다. 사유가 progress bar 가 아니라 아래쪽 라벨에, 그것도
`frame -1 is outside the sequence` 라는 **reference 인덱스**로 표시됐다. 화면에 frame −1
은 없으니 읽어도 의미가 없다. → bar 가 직접 말하도록 (`Frame 0: no reference`),
메시지는 보고 있는 프레임 기준으로 재작성.

**경계에서 자동으로 부호를 뒤집지 않았다.** 부호 있는 interval 은 명시적으로 합의된
사양이고, 사용자가 넣은 값을 조용히 바꾸는 쪽이 더 나쁘다.

### C-6. 그 밖에 고친 것

- `videoHandlerYUV::getFormatAsString()` 의 빈 `std::optional` 역참조 → `test.yuv`
  같은 이름에 해상도가 없는 파일을 열면 `bad_alloc` abort (패치 `0024`).
- `loadAutosavedPlaylist()` 가 파일 경로 자리에 디렉토리를 넘겨 복원이 한 번도 동작한
  적이 없었다 (패치 `0025`).
- `StatisticsData::setFrameIndex()` 가 `accessMutex` 를 스스로 잠근다. 이미 잡은 채로
  부르면 **비재귀 뮤텍스 자기 교착**. 락을 걷어내고, 대신 `setFrameIndex()` 가 프레임
  캐시를 비우므로 **채우기 전에** 부르도록 순서를 고정했다.

---

## D. 개발 중 스스로 만든 오류 (재발 방지용)

| 증상 | 원인 |
|---|---|
| 모든 MV 가 (0,0) 으로 붕괴 | odyssey VBS rate 항에 step-3 형태(pmv 상대, 가중, /6, `<<6`)를 씀. 실제는 **center 상대, 무가중, /8, 제곱, 8x8 별 합산** |
| 마지막 superblock 행에서 OOB 읽기 | 피라미드 padding 부족. 인코더는 픽처를 superblock 격자에 정렬해 우회한다 → `kHmePyramidPad = 64` |
| 테스트 24 행(hang) | `setFrameIndex()` 자기 교착 (C-6) |
| 테스트가 통과하는데 화면은 틀림 | 등록·저장·**렌더링**은 서로 다른 것. 픽셀을 세는 검사를 넣기 전까지 C-4 를 못 잡았다 |
| 진단이 헛돎 | `cc.sh` 가 `libYUViewLib.a` 를 **정적 링크**한다. 재빌드 후 진단 바이너리를 다시 컴파일하지 않으면 옛 코드를 관측한다 |
| 성공한 빌드가 컴파일 실패를 가림 | stale `.o`. 내 변경을 탓하기 전에 clean rebuild 부터 |

---

## E. 테스트

Qt 없이 도는 단위 테스트 3개 + MainWindow 를 구동하는 회귀 5개.

| 테스트 | 무엇을 막는가 |
|---|---|
| `unit/me-plane-and-cost` | plane padding, 다운/업스케일, cost·rate 항 |
| `unit/me-svt-integer` | HME 캐스케이드, check_00_center, static bypass |
| `unit/me-odyssey-openloop` | half 중심 결정 → bilinear upscale → VBS |
| `22-me-statistics-overlay` | 타입 등록/제거, vectorScale 8, 디코더 타입과 비충돌 |
| `23-me-runner` | 프레임 범위 규칙(부호 포함), 10bit shift-down, 실패 사유, 취소 |
| `24-me-panel` | 패널 컨트롤 → 파라미터, 백그라운드 실행, progress bar, frame 0 경계 |
| `26-me-overlay-item-switch` | C-3 abort, C-4 두 원인, **화살표가 실제로 칠해지는지** |
| `27-me-block-info-on-click` | 클릭 → Block Info 행, 체크된 크기만, 크기당 1행, lag 가드 |
| `25-mainwindow-teardown` | C-1 이중 해제, 모든 dock 의 메뉴 항목 |

`26`/`27` 은 자기 클립(패닝 텍스처)을 만든다. **정지 패턴으로는 프레임 전체에서 non-zero
MV 가 하나뿐**이라 "화살표가 보인다" 를 걸기에 약하다.

현재: **30/30 PASS**.

---

## F. 남은 것

| 항목 | 상태 |
|---|---|
| 2차: compressed stream 지원 | 미착수. reference 프레임을 디코딩해야 한다. **이게 되면 비트스트림의 실제 MV 와 재현 MV 를 겹쳐 볼 수 있다 — 이 기능의 최종 목적** |
| odyssey closed-loop ME | 조사 완료(Confluence), 구현 미착수. open-loop 결과를 center 로 받고 recon 참조가 필요 |
| bi-prediction | 미착수. `RefList` 로 자료구조만 열어 둠 |
| `ods_me_construct_candidates` | 복사가 아니라 **모델링**이다. HW 후보 순서는 고정 8x8=64 로 재현했으나 동치 증명은 없음 |
| `mv_rate_weight` | **추정값이다.** odyssey 소스에서 확정하지 못했다 |
| playlist `absolutePath` | `file://` URL 로 저장돼 매칭되지 않는다. 미수정, 기록만 |
| 블록 하이라이트 렌더링 | 코드 경로상 동작해야 하나 **화면 확인 안 함** |
| 실제 X 화면 검증 전반 | 전부 offscreen 검증이다 |
