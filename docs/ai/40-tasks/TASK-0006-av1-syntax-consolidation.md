---
title: TASK-0006 AV1 syntax 항목 통합 + OBU info 탭
status: todo
created: 2026-08-07
updated: 2026-08-07
author: claude-opus-5
verified: no
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 배경

AV1 analyzer 의 syntax 표와 오버레이 목록이 너무 길다. 사용자가 항목 제거/통합과
OBU 탐색 기능을 요청했다. 제거(A)와 레이아웃(C)은 이미 끝났다 (커밋 `1dde6d3`, `e6d6f2e`).
이 문서는 **남은 B, D, E** 의 확정된 사양이다. 설계 결정은 모두 사용자와 합의됐다.

## 현재 등록된 통계 타입 (제거 후, 실측)

```
0 Pred Mode          1 Segment ID        2 skip              3 skip_mode
4 intra pred mode (Y)                    5 intra pred mode (UV)
8 intra angle delta (Y)                  9 intra angle delta (UV)
10 intra direction luma (벡터)           11 intra direction chroma (벡터)
12 chroma from luma alpha U              13 chroma from luma alpha V
14 ref frame index 0                     15 ref frame index 1
24 Motion Vector 0 (벡터, 빨강 w2)       25 Motion Vector 1 (벡터, 파랑 w2)
26 Transform Size
```

정의 위치: `decoder/decoderDav1d.cpp` `fillStatisticList()` / emit 은 `parseBlockPartition()`.

## B. 항목 통합 — 확정 사양

핵심 원칙: **오버레이의 색상·의미는 건드리지 않고, syntax 표에서만 행을 합친다.**
예외는 intra 계열의 오버레이 텍스트다 (아래 B-2).

### B-1. motion vector 0 + 1 → 표에서만 통합

- 오버레이 체크박스는 **2개 그대로**. 색 구분(L0 빨강 / L1 파랑, 두께 2)을 유지해야 하므로
  타입을 합칠 수 없다 — `StatisticsType` 은 `vectorStyle` 을 하나만 갖는다.
- `getBlockInfoAt()` (또는 `BlockInfoWidget`) 에서 24/25 를 한 행으로 조립.
  예: `Motion Vector | L0 (-1.25,0.5)  L1 (2.0,-0.75)`. compound 가 아니면 L0 만.

### B-2. intra pred mode + angle delta + direction

- **오버레이 블록 blending 은 `intra pred mode` (4/5) 만 사용한다.**
  `intra angle delta` (8/9) 는 오버레이 목록에서 뺀다 — 단, **값은 계속 emit 해야 한다**
  (아래 텍스트와 syntax 표에서 쓰인다). 즉 타입을 지우지 말고 오버레이에서만 감춘다.
  → `StatisticsType` 에 "목록에 노출하지 않음" 개념이 없으므로, 가장 단순한 방법은
     8/9 를 `statisticsData` 에 등록은 하되 UI 목록에서 제외하는 것. 등록 자체를 빼면
     `at(8)/at(9)` emit 이 갈 곳이 없어진다. **구현 시 여기가 첫 번째 확인 지점.**
- `intra direction` (10/11, 벡터) 은 `intra pred mode` (4/5) 타입에 **실제로 병합**한다.
  `StatisticsType` 은 값+벡터를 동시에 가질 수 있다 (`hasValueData` + `hasVectorData`).
  → 체크박스가 2개 줄어든다.
- **`intra pred mode` 가 체크되면 블록 내부 텍스트에 mode 와 angle delta 를 함께 표시**한다.
  예: `DC_PRED (0)  Δ+2`. 그리기 위치는 `statistics/StatisticsDataPainting.cpp` 의
  값 텍스트 렌더링 경로. **구현 전에 그 경로가 실제로 존재하는지 확인할 것** (미확인).

### B-3. ref frame index 0 + 1 → 표에서만 통합

- 오버레이 체크박스는 2개 그대로 (compound 면 두 값이 서로 다른 블록 집합을 덮는다).
- 표에서 한 행: `ref frame | L0 LAST(0)  L1 ALTREF(6)`.
- 이름 매핑이 없으므로 숫자만 나온다. 0-based 인덱스의 의미는
  `0 LAST, 1 LAST2, 2 LAST3, 3 GOLDEN, 4 BWDREF, 5 ALTREF2, 6 ALTREF` (커밋 `c294c7d` 에서 실측 확인).
  이름을 붙이려면 `setMappingValues()` 를 추가하면 된다.

### 통합 후 예상 오버레이 목록

```
Pred Mode / Segment ID / skip / skip_mode
intra pred mode (Y)  ← 방향 벡터 포함, 블록 텍스트에 delta 병기
intra pred mode (UV) ← 동일
chroma from luma alpha U / V
ref frame index 0 / 1
Motion Vector 0 (빨강) / Motion Vector 1 (파랑)
Transform Size
```

## D. 좌측 Info listbox 에 OBU info 탭

- 현재 Info pane 은 `playlistItem::getInfo()` 의 key/value 목록 (`FileInfoWidget`).
- OBU 목록은 `parser::ParserAV1OBU` 의 파스 트리에서 가져온다. Bitstream Analysis 탭이
  이미 같은 파서를 쓰므로, 데이터 소스는 있다. 새로 만들 것은 탭 UI 와 목록 채우기.

## E. OBU double-click → 이동 (둘 다)

사용자 결정: **ⓒ 둘 다 수행**한다.
1. 해당 OBU 가 속한 **프레임으로 seek** (`PlaybackController::setCurrentFrame` 계열)
2. **Bitstream Analysis 탭의 파스 트리**에서 해당 노드로 스크롤 + 선택

## 작업 순서 권장

1. **B** (한 세션) — B-2 의 두 "확인 지점" 을 먼저 코드로 확인하고 시작할 것
2. **D + E** (한 세션) — 파서 트리 연동이라 분량이 크다

## 검증

- 통계 타입 목록은 헤드리스로 찍어서 확인한다 (기존 프로브 패턴,
  `tests/regression/12-*` 와 세션 중 쓴 `probe-mvdraw.cpp` 참고).
- 오버레이 렌더는 오버레이를 끈 렌더와 **차분**해서 확인한다. 테스트 패턴 자체에 순수
  빨강/파랑이 있어 색 픽셀을 그냥 세면 아무것도 증명하지 못한다 (커밋 `953ebeb` 참고).
