---
title: SB 단위 BD-rate 설계
status: implemented (1-4단계). 5단계 미착수
created: 2026-09-08
updated: 2026-09-11
author: claude-opus-5
verified: "단위 45항목 + 회귀 31, 34. 축 눈금은 오프스크린 렌더 이미지를 눈으로 확인 (겹침·성김 2건 수정)"
related: TASK-0007 (sb_bitcount), 패치 0023 (org YUV SSE / RD plot), TASK-0009 (백그라운드 실행 패턴)
---

## 무엇을 만드는가

여러 stream 을 그룹으로 묶어 **superblock 단위 rate–distortion 곡선**을 만들고, 그룹 간
BD-rate 를 계산해 보여준다. 목적은 "어느 superblock 에서 손해를 봤는지" 를 짚는 것이다.

사용 흐름 (사용자 합의):

1. playlist 에서 stream n 개를 multi-select 한다. y4m/yuv 가 섞여 있으면 그것을 org 로 쓴다.
2. **SB BD-rate** 버튼을 누르면 그 선택이 **곡선 하나(= 그룹 하나)** 로 추가된다.
   그룹 이름은 자동 생성하고, 사용자가 바꿀 수 있다.
3. 다른 선택으로 다시 누르면 두 번째 그룹이 곡선으로 추가된다.
4. **처음 만든 그룹이 anchor** 이고, 나중에 다른 그룹으로 바꿀 수 있다.
5. popup window 에 그래프, 하단에 수치 table. 체크박스 **SB / Frame / Sequence** 로
   좌 / 중 / 우 패널을 켜고 끈다. 체크를 끄면 그 곡선은 사라진다.

| 패널 | 대상 | 비용 |
|---|---|---|
| **SB** | 현재 프레임의 **active SB** (클릭한 superblock) | 싸다. 프레임당 이미 수집되는 값 |
| **Frame** | 현재 프레임 전체 (bits 합, SSE 합 → PSNR) | 싸다 |
| **Sequence** | 전 프레임 누적 | 스트림 수 × 프레임 수 만큼 디코딩. **실측 결과 감당 가능** (아래) |

---

## 이미 있는 것

새로 만들 것이 생각보다 적다. 재료가 전부 있다.

| 필요한 것 | 이미 있는 것 | 위치 |
|---|---|---|
| SB 당 bits | `getSuperblockBits(frameIdx)` — `sb_bitcount` 를 SB 격자로 접어 반환 | `playlistItemCompressedVideo.cpp:1325` |
| SB 당 SSE | `getPixelBlockStats(pos, frameIdx)->sse` — org YUV 대비 luma SSE | `PixelStatistics.h` |
| SB 격자 정렬 | `getPixelStatisticsBlockSize()` 가 `getDefaultGridSize()`(= AV1 superblock) 를 따른다 | `playlistItemWithVideo.cpp:340` |
| org 첨부 | `setOriginalYUVSource(path, &error)` — 크기·프레임당 바이트 검증, y4m 인덱싱 포함 | 패치 `0032` |
| RD 산점도 | `RateDistortionPlotWidget` — SB 당 (bits, SSE) 를 이미 그린다 | 패치 `0023` |
| 취소 가능한 백그라운드 실행 | ME 의 `CancelToken` / `ProgressToken` / `QFutureWatcher` 패턴 | `src/me`, `src/integration` |
| 프레임별 SSE 캐시 | `pixelStatsCache` (`.bd_analyzer` 사이드카) | `playlistItemWithVideo.h:224` |

즉 **SB 하나에 대한 (bits, SSE) 는 이미 계산된다.** 새로 만드는 것은 (1) 그룹 모델,
(2) 여러 스트림·여러 프레임에 대한 수집 오케스트레이션, (3) PSNR 과 BD-rate 수식,
(4) popup UI 다.

---

## 막히는 지점 (착수 전 확인 필요한 것)

