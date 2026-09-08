---
title: SB 단위 BD-rate 설계
status: proposed
created: 2026-09-08
updated: 2026-09-08
author: claude-opus-5
verified: no
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
| **Sequence** | 전 프레임 누적 | **비싸다.** 스트림 수 × 프레임 수 만큼 디코딩 |

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
  - 각 패널: x = bits(log), y = PSNR, 그룹마다 색이 다른 곡선 + 점.
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
| 1 | n 항목 선택 접근자, 그룹 모델, org 자동 인식, 착수 조건 검사(B-2/B-5) | 그룹을 만들 수 있다. 그래프는 없다 |
| 2 | `BdRateMath` + 단위 테스트 (PSNR, 피팅, 겹침 없음, 점 부족) | 수식이 검증된다 |
| 3 | 현재 프레임 수집 → SB / Frame 패널 + table | **여기까지가 실용 최소치** |
| 4 | Sequence 패널 — 백그라운드 + 진행률 + 취소 | 전 구간 |
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

- **Sequence 수집 시간**을 아직 재지 않았다. 1080p·300프레임·스트림 8개면 디코딩만
  2400 프레임이다. 4단계 착수 전에 실측해서, 감당 못 할 규모면 프레임 샘플링(예: 매 N번째)을
  옵션으로 넣어야 한다.
- **SB 단위 BD-rate 의 해석**은 사용자가 조심해야 하는 값이다 (B-6). UI 가 "정의 불가" 와
  "점 부족" 을 눈에 보이게 표시하는 것이 이 기능의 신뢰도를 좌우한다.
- `loadFrame` 은 메인 스레드가 아닐 것을 `Q_ASSERT` 로 요구한다 (릴리스에서는 컴파일
  아웃된다). 워커에서 돌릴 때 디코더 상태가 GUI 쪽 재생과 충돌하지 않는지 확인이 필요하다 —
  ME 는 디코더를 건드리지 않았으므로 이 경로는 처음이다.
- 그룹이 많아지면 색 구분이 한계에 닿는다. 6개 이상은 범례에 의존해야 한다.
