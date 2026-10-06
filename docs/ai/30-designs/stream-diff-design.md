---
title: 두 스트림 비교 (Find diff) 설계
status: draft
created: 2026-09-22
updated: 2026-10-04
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

## 4.5 개정 (2026-10-01) — B 를 빼고, payload 비교를 그 자리에 넣는다

Knight 의 실제 사례(`enc_cfg_06_PierSeaSide ... .av1` vs `.dut.av1`, 2번째 GOP order_hint 26)를
돌려 보고 계층 구성을 바꿨다. 현재 파이프라인은 네 단계다.

| 단계 | 하는 일 | 구현 |
|---|---|---|
| 1 | sequence header 비교 | `tools/cli/stream-diff-headers` (계층 A) |
| 2 | OBU header + frame header 비교 | 같음 |
| 3 | **OBU payload 를 바이트로 비교** | `src/diff/ObuPayloadDiff`, `tools/cli/stream-diff-payload` |
| 4 | 3 이 지목한 **그 프레임만** 디코딩해 블록 syntax 비교 | `src/diff/BlockSyntaxDiff`, `tools/cli/stream-diff-blocks` |

### 왜 B(24bit 창)를 뺐는가

**블록 비트 오프셋이 packet 상대이고, `getItemDataDump()` 는 *표시* 프레임의 packet 을 준다.**
hidden ARF 가 끼는 순간 둘이 어긋난다 — 측정값: 표시 프레임 2 의 dump 는 5바이트인데
그 블록들의 비트 범위는 74003..82560 으로 **앞 packet** 안에 있다. 잘못된 버퍼를 기준으로
읽으면 틀린 비트를 조용히 비교한다.

3단계(payload 전체를 바이트로 비교)는 이 문제를 **통째로 우회한다.** OBU 경계는 leb128
`obu_size` 로 정확히 나오고, 비교 단위가 바이트라 비트 기준점이 필요 없다. 같은 답(첫 갈림
지점)을 더 정확하게 준다. Knight 승인으로 B 는 제외했다.

### 4단계가 보고하는 좌표

전역 규약을 따른다 — **SB 와 MI 는 `(row, col)`, MV 는 `(x, y)`.**

- 프레임을 **MI(4x4) 격자**로 들고, 각 위치마다 그 위치를 덮는 coding block 을 기록한다.
  블록 목록이 아니라 위치 기준이라 **partition 이 달라도 짝이 맞는다** — A 가 32x32 를 쪼개지
  않고 B 가 쪼갰으면 짝지을 블록이 없지만, 각 MI 위치에는 양쪽 모두 블록이 하나씩 있다.
- SB 는 raster 순서, SB 안의 MI 도 raster 순서로 훑는다. **z-scan 이 아니다** — SB 안의 부호화
  순서는 partition 트리를 따르는데 디코더가 그것을 내보내지 않고, Morton 코드로 흉내내면
  수직 분할에서 틀린다. 그래서 "첫" 은 *위에서-왼쪽에서* 라는 뜻이고, 첫 SB 안의 차이 나는
  블록은 **전부** 보고한다.

### sb_bitcount 는 블록 syntax 가 아니다 (측정으로 확인)

dav1d 는 `sb_bitcount` 와 `sb_qindex` 를 **SB 전체 값인데 그 SB 의 모든 블록에 붙여서** 내보낸다
(`decoderDav1d.cpp:1083`, `playlistItemCompressedVideo.cpp:1377` 에 이미 기록돼 있다).

블록 syntax 로 세면 한 블록의 차이가 그 SB 의 모든 블록으로 번진다. WorldCup 512x288 프레임 1
실측:

| | 차이 MI 위치 | SB(0,1) 의 차이 블록 |
|---|---|---|
| SB 값을 블록으로 셀 때 | 8208 / 9216 | 4 (3개가 허수) |
| SB 값을 분리할 때 | 2594 / 9216 | 1 |

`BlockDiffOptions::superblockElements` 로 이름을 **호출자가 넘긴다** — `src/diff/` 코어는 특정
디코더의 어휘를 몰라야 하기 때문이다. SB 값 차이는 SB 줄에 한 번만 적는다.

그 결과 답이 둘로 갈린다:

- **`firstSbWithBlockDiff()`** — 블록 syntax 가 실제로 갈린 첫 SB. 이것이 찾던 답이다.
- **`firstSb()`** — 비트 수 같은 SB 집계만 달라도 포함. 같은 판단을 하고 residual 만 다른 경우라
  의미는 있지만, 갈림 지점은 아니다.