### B-1. `getSelectedItems()` 가 2개로 잘린다

```cpp
std::array<playlistItem *, 2> PlaylistTreeWidget::getSelectedItems() const
```

split view 가 최대 2개를 나란히 놓기 때문에 만들어진 API 다. Qt 의
`QTreeWidget::selectedItems()` 는 전부 돌려주므로, **n 개용 접근자를 새로 추가**한다
(기존 것은 split view 가 계속 쓴다).

### B-2. `sb_bitcount` 는 dav1d analyzer 디코더에서만 나온다

FFmpeg 폴백으로 열린 스트림은 bits 가 없다. 그룹에 그런 스트림이 있으면 **곡선을 그리지 않고
이유를 말해야** 한다 (조용히 0 으로 두면 BD-rate 가 거짓이 된다).

### B-3. 통계 수집이 켜져 있어야 한다

`sb_bitcount` 는 `setBlockInfoRequested(true)` 이거나 오버레이가 켜져 있을 때만 수집된다.
수집 대상은 **선택된 항목만이 아니라 그룹의 모든 스트림**이므로, 수집을 켜고 프레임을
로드하는 일을 우리가 직접 몰아줘야 한다.

### B-0. Sequence 수집 비용 — 실측 완료 (2026-09-08)

1920×1080, SB 64 (510 SB/frame), libaom-av1 crf30, dav1d analyzer 디코더:

| 항목 | cold | warm (SSE 캐시 적중) |
|---|---|---|
| `loadFrame` (디코딩 + `sb_bitcount` 수집) | 18.6 ms/frame | 18.6 ms/frame |
| `getSuperblockBits` | ~0 ms | ~0 ms |
| SSE 계산 + SB 510개 조회 | 5.05 ms/frame | **0 ms** |
| **합계** | **24.85 ms/frame** | **19.15 ms/frame** |

`.bd_analyzer` 캐시는 60프레임에 960K.

환산: **8 스트림 × 300 프레임 = 2400 프레임 → 약 60초** (cold), 46초 (warm).

**결론 두 가지.**

1. **프레임 샘플링은 넣지 않는다.** 전 구간을 그대로 돌려도 분 단위를 넘지 않는다.
2. **디코더를 워커 스레드에서 돌리지 않는다.** 프레임당 25 ms 면 GUI 스레드에서 타이머로
   프레임 하나씩 슬라이스해도 UI 가 멈추지 않는다 (`QTimer` 0ms 또는 프레임당 1틱).
   설계 초안의 "워커에서 `loadFrame` 을 돌릴 때 디코더 상태가 GUI 재생과 충돌하는가" 라는
   위험이 이 방식으로 사라진다 — 디코더를 만지는 스레드가 하나로 유지된다.
   취소는 슬라이스 사이에서 자연스럽게 걸린다.

측정의 한계: 합성 영상(`testsrc2`)이고 순차 접근이다. 실제 촬영 영상은 디코딩이 더 느릴 수
있고, 순차 접근은 디코더의 최적 경로다 — Sequence 수집은 순차이므로 이 가정이 맞다.

### B-4. 통계는 "현재 프레임" 하나뿐이다

`StatisticsData::setFrameIndex()` 가 프레임이 바뀔 때마다 frameCache 를 비운다. 즉 스트림
하나에서 프레임 N 의 SB bits 를 얻으려면 그 스트림을 프레임 N 으로 디코딩해 놓아야 한다.
**Sequence 패널이 비싼 이유가 이것이다** — 스트림 수 × 프레임 수 만큼의 디코딩이다.

SSE 쪽은 사정이 낫다. `pixelStatsCache` 가 `.bd_analyzer` 에 프레임별로 남으므로 두 번째
실행부터는 다시 계산하지 않는다.

### B-5. 격자가 그룹 간에 같아야 한다

