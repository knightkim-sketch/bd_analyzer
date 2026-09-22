---
title: 두 스트림 비교 (Find diff) 설계
status: draft
created: 2026-09-22
updated: 2026-09-22
author: claude-opus-5
verified: partial
---

# 두 스트림 비교 (Find diff)

## 1. 무엇을 푸는가

같은 소스를 두 인코더(또는 두 설정)로 만든 스트림이 **어디서부터 갈라지는지**를 찾는다.
지금은 Block Info 패널이 **클릭한 픽셀 하나**의 구문만 보여주므로, "첫 번째로 달라지는 블록"을
찾으려면 사람이 수백 번 클릭해야 한다.

목표는 하나다 — **최초 불일치 블록의 좌표와 그 원인이 된 구문 요소를 특정한다.**

## 2. 이미 있는 것 (확인함)

설계의 대부분은 새로 만드는 게 아니라 이어 붙이는 것이다.

| 필요한 것 | 현재 상태 |
|---|---|
| 블록별 구문 값 | `playlistItemCompressedVideo::getBlockInfoAt(QPoint, frame)` → `stats::BlockInfo` |
| 블록의 코딩 사각형 | `BlockInfo::codingBlockRect` (프레임 **픽셀 좌표**) |
| 블록의 비트 범위 | `BlockInfo::bitstreamRange` = `{startBit, endBit, origin}`. analyzer dav1d 가 내보낸 실제 읽기 위치 |
| 비트 범위 → 바이트 | `sliceBitRange(frameData, range)` — hexdump 패널이 쓰는 함수 |
| SB 단위 값 | 블록 엔트리의 `sb_qindex`, `sb_bitcount` |
| 헤더 구문 트리 | `PacketItemModel` / `TreeItem` (이름·값·coding·code) |
| recon 차분 | File → **Add Difference Sequence** (두 영상의 차분 아이템) |
| 배치 덤프 | `tools/cli/av1-block-dump.cpp` — 같은 경로를 CSV 로 |
| 블록 선택 브로드캐스트 | `splitViewWidget::blockSelected(item, pixelPos, frameIdx)` → Mainwindow 가 Block Info / Frame Info / hexdump 패널로 팬아웃 |
| 패널 측 수신구 | `BlockInfoWidget::setSelectedBlock(item, pixelPos, frameIdx)` — **public slot** 이라 직접 호출 가능 |
| 프레임 이동 | `PlaybackController::setCurrentFrameAndUpdate(frameIdx)` |

**즉 데이터는 이미 다 있다.** 없는 것은 두 스트림을 나란히 놓고 비교하는 계층과 UI 다.

## 3. 블록 수가 프레임마다 크게 다른 것은 정상이다

처음에는 `av1-block-dump` 이 첫 프레임만 온전하다고 판단했다. **틀린 판단이었다.**
512x288 스트림을 측정한 결과:

| frame | queried | invalid | wrongFrame | ok | distinct blocks | 픽셀 커버리지 |
|---|---|---|---|---|---|---|
| 0 | 9216 | 104 | **0** | 9112 | 792 | 98.9% |
| 1 | 9216 | 0 | **0** | 9216 | 43 | 100.0% |
| 2 | 9216 | 256 | **0** | 8960 | 60 | 97.2% |
| 3 | 9216 | 128 | **0** | 9088 | 45 | 98.6% |
| 4 | 9216 | 32 | **0** | 9184 | 94 | 99.7% |
| 5 | 9216 | 0 | **0** | 9216 | 44 | 100.0% |

`wrongFrame` 이 전 프레임 0 이다 — 통계는 지연되지 않는다. 프레임 0 은 intra 라 작은 블록이 792개
나오고, 이후 inter 프레임은 **큰 블록 43~94개가 화면을 100% 가까이 덮는다.** 정지된 뉴스 클립에서는
당연한 결과다.

**교훈: 행 수로 완전성을 판단하면 안 된다.** 판단 기준은 **픽셀 커버리지**다. 비교 기능의 자체
검사도 커버리지로 한다 — 커버리지가 낮으면 그때가 실제로 통계가 덜 온 것이다.

한편 `av1-block-dump` 은 `loadFrame()` 을 **메인 스레드에서** 부른다. 그 함수 첫 줄은
`Q_ASSERT(QThread::currentThread() != QApplication::instance()->thread())` 로 "메인 스레드에서
부르지 말라"는 계약이다. release 빌드라 assert 가 사라져 통과할 뿐이다. 지금 증상의 원인은
아니지만, 비교 기능이 같은 경로를 쓰기 전에 정리해야 한다.

## 4. 비교 계층