둘이 다르면 CLI 가 **둘 다** 찍는다.

### 3단계 → 4단계 프레임 매핑 (Knight 사례에서 걸린 부분)

**3단계는 *코딩* 프레임을, 4단계는 *표시* 프레임 인덱스를 다룬다. 둘은 같은 번호가 아니다.**

PierSeaSide 실측:

| TU 90 | order_hint | show_frame |
|---|---|---|
| OBU 1 | 28 | 0 (hidden) |
| **OBU 2** | **26** | **0 (hidden)** ← 3단계가 지목 |
| OBU 3 | 25 | 1 |

- `show_frame = 0` 인 hidden ARF 는 **뒤의 어떤 TU 가 `show_existing_frame` 으로** 화면에 올린다.
- display 90 은 order_hint **25** 를 보여준다. 이 GOP 은 `display = order_hint + 65` 이고
  (ki65 이므로 2번째 GOP 이 display 65 시작), **order_hint 26 은 display 91** 이다.
- display 90 으로 비교하면 `SB(10, 29)` 라는 **무관한 답**이 나온다. display 91 로 비교하면
  `SB(4, 47)` 로 Knight 의 답과 일치한다.

그래서 `stream-diff-blocks` 가 `--tu N --obu N` 을 받아 직접 푼다. 디코더가 하는 대로 참조 슬롯을
흉내낸다 — 디코딩된 프레임을 `refresh_frame_flags` 가 지목한 모든 슬롯에 쓰고,
`show_existing_frame` 은 `frame_to_show_map_idx` 가 가리키는 슬롯을 화면에 올린다.
(검산: OBU 2 의 `refresh_frame_flags = 4` = 슬롯 2, display 91 이 `map_idx = 2`.)

> **함정**: packet item model 의 **행 번호와 `Global AVPacket Count` 가 한 칸 어긋난다.**
> 3단계가 보고하는 번호는 packet 쪽이므로 행으로 색인하면 옆 TU 를 집는다 — 프레임 구성이 다르다.
> (레이어 A 에서 한 번 같은 실수를 했고, 여기서 또 걸렸다.)

### 4단계 비용 — 두 번 줄였다 (실측)

4K 한 프레임을 처음 돌렸을 때 **21분 37초**였다. 두 가지를 고쳤고, 두 번 모두 **출력은 동일**하다.

| | 질의 수 (A/B) | 스캔한 SB | 4K 프레임 17 |
|---|---|---|---|
| 처음 | 518,400 / 518,400 | 2040 / 2040 | **21m 37s** |
| 블록당 1회 질의 | 68,389 / 67,975 | 2040 / 2040 | **3m 55s** |
| + 첫 차이에서 중단 | 2,484 / 2,487 | **31** / 2040 | **22.8s** |

답은 셋 다 `SB(0, 30)` / `MI(0, 480)`, 그 SB 안 7개 블록까지 같다.

**1) MI 위치당이 아니라 블록당 한 번 질의한다.** `StatisticsData::getBlockInfoAt` 은 모든 통계
타입의 모든 값에 대한 선형 스캔이다 — 마우스 클릭 한 번에는 공짜지만 4K MI 격자 518,400 위치에는
아니다. 질의가 **덮는 블록의 rect 전체**를 돌려주므로 그 안의 MI 위치는 한 번의 답으로 전부
확정된다. 실제 질의가 블록을 놓은 위치만 건너뛰고, 아무 블록도 덮지 않는 위치는 여전히 질의하므로
근사가 아니다.

**2) 첫 차이에서 멈춘다.** SB 를 raster 순서로 훑고 블록이 실제로 갈린 첫 SB 에서 중단한다.
`--all` 로 전체 집계를 볼 수 있다.

### 왜 이진 탐색이 아닌가 (Knight 제안 검토, 실측으로 기각)

> "SB 1 비교, half 비교, 같으면 다음 half"

비용이 어디 있는지는 정확한 지적이고 그래서 위 2)를 넣었다. 다만 **이진 탐색 자체는 틀린 답을
조용히 낸다.**

- 성립 조건은 "경계 앞은 전부 같고 뒤는 전부 다르다"는 **단조성**이다.
- 뒷부분이 단조가 아니다. 산술 부호기가 갈린 뒤에도 일부 SB 는 우연히 같은 syntax 로 디코딩된다.
- 실측 (Neon1224 4K 프레임 17): 2040 SB 중 1954개 차이 → 86개 동일. 첫 차이가 `SB(0, 30)` 이므로
  앞의 30개가 동일, 즉 **첫 차이 뒤에 동일한 SB 가 56개**다.