- 해상도가 다르면 SB 좌표가 대응하지 않는다.
- `sb_size` 가 64 / 128 로 갈리면 같은 좌표가 다른 영역이다 (AV1 은 둘 다 허용한다).
- org 는 스트림의 크기·픽셀 포맷과 일치해야 한다 (`setOriginalYUVSource` 가 이미 검증한다).

**착수 조건으로 검사하고, 안 맞으면 그룹 생성을 거절한다.**

### B-6. SB 단위 BD-rate 는 통계적으로 약하다 — 정직하게 표시할 것

BD-rate 는 두 곡선이 **PSNR 구간에서 겹칠 때만** 정의된다. superblock 하나의 RD 곡선은
프레임 전체보다 훨씬 거칠고, 두 그룹의 PSNR 범위가 겹치지 않는 SB 가 흔히 생긴다.
그런 SB 는 **숫자를 만들어내지 말고 "정의 불가" 로 표시**해야 한다.

또 표준 BD-rate 는 (log10 rate, PSNR) 4점에 3차 다항식을 맞춘다. 그룹당 점이 4개 미만이면
차수를 낮춰야 하고(3점 → 2차, 2점 → 선형), 그 사실을 UI 에 적어야 한다. 2점 선형은
BD-rate 라기보다 기울기 비교에 가깝다.

---

## 설계

### 1. 그룹 모델 (`src/bdrate/BdRateGroup.h`)

```
BdRatePoint   { QString label; playlistItem *item; }        // 한 QP 점 = 스트림 하나
BdRateGroup   { QString name; bool isAnchor; vector<BdRatePoint> points; QString orgPath; }
BdRateModel   { vector<BdRateGroup> groups; int anchorIndex; }
```

- 그룹 이름 자동 생성: 선택된 파일명의 **공통 접두어**를 쓴다
  (`a_q30.ivf a_q35.ivf a_q40.ivf` → `a`). 공통 접두어가 없으면 `Group 1`, `Group 2` …
  이름은 table 에서 직접 편집한다.
- anchor 는 기본적으로 index 0 이고, table 의 radio 로 바꾼다. anchor 를 바꾸면
  BD-rate 만 다시 계산한다 — 수집한 (bits, PSNR) 은 그대로 재사용한다.
- org 는 선택 안에 있던 raw 항목의 경로다. 없으면 그룹 안의 스트림에 이미 붙어 있는
  org 를 쓰고, 그것도 없으면 그룹 생성을 거절한다 (PSNR 을 만들 수 없다).

### 2. 수집 (`src/bdrate/BdRateCollector`)

ME 와 같은 구조를 그대로 쓴다. Qt 를 아는 층과 모르는 층을 나눌 필요는 없다 — 여기서는
디코더를 구동해야 하므로 전부 통합층에 둔다.

```
요청  : { groups, frameRange, scope(SB|Frame|Sequence), CancelToken*, ProgressToken* }
결과  : SbSamples  = map<(sbX,sbY), map<groupIdx, vector<(bits, sse, sampleCount)>>>
        FrameSamples, SequenceSamples
```

- 스트림 하나를 잡고 프레임을 순회하며 `loadFrame` → `getSuperblockBits` →
  `getPixelBlockStats` 를 모은다. **스트림 단위로 순차, 그룹 단위로 순차** — 디코더는
  순차 접근에 맞춰 만들어져 있어 프레임을 뛰어넘으면 비싸다.
- 취소는 프레임 경계에서 검사한다 (ME 는 superblock 경계였다. 여기서는 프레임 하나가
  최소 작업 단위다).
- 진행률은 `(스트림, 프레임)` 쌍 개수로 센다.

### 3. 수식 (`src/bdrate/BdRateMath.{h,cpp}` — Qt 무관, 단위 테스트 대상)