네 계층을 **싼 것부터** 돌린다. 앞 계층이 후보 위치를 좁혀 주면 뒤 계층은 그 근처만 본다.

### A. 헤더 구문 (sequence / frame header)

- 두 스트림의 파서 트리를 순서대로 훑어 `(이름, 값)` 을 비교한다.
- 이름이 다르면 **한쪽에만 있는 요소**로, 값이 다르면 **값 불일치**로 보고한다.
- 어제 VQ Analyzer 대조에 쓴 방식과 같다 — 배열 인덱스 표기 차이 같은 것은 정규화가 필요하다.
- 헤더가 다르면 그 아래 블록 비교는 의미가 약해지므로 **먼저 보고하고 사용자가 계속할지 정한다.**

### B. SB 단위 24bit 비트스트림 비교 — 빠른 위치 탐색

각 superblock 의 첫 블록 `bitstreamRange.startBit` 에서 **24비트**를 읽어 두 스트림을 비교한다.

- 읽기는 `sliceBitRange` 로 하고, 바이트 경계가 아니므로 비트 시프트가 필요하다.
- 비교 결과는 SB 격자 위의 **일치/불일치 맵**이다.

**이 지점은 실제 불일치 지점이 아니다.** 근거:
- 비트 범위는 코드 주석이 말하듯 *"the arithmetic coder's read positions, not a bit field"* 다.
  산술 부호기는 심볼 경계와 비트 경계가 일치하지 않고, 확률 상태가 누적된다.
- 한 심볼이 갈리면 그 뒤 비트열은 **내용과 무관하게** 전부 달라진다. 즉 24bit 불일치는
  "여기 이후 어딘가에서 갈렸다"만 말해 준다.

**따라서 B 는 판정이 아니라 탐색이다.** 실제 판정은 C 가 한다:
> 24bit 이 처음 갈리는 SB 를 찾고, **그 지점 이후에 syntax 가 실제로 달라지는 첫 블록**을
> 진짜 불일치 블록으로 본다.

이 규칙을 UI 와 보고서에 명시한다. B 의 결과만 보고 "여기가 원인"이라고 읽히면 안 된다.

### C. 블록 단위 구문 비교 — 실제 판정

- **픽셀 좌표 기준으로 정렬한다.** 두 스트림의 partition 이 다르면 블록 인덱스는 맞지 않는다.
  4x4 격자를 훑어 각 위치의 `codingBlockRect` 와 구문 엔트리를 비교한다.
- 비교 단위는 `BlockInfoEntry` 의 `(typeName, valueText)` 다. 이미 포맷된 값이라 표시와 일치한다.
- 보고 순서: `(frame, y, x)` 오름차순. B 가 지목한 SB 이후부터 훑되, 전체 훑기도 선택 가능하게 한다.
- 분류:
  - **partition 불일치** — `codingBlockRect` 가 다름
  - **구문 불일치** — 사각형은 같고 값이 다름
  - **한쪽에만 존재** — 한쪽에서 블록을 못 찾음

### D. recon 비교

- 기존 difference 아이템 경로를 재사용한다. 새 디코딩 경로를 만들지 않는다.
- 프레임별 SSE/PSNR 과 차이가 0 이 아닌 첫 프레임을 보고한다.
- **체크박스로 on/off** — 두 스트림을 모두 디코딩해야 하므로 가장 비싸다.

### 제외: CDF 비교

지금은 **접근 경로가 없다.** analyzer dav1d 포크가 내보내는 것은 `Av1Block` + 비트 범위 +
`sb_qindex` 뿐이고, 엔트로피 코더의 적응 확률 테이블은 내보내지 않는다. 넣으려면 포크에 CDF
export 를 구현하고 `libdav1d-internals.so` 와 YUViewLib 이 공유하는 ABI 를 확장해야 하는데,
그 포크는 2019년 0.2.2 에서 멈춰 있다. **별도 과제로 분리하고 조사부터 한다.**

## 5. UI

- playlist 에서 **정확히 2개** 선택 → 우클릭 메뉴 / View 메뉴에 **Find diff**.
- 2개가 아니면 항목을 비활성화하고 이유를 보여준다 (BD-rate group 거절과 같은 방식).
- 결과는 popup 창:
  - 상단: 두 스트림 이름, 헤더 비교 요약 (A)
  - 중단: SB 격자 맵 (B) — 24bit 불일치 SB 를 표시하고, **"위치 탐색용이며 판정이 아니다"** 를 명시
  - 하단: 블록 불일치 표 (C) — `frame / x,y / 종류 / 구문 요소 / 양쪽 값`. 행을 누르면 해당
    프레임·좌표로 이동
  - 체크박스: **CDF 비교** (비활성, 사유 표시), **recon 비교** (D)