- 실측 (WorldCup 512x288 프레임 1): 40 SB 중 36개 차이, 첫 차이 `SB(0, 1)` → **첫 차이 뒤 동일 SB 3개**.
- 중간점이 그중 하나에 떨어지면 "여기까지 같다"로 읽고 **진짜 첫 차이를 건너뛴다.**

게다가 **이진 탐색은 더 싸지도 않다.** 어떤 SB 가 *첫* 차이임을 증명하려면 그 앞을 전부 비교해야
하므로, 앞에서부터 훑고 첫 hit 에서 멈추는 것이 O(k) 로 최소다. 이진 탐색은 log n 탐침 **더하기**
앞부분 전수 확인이 필요하다. 실측에서 2040개 중 31개만 보고 끝났다.

## 4.6 마무리 (2026-10-04) — 창에 3·4단계와 D 를 배선하고, 찾은 지점으로 이동한다

Knight 가 정한 범위: **창에 3·4단계 연결, 5.1 이동·활성화, 5.2 A/B 토글, D recon 비교.**

### 코드 배치

| 무엇 | 어디 | 비고 |
|---|---|---|
| 프레임 매핑 + 블록 로더 + 4단계 walk | `src/integration/StreamDiffSteps` | CLI 에서 옮겨 왔다. GUI 와 CLI 가 같은 코드를 쓴다 |
| recon 산수 (plane 별 SSE, 첫 차이 위치) | `src/diff/ReconDiff` (Qt 없음) | 단위 테스트 `tests/unit/recon-diff.cpp` |
| recon 디코딩 루프 | `StreamDiffSteps::runReconStep` | item 의 자체 디코딩 경로 (`loadFrame` + 핸들러 raw 버퍼) |
| D CLI | `tools/cli/stream-diff-recon` | `--first` 는 첫 차이 프레임에서 중단 |
| 창 | `src/integration/StreamDiffWindow` | 1-2 → 3 → 4 → (D) 를 한 워커 스레드에서 차례로 |
| 이동 | 패치 `0060` | `splitViewWidget::selectBlockAtItemPixel` + `MainWindow::showStreamDiffLocation` |

**리팩터 검증:** `stream-diff-blocks` 를 공유 코드로 옮긴 뒤 WorldCup(`--tu 1 --obu 1`, `--frame 3 --all`),
PierSeaSide(`--tu 90 --obu 2`) 출력을 이전 바이너리 출력과 diff 했다. **바이트 동일**, 단 하나 —
`--all` 에서 `earliest superblock differing at all` 줄이 첫 블록 차이 **뒤의** SB 를 가리키던 것을
고쳤다 (WorldCup 프레임 3: 답 `SB(0, 1)` 인데 `SB(0, 3)` 을 "earliest" 로 찍었다). 이제 답보다 앞설 때만 찍는다.

### 창이 하는 일

- 한 워커가 1-2단계(헤더), 3단계(payload), 4단계(3단계가 지목한 프레임)를 차례로 돌린다.
  4단계의 프레임 매핑은 1-2단계에서 A 를 파싱한 **같은 모델**로 만든다 — 4K 를 두 번 파싱하지 않는다.
- 4단계는 **사용자의 playlist item 이 아니라 전용 item** 을 연다. 디코딩은 item 의 디코더를 움직이고,
  뷰어가 자기 스레드에서 같은 item 을 몰고 있기 때문이다.
- 결과는 단계별 최상위 노드 4개(헤더 / payload / 블록 / 복원 영상)의 트리. 블록·SB·프레임 행은
  **더블클릭하면 그 지점으로 이동**한다. 문법 요소 행은 부모 블록 행의 위치로 간다.
- **Go to first difference**: 4단계의 첫 차이 블록, 없으면 D 의 첫 차이 프레임(첫 차이 luma 픽셀).
- 창을 닫으면 진행 중인 작업을 취소한다. D 체크를 해제하면 **D 만** 멈춘다.

### 5.1 / 5.2 구현 — 설계에서 바뀐 점

- **Block info 토글 = playlist 선택 순서.** 0058 이후 Block Info·hexdump 패널은 두 스트림을 함께
  보여 주고, 첫 열은 **playlist 첫 선택**이다. 그래서 토글은 대상 스트림을 첫 선택(왼쪽 뷰, 첫 열)으로
  두고 다른 스트림을 둘째 선택으로 남긴다. 블록 선택은 view 0 에 한다. B 를 고르면 split view 의
  좌우가 바뀐다 — 의도된 동작이다.