```
psnrFromSse(sse, sampleCount, bitDepth)
    = 10 * log10( (2^bitDepth - 1)^2 * sampleCount / sse )
    sse == 0 → 무한대. 상한을 두고 "무손실" 로 표시한다.

bdRate(anchor, test)                       // 각각 vector<(rate, psnr)>
    1. rate → log10(rate)
    2. psnr 로 정렬, 겹치는 psnr 구간 [max(min), min(max)] 을 구한다
    3. 겹치지 않으면 nullopt
    4. 점 개수에 따라 3차 / 2차 / 선형 피팅
    5. 구간 적분 차 → 10^(Δ/Δpsnr) - 1 → %
```

- 경계 SB 는 픽처 밖이 잘리므로 `sampleCount` 를 실제 겹치는 픽셀 수로 계산한다.
  64×64 를 고정으로 쓰면 경계 SB 의 PSNR 이 낙관적으로 나온다.
- `rate` 는 SB 의 bits. Sequence 패널에서는 프레임 합.

### 4. UI

- **버튼**: playlist 툴바 또는 View 메뉴에 `SB BD-rate`. 선택이 유효하지 않으면 비활성 +
  이유 표시 (스트림 0개, org 없음, 격자 불일치, dav1d 아님).
- **popup window** (`BdRatePlotWindow`, non-modal):
  - 상단: 체크박스 `SB` `Frame` `Sequence` → 켜진 것만 좌·중·우로 배치한다.
    끄면 그 패널을 숨긴다 (레이아웃에서 제거).
  - 각 패널: x = bits+1(log, 눈금·수치 있음), y = PSNR(dB, 눈금·수치 있음), 그룹마다 색이 다른 곡선 + 점.
    anchor 는 굵게. BD-rate % 를 범례에 함께 적는다.
  - 하단: 그룹 × 점 table (bits, PSNR) + BD-rate 열. 이름 편집, anchor radio, CSV export.
  - **SB 패널은 active SB 를 따라간다** — `splitViewWidget::blockSelected` 에 붙어 클릭한
    superblock 으로 갱신한다. 클릭 전에는 "superblock 을 클릭하세요" 를 표시한다.
- 기존 `RateDistortionPlotWidget` 은 **그대로 둔다.** 그것은 한 스트림의 프레임 내 산점도로
  용도가 다르다. 축과 곡선 그리기 코드는 필요하면 뽑아 공유한다.

---

## 단계

| 단계 | 내용 | 산출물 |
|---|---|---|
| 1 | n 항목 선택 접근자, 그룹 모델, org 자동 인식, 착수 조건 검사(B-2/B-5) | **완료** |
| 2 | `BdRateMath` + 단위 테스트 (PSNR, 피팅, 겹침 없음, 점 부족) | **완료** |
| 3 | 현재 프레임 수집 → SB / Frame 패널 + table | **완료** |
| 4 | Sequence 패널 — GUI 스레드 슬라이스 + 진행률 + 취소 (B-0) | **완료** |
| 5 | CSV export, 이름 편집, anchor 변경 | 마무리 |

1–3 단계까지가 "버튼을 눌러 SB BD-rate 를 본다" 를 만족한다. 4 단계는 비용이 크고 독립적이라
나중에 붙여도 된다.

---

## 검증 계획

| 무엇 | 어떻게 |
|---|---|
| PSNR 수식 | 알려진 SSE·픽셀 수로 손계산 대조. 경계 SB 의 sampleCount |
| BD-rate 수식 | 공개된 Bjøntegaard 참조 구현의 예제 수치와 대조 |
| 겹치지 않는 구간 | 일부러 분리된 두 곡선 → "정의 불가" 가 나오는지 |
| 점 부족 | 2점·3점 그룹에서 차수가 낮아지고 UI 에 표시되는지 |
| 격자 불일치 | 해상도가 다른 스트림, `sb_size` 가 다른 스트림을 섞어 거절되는지 |
| dav1d 아님 | FFmpeg 폴백 스트림을 넣어 거절되는지 |
| 수집 정확성 | 한 스트림 한 프레임에서 기존 RD plot 의 (bits, SSE) 와 값이 일치하는지 |
| 취소 | Sequence 수집 중 취소가 프레임 경계에서 먹는지 |