### 5.1 최종 mismatch 지점으로 이동하고 활성화한다

C 가 최초 불일치 블록을 확정하면, 사용자가 다시 찾아 클릭하지 않도록 **앱을 그 지점 상태로 만든다.**

1. playlist 선택을 해당 스트림(A 또는 B — 아래 토글을 따른다)으로 바꾼다.
2. `PlaybackController::setCurrentFrameAndUpdate(frame)` 으로 그 프레임으로 이동한다.
3. 그 블록을 **선택된 상태로 만든다** — 화면의 하이라이트와 Block Info / Frame Info / hexdump
   패널이 동시에 그 블록을 가리켜야 한다.

3번은 마우스 이벤트를 흉내내지 않는다. 클릭이 최종적으로 하는 일은
`splitViewWidget::blockSelected` 를 내보내는 것이고, Mainwindow 가 그것을 세 패널로 팬아웃한다.
**같은 신호를 쓰면 클릭과 구분되지 않는 상태가 된다.**

단, 화면 위 하이라이트는 `splitViewWidget::blockSelection` 에 들어 있고 **public setter 가 없다.**
`mousePressEvent` 가 하는 일(좌표 해석 → 블록 질의 → 캐시 갱신 → repaint → 신호 방출)에서
좌표 해석만 건너뛰는 **public slot 하나를 upstream 패치로 추가한다.** 그 슬롯이 단일 진입점이 되고,
Find diff 창은 그것만 부른다.

패널이 안 열려 있을 수 있으므로, 이동 시 Block Info dock 이 닫혀 있으면 함께 띄운다.

### 5.2 A / B 토글 — 어느 쪽 정보를 볼 것인가

불일치 블록은 **양쪽 값이 모두 궁금한 대상**이다. 그래서 결과 창 상단에 스트림 선택을 둔다.

- **Syntax info: A / B** — 헤더·구문 트리를 어느 스트림 것으로 볼지
- **Block info: A / B** — 이동·활성화 대상 스트림, 즉 Block Info 패널에 뜨는 쪽

두 토글은 **독립**이다. 한쪽 구문 트리를 보면서 다른 쪽 블록을 활성화하는 조합이 실제로 쓰인다.

- 토글을 바꾸면 **현재 선택된 불일치 지점을 유지한 채** 대상 스트림만 바꿔 다시 활성화한다
  (프레임·좌표는 그대로).
- 불일치 표의 각 행은 이미 양쪽 값을 나란히 보여준다. 토글은 **표가 아니라 앱 본체의 패널**이
  어느 쪽을 가리킬지를 정한다 — 그 구분을 UI 문구에 넣는다.

## 6. 작업 순서

1. 블록 조회를 계약대로(메인 스레드 밖에서) 부르고, 완전성을 **픽셀 커버리지**로 자체 검사 (3장)
2. Find diff 다이얼로그 + 헤더 구문 비교 (A)
3. SB 24bit 비교 (B)
4. 블록 구문 비교 (C)
5. recon 비교 배선 (D)

각 단계마다 회귀 테스트를 하나씩 붙인다. 비교 로직의 알맹이(정규화·정렬·판정)는 Qt 없는
코어(`src/diff/`)에 두고 단위 테스트한다 — GUI 없이 검증할 수 있어야 한다.

## 7. 미검증 / 위험

- **24bit 폭의 근거는 사용자 경험이다.** 왜 24인지 스펙적 근거를 확인하지 않았다. 폭을
  파라미터로 두고 기본값 24 로 한다.
- SB 첫 블록의 `startBit` 이 곧 SB 의 시작이라고 가정한다. **확인 필요** — 블록 순회 순서와
  SB 경계가 어긋나면 비교 기준점이 틀어진다.
- 두 스트림의 해상도·프레임 수가 다르면 비교는 거절한다. 판정 규칙 미정.
- `bitstreamRange` 는 analyzer dav1d 디코더에서만 나온다. FFmpeg 폴백 스트림은 B 를 못 쓴다 —
  그 경우 A 와 C 만 돌린다.
- 프레임 수가 많고 해상도가 크면 C 는 비싸다. 4x4 격자 전수 훑기의 비용을 재지 않았다.
- 5.1 의 이동·활성화는 **upstream 패치가 필요하다** (`splitViewWidget` 에 블록 선택 public slot).
  `blockSelected` 신호와 `setSelectedBlock` 슬롯은 이미 있으므로 추가 범위는 그 하나다.
- A/B 토글을 바꿀 때 해당 스트림에 그 좌표의 블록이 없는 경우는 **정상 스트림에서는 발생하지
  않는다** (확인됨). 좌표는 유지하고 **"블록 없음"** 을 표시하는 것으로 확정한다.