- **Syntax info 토글**은 Bitstream Analysis 패널에 `currentSelectedItemsChanged(syntaxStream, …)` 를
  직접 넣는다. Block info 와 같은 스트림이면 부르지 않는다 (같은 파일을 두 번 파싱하지 않기 위해).
- 토글을 바꾸면 **마지막으로 이동한 지점**을 새 스트림으로 다시 보여 준다.
- 이동 자체는 마우스 이벤트를 흉내내지 않는다. `selectBlockAtItemPixel` 은 `selectBlockAt` 에서
  좌표 해석만 뺀 꼬리이고, 같은 `blockSelected` 신호를 낸다 — 클릭과 구분되지 않는다.

### D — 설계에서 바뀐 점과 실측

설계는 "difference 아이템 경로 재사용"이었다. **디코딩 경로는 재사용하고 산수만 새로 했다.**
difference 아이템은 MSE 를 **문자열**(`"MSE/PSNR Y"`)로 내고 매 프레임 RGB 변환을 거친다 — 프레임별
표에는 둘 다 맞지 않는다.

| 스트림 | 범위 | 시간 | 결과 |
|---|---|---|---|
| WorldCup 512x288 8-bit | 130 프레임 | 1.0 s | 첫 차이 **프레임 1**, 129/130 차이 |
| PierSeaSide 4K 10-bit | `--first` | 17 s | 첫 차이 **프레임 90** |
| PierSeaSide 4K 10-bit | 130 프레임 | 22 s, RSS 0.98 GB | 36/130 차이 |

**독립 검산:** WorldCup 두 스트림을 ffmpeg 로 yuv 디코딩해 Python 으로 계산했다. 프레임 1·3·8 의
Y SSE(274552 / 377295 / 35039), 차이 샘플 수, 첫 차이 픽셀, 차이 프레임 수(129) 모두 일치.

**D 가 4단계와 다른 답을 내는 것은 정상이다.** WorldCup 은 3단계가 TU 1 / OBU 1 (hidden ARF, 표시
프레임 16)을 지목하지만 픽셀은 **프레임 1** 부터 다르다 — 프레임 1 이 그 ARF 를 참조하기 때문이다.
PierSeaSide 도 같다: 비트스트림 첫 차이는 표시 91, 픽셀 첫 차이는 표시 90.

### 함께 고친 것 — dav1d 메모리 (패치 YUView `0059`, dav1d `0002`)

D 를 4K 로 돌리자 RSS 가 **7.2 GB** 였다. 원인이 둘 겹쳐 있었다.

1. **YUView 가 `dav1d_picture_unref` 를 한 번도 부르지 않았다.** `Dav1dPictureWrapper::clear()` 는
   `memset` 만 한다. 디코딩한 프레임마다 picture 참조가 하나씩 남았다 (4K 10-bit 프레임당 ~25 MB).
   → `0059`: 다음 picture 를 받기 전과 decoder close 전에 unref. `curPicture` 를 값 초기화 —
   생성자 경로의 `resetDecoder()` 가 첫 디코딩 전에 불려 쓰레기 `ref` 를 unref 하다 죽었다.
2. **포크가 analyzer 저장소(`blk_data` / pred / pre_lpf)를 할당만 하고 해제하지 않았다.** 통계를
   켜면 프레임당 ~30 MB 가 따로 샜다. → dav1d `0002`: picture 와 함께 해제.

1 을 고치자 **숨어 있던 버그가 드러났다.** WorldCup 프레임 16 의 블록 통계가 실행마다 달라졌다 (A 대
A 비교에서도 차이). valgrind: Invalid read 0건, `calculateIntraPredDirection` 이 **미초기화 값**에
의존 4196건. 포크가 analyzer 저장소를 0 으로 채우지 않고, 디코더가 모든 셀의 모든 필드를 쓰지는
않는다. 누수가 있을 때는 해제가 없어 늘 OS 의 새 (0) 페이지를 받았기 때문에 드러나지 않았다.
→ `0002` 가 할당 시 0 으로 채운다. 이후 5회 반복 출력 동일, 수정 전 기준 출력과 바이트 동일.