---

## 미결 / 위험

- ~~Sequence 수집 시간~~ → B-0 에서 실측했다. 1080p 기준 60초 규모이므로 샘플링 없이 간다.
- **SB 단위 BD-rate 의 해석**은 사용자가 조심해야 하는 값이다 (B-6). UI 가 "정의 불가" 와
  "점 부족" 을 눈에 보이게 표시하는 것이 이 기능의 신뢰도를 좌우한다.
- ~~워커에서 `loadFrame` 을 돌릴 때의 디코더 상태 충돌~~ → B-0 의 결론대로 GUI 스레드
  슬라이스로 가므로 해당 없다. 대신 **슬라이스 도중 사용자가 재생·프레임 이동을 하면**
  같은 디코더를 두 목적으로 쓰게 된다 — 수집 중에는 그 아이템의 프레임 이동을 막거나,
  수집을 취소하고 다시 시작해야 한다.
- 그룹이 많아지면 색 구분이 한계에 닿는다. 6개 이상은 범례에 의존해야 한다.

---

## 구현 결과 (1-3단계, 2026-09-08)

### 파일

| 파일 | 역할 |
|---|---|
| `src/bdrate/BdRateMath.{h,cpp}` | Qt 무관. `psnrFromSse`, `polyFit`/`polyEval`, `bdRate` |
| `src/integration/BdRateGroups.{h,cpp}` | 선택 → 그룹, 이름 자동 생성·중복 회피, org 자동 인식, 거절 사유 |
| `src/integration/BdRateCollector.{h,cpp}` | 한 프레임 수집. SB/프레임 (bits, SSE, sampleCount) |
| `src/integration/BdRatePlotWindow.{h,cpp}` | popup: 체크박스 3개, 패널 3개, 그룹 table, 값 table |
| 패치 `0034` | `getAllSelectedItems()`, `providesSuperblockBits()`, const `getStatisticsTypes()`, `src/bdrate` 빌드 |
| 패치 `0035` | `View → SB BD-rate from Selection` (`Ctrl+R`) 와 MainWindow 배선 |

### 수식 검증 방법

외부 참조 데이터 없이 **산술로** 검증했다. test 곡선의 rate 를 모든 지점에서 k 배 하면
BD-rate 는 정확히 `(k-1)*100 %` 다 — 곡선 모양과 무관하다. k = 0.5 / 0.8 / 1.0 / 1.25 / 2.0
에서 1e-6 오차로 통과한다. log, 피팅, 적분, 지수화 전체가 이 한 성질로 묶인다.

### 착수 후 알게 된 것

1. **`loadFrame` 을 수집기가 직접 불러야 한다.** 스트림은 자기가 디코딩한 프레임만
   `sb_bitcount` 를 보고하고, 화면에 보이는 아이템만 다른 무언가가 디코딩해 준다. 이걸 빼고
   만든 첫 버전은 4점 그룹에서 **1점만** 수집됐다 (실측). 측정된 25 ms/frame 근거로 GUI
   스레드에서 동기 호출하고, `emitSignals=false` 로 repaint 되먹임을 막는다.
2. **SSE 는 비동기라 첫 수집은 항상 `pending` 이다.** 재시도 타이머(200 ms, 40회 = 8초)가
   없으면 값이 영원히 0 으로 남는다 — 이것도 실측으로 잡았다.
3. **그룹 이름이 중복된다.** 이름을 파일명에서 뽑으므로 같은 선택으로 두 번 누르면 같은
   이름이 나온다. anchor radio 와 범례가 모두 모호해지므로 `(2)` 접미사로 유일화한다.
4. **B-6 이 실제로 크다.** 1080p `testsrc2` 4점에서 510 SB 중 **점 2개 이상은 107개**뿐이었다.
   내역은 사용 404점 / `bits==0` 48점 / **무손실(sse==0) 1588점**. 평탄한 합성 영상이라
   무손실 SB 가 압도적이다 — 실촬 영상에서는 훨씬 덜하겠지만, UI 가 "lossless" 와
   "no PSNR overlap" 과 "too few points" 를 구분해 말해야 하는 근거가 됐다.

### 지금 화면에서 되는 것

- `Ctrl+R` 로 선택을 곡선으로 추가. 같은 선택을 다시 누르면 두 번째 곡선.
- 그룹 table 에서 이름 편집, anchor radio 로 anchor 변경 (수집값 재사용, BD-rate 만 재계산).
- 체크박스 `SB` / `Frame` 로 패널 표시. `Sequence` 는 비활성(4단계).
- 값 table 에 그룹 × 스트림 × scope 별 bits / PSNR. 무손실은 `lossless` 로 표기.
- SB 패널은 클릭한 superblock 을 따라간다.

실측 확인 (176×144, CRF 20/40/55):

```
frame     : bits 6435 / 2842 / 1071   PSNR 51.723 / 42.143 / 36.924
SB (1,1)  : bits 2276 /  801 /  164   PSNR 50.359 / 37.386 / 32.386
```

### 4-5단계에 남은 것

- Sequence 수집 (프레임 슬라이스 + 진행률 + 취소). 수집 도중 사용자가 프레임을 옮기면
  같은 디코더를 두 목적으로 쓰게 되는 문제를 그때 다뤄야 한다.
- CSV export.
- 프레임 오버레이 heatmap (사용자가 고른 표시 형태 중 popup+table 만 구현했다).
- ~~`bits == 0` 인 SB 의 처리 방침~~ → 사용자 결정으로 **관행대로 `bits+1`** 을 채택했다.
  전 지점에 균일하게 적용한다 (0 만 보정하면 축에 단절이 생긴다). 프레임·시퀀스 총합에서는
  측정 불가능한 차이이고, table 은 실제로 쓴 bits 를 그대로 보여주며 그래프 축만
  `bits + 1 (log)` 로 표기한다.

---

## 4단계 구현 결과 (2026-09-08)

`BdRateSequenceSweeper` (`src/integration/BdRateCollector.{h,cpp}`).

- **GUI 스레드 슬라이스.** `QTimer` interval 0 으로 tick 당 프레임 하나. B-0 의 25 ms/frame
  근거대로 워커 스레드를 쓰지 않으므로, 뷰가 이미 구동하는 디코더를 두 스레드가 만지는 상황이
  생기지 않는다.
- **스트림 단위 순차.** 그룹 → 점 → 프레임 순서로 커서를 옮긴다. 디코더는 순차 접근에 맞춰
  있어 스트림을 프레임마다 번갈아 잡으면 매 걸음이 seek 이 된다.
- **SSE 대기.** 프레임의 SB 전부에 SSE 가 도착해야 그 프레임을 누적한다. 없으면 같은 프레임을
  다음 tick 에 다시 본다 (최대 200회). 블로킹하지 않는다.
- **누적 방식.** bits 합, SSE 합, sampleCount 합을 쌓고 **총합에서 PSNR 을 낸다.** 프레임별 dB
  를 평균하는 것은 다른 값이며 흔한 오류다.
- **프레임 범위**는 모든 스트림의 교집합이다. 잘린 인코드가 "그 프레임은 0 bits" 로 읽히는 것을
  막는다.
- **진행률**은 (스트림, 프레임) 쌍 기준. 25 스텝마다만 갱신 신호를 낸다 — 매 프레임 리페인트하면
  스위프 시간을 그리기에 쓴다.
- **체크박스가 스위치**다 (사용자 요구). `Sequence` 를 켜면 시작, 끄면 취소. 진행률 바와
  Cancel 버튼은 스위프 중에만 보인다.