| 4K PierSeaSide, 디코딩 40 프레임 | 수정 전 | 수정 후 |
|---|---|---|
| 통계 끔 | 1236 MB (계속 증가) | ~450 MB 평탄 |
| 통계 켬 | 1665 MB (계속 증가) | ~740 MB 평탄 |

### 3단계가 IVF 를 받는다

OBU 워커가 컨테이너 없는 `.av1` 만 받았다. 회귀 테스트와 libaom/ffmpeg 산출물이 전부 IVF 라 창에서
3·4단계가 돌지 않았다. IVF 프레임 헤더를 건너뛰고, **IVF 프레임 하나를 temporal unit 하나로** 센다
(delimiter 가 없어도). 오프셋은 파일 오프셋 그대로. 검산: WorldCup `.av1` 쌍을 `ffmpeg -c copy -f ivf`
로 다시 감싸 돌린 결과가 `.av1` 과 같다 (TU 1 / OBU 1 → 표시 16 → `SB(2, 0)`, `MI(32, 8)`).

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

1. 블록 조회를 계약대로(메인 스레드 밖에서) 부르고, 완전성을 **픽셀 커버리지**로 자체 검사 (3장) — 완료
2. Find diff 다이얼로그 + 헤더 구문 비교 (A) — 완료
3. SB 24bit 비교 (B) — 4.5 에서 payload 비교로 대체
4. 블록 구문 비교 (C) — 완료 (CLI 10-01, 창 10-04)
5. recon 비교 배선 (D) — 완료 (10-04, 4.6)
6. 5.1 이동·활성화, 5.2 A/B 토글 — 완료 (10-04, 4.6)

각 단계마다 회귀 테스트를 하나씩 붙인다. 비교 로직의 알맹이(정규화·정렬·판정)는 Qt 없는
코어(`src/diff/`)에 두고 단위 테스트한다 — GUI 없이 검증할 수 있어야 한다.

## 7. 미검증 / 위험

- **24bit 폭의 근거는 사용자 경험이다.** 왜 24인지 스펙적 근거를 확인하지 않았다. 폭을
  파라미터로 두고 기본값 24 로 한다.
- SB 첫 블록의 `startBit` 을 SB 시작으로 쓰는 것은 **측정으로 확인했다** (512x288, SB=64, 프레임당
  40개). min-startBit 이 raster 순서로 **단조 증가**하고, SB 구간은 대체로 겹치지 않는다.
  **다만 프레임 1에서 4곳이 겹쳤다.** 비트 범위가 산술 디코더의 읽기 위치이기 때문이며, 경계가
  비트 단위로 정확하지 않다는 뜻이다. B 가 판정이 아니라 탐색인 또 하나의 이유다 —
  기준점 자체가 ±수 비트 흔들릴 수 있으므로 24bit 창은 "이 근처" 이상을 주장하지 않는다.
- 두 스트림의 해상도·프레임 수가 다르면 비교는 거절한다. 판정 규칙 미정.
- `bitstreamRange` 는 analyzer dav1d 디코더에서만 나온다. FFmpeg 폴백 스트림은 B 를 못 쓴다 —
  그 경우 A 와 C 만 돌린다.
- 프레임 수가 많고 해상도가 크면 C 는 비싸다. 4x4 격자 전수 훑기의 비용을 재지 않았다.
  → 4.5 에서 측정하고 줄였다 (4K 한 프레임 22.8 s).
- **이동(5.1)은 헤드리스 회귀 42 로만 확인했다.** 선택 순서·프레임·Block Info 상태 문자열을 본다.
  화면의 하이라이트와 hexdump 가 실제로 그 블록을 가리키는지는 **GUI 에서 눈으로 확인 미실시**.
- Syntax info 토글이 Bitstream Analysis 패널을 실제로 바꾸는지는 회귀에서 검사하지 않는다
  (위젯의 현재 item 이 private). GUI 확인 필요.
- D 는 두 스트림의 **표시 프레임 수가 다르면 짧은 쪽까지만** 비교한다. 거절하지 않는다.
- 5.1 의 이동·활성화는 **upstream 패치가 필요하다** (`splitViewWidget` 에 블록 선택 public slot).
  `blockSelected` 신호와 `setSelectedBlock` 슬롯은 이미 있으므로 추가 범위는 그 하나다.
- A/B 토글을 바꿀 때 해당 스트림에 그 좌표의 블록이 없는 경우는 **정상 스트림에서는 발생하지
  않는다** (확인됨). 좌표는 유지하고 **"블록 없음"** 을 표시하는 것으로 확정한다.