- **부분 결과를 그린다.** 1분간 빈 상자를 보여주는 것보다 채워지는 곡선이 낫고, 상태줄이
  아직 최종이 아님을 말한다.
- 스위프가 끝나면 `refresh()` 로 프레임·SB 패널을 다시 수집한다 — 스위프가 스트림들을 다른
  프레임으로 옮겨 놓았기 때문이다.

실측 (176×140, 11프레임, CRF 20/40/55):

```
seq 0-10  : bits 67304 / 32772 / 19608   PSNR 51.360 / 41.567 / 36.142
seq SB(1,1): bits 21056 / 9091 / 3735    PSNR 48.787 / 37.363 / 32.011
```

회귀 31 이 스위프 완료·누적성(시퀀스 bits > 프레임 bits)·단조성·취소를 고정한다.

---

## RD curve 축 눈금 (2026-09-11)

곡선만 있고 축에 눈금이 없어 "이 점이 몇 bit 몇 dB 인지" 를 읽을 수 없었다. 양 축에 눈금·수치·
격자를 넣었다.

- **PSNR 축(선형)**: `niceAxisStep()` 이 1 / 2 / 5 × 10^k 중 하나를 고른다.
- **rate 축(로그)**: `logAxisTicks()` 가 **bit 단위로 round 한 값**(1, 2, 5, 10, 20, 50 …)만 찍는다.
  로그 공간에서 등간격으로 찍으면 10^3.4, 10^3.7 같은 아무도 못 읽는 눈금이 된다.
- 라벨은 `formatAxisRate()` 로 짧게 (`5000` → `5k`). 좁은 패널에서 라벨이 겹치면 눈금만 남기고
  그 라벨은 건너뛴다 — 겹친 글자는 없느니만 못하다.
- 격자선은 곡선 **아래**에 그린다.

### 구현 중 렌더링으로 잡은 것 2건

1. **x축 제목이 눈금 수치와 같은 줄에 있었다.** 축이 `3k 5k bits + 1 (log) 7k 10k` 로 읽혔다.
   제목을 눈금 라벨 아래로 내렸다.
2. **눈금이 너무 성겼다.** `niceAxisStep()` 이 다음 round 값으로 **올림**하고 있어서, 12 dB 구간이
   2.4 를 요청하고 5 를 받아 축 전체에 라벨이 **40, 45 둘뿐**이었다. **반올림**으로 바꿔 2 dB 간격
   (38/40/42/44/46/48)이 됐다. 둘 다 숫자만 봐서는 드러나지 않고 그림을 봐야 보이는 종류다.

### 왜 테스트로 고정했나

틀린 눈금은 틀려 보이지 않는다. 42.7 / 45.1 / 47.5 는 42 / 44 / 46 만큼 그럴듯하게 그려지고,
rate 축이 한 decade 어긋나도 여전히 말이 되는 숫자다. 그래서 세 헬퍼를 `BdRatePlotWindow.h` 에
노출하고 회귀 `34-rd-curve-axis-ticks` 가 값을 고정한다 — 렌더 결과의 여백 잉크량까지 확인해
라벨이 실제로 그려졌는지도 본다.

좁은 구간(예: 2100–4800 bits)에는 1/2/5 mantissa 가 하나도 들어오지 않아 눈금이 1개만 나오던
것도 이 테스트가 잡았다. 그 경우 축은 사실상 선형이므로 **bit 단위 선형 round 눈금**으로 폴백한다.

---

### 5단계에 남은 것

- CSV export
- 프레임 오버레이 heatmap
- **스위프 중 사용자가 프레임을 옮기면** 같은 디코더를 뷰와 스위퍼가 번갈아 잡는다. 지금은
  스위프가 이기고 뷰가 뒤늦게 다시 디코딩할 뿐이라 오동작은 관측되지 않았지만, 스위프 중
  프레임 이동을 막거나 스위프를 취소하는 처리가 필요하다. **미검증**.
