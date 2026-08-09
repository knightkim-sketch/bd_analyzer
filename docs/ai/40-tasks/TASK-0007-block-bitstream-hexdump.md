---
title: TASK-0007 블록 비트스트림 hexdump + 슈퍼블록 통계
status: implemented
created: 2026-08-08
updated: 2026-08-09
author: claude-opus-5
verified: "빌드·회귀 17/17·헤드리스 프로브·GUI 클릭 확인, 배포본 실행 확인까지 완료"
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
dav1d: ChristianFeldmann/dav1d @ 0.2.1.0.analyze (0a7f210) + 자체 패치
---

## 배경

TASK-0006 종료 시점(커밋 `a1c7a8d`, 패치 `0021`)에서 다른 에이전트(codex)가 "블록 클릭 시
해당 bitstream hexdump 를 보여주는" 기능을 추가했으나 **동작하지 않는 상태**로 남아 있었다.
사용자 신고: *"첫 frame 의 frame stream dump 는 보이지만, block click 시 block hexdump 가
동작하지 않고, 다른 frame 선택 시 hexdump 가 아예 동작하지 않는다."*

원인 분석에서 시작해 기능 완성, 통계 타입 추가, UI 정리까지 이어진 작업 기록이다.
모든 설계 결정은 사용자와 합의됐다.

관련 커밋 (bd_analyzer `main`):

```
616b7b2  Build the dav1d analyzer decoder from source
8ac8be8  Add the block bitstream hexdump and superblock statistics   (패치 0022)
1f0994b  Add regression tests for the bitstream dump and overlay grouping
a5988cc  Commit the runnable deployment bundle
b9d7e33  Make the pixel analysis follow the displayed frame
262b882  Move the luma histogram back to the bottom right
```

upstream 수정은 전부 **패치 `0022`** 한 장에 들어 있다. codex 의 원본 상태를 in-place 로
수정해 버려서 중간 상태를 복원할 수 없었고, 쪼개면 추측이 섞이므로 단일 패치로 뒀다.

---

## A. 최초 신고 3건의 원인 — 전부 다른 문제였다

### A-1. 블록 hexdump 가 안 나옴 → **죽은 코드**

`getItemDataDump()` 는 `blockInfo.bitstreamRange` 가 있을 때만 블록 슬라이스를 만든다.
그 유일한 생산자가 `decoderDav1d.cpp` 의 `addBlockBitstreamRange()` 인데,

```c
#if defined(YUVIEW_DAV1D_AV1BLOCK_HAS_BITSTREAM_RANGE)
```

이 매크로가 **리포지토리 어디에도 정의돼 있지 않았다.** 전체 grep 결과 이름이 나오는 곳은
`blockData.h` 의 `#if` 와 `decoderDav1d.cpp` 의 `#if` 두 군데뿐. 어떤 `.pro` 에도
`DEFINES +=` 가 없었다. 즉 항상 프레임 전체 덤프로 폴백.

**매크로만 켜면 되는 문제가 아니었다.** `bin/decoder/libdav1d-internals.so` 는 prebuilt
바이너리였고 그 `Av1Block` 에는 해당 필드가 없다. 켜면 YUView 쪽 struct 만 커져
`blockData[y * b4_stride + x]` 의 stride 가 어긋나 **모든 블록 통계가 조용히 쓰레기**가 된다.
→ dav1d 를 소스에서 빌드하는 것이 전제 조건 (B 항목).

### A-2. 다른 frame 에서 동작 안 함 → **fd 누수 + 매핑 오류**

`FileSourceFFmpegFile::~FileSourceFFmpegFile()` 이 packet 만 free 하고
**`avformat_close_input` 은 심볼만 resolve 된 채 어디서도 호출되지 않았다.**
`openInput()` 주석은 "wrapper takes ownership" 이라고 하지만 아무도 해제하지 않는다.

원래는 아이템당 1개만 살아 문제가 없었는데, 덤프가 **프레임 변경마다 / 블록 클릭마다**
새 리더를 만들면서 노출됐다. 실측:

```
20회 → +20 fd,  100회 → +100 fd,  1300회 → +1300 fd
```

soft limit 도달 후에는 `Unable to reopen the input file for packet dump.` 만 나온다.

gdb 로 잡은 재생 중 누수 경로 (프레임 덤프와 무관한 별개 건):

```
PlaybackController::goToNextItem()          # 시퀀스 끝
 → PlaylistTreeWidget::selectNextItem()     # 아이템이 1개라 자기 자신 재선택
 → selectionRangeChanged
 → BitstreamAnalysisWidget::restartParsingOfCurrentItem()
 → ParserAVFormat::runParsingOfFile()       # 로컬 FileSourceFFmpegFile
 → 재생 라운드당 test.ivf 핸들 +11
```

`getItemDataDump` 을 완전히 무력화한 빌드로도 증가율이 동일해 덤프와 무관함을 확정했다.

### A-3. frame index → packet 매핑

- **AnnexB**: `getFrameStartEndPos()` 는 `FrameIndexCodingOrder` 로 `frameListCodingOrder` 를
  인덱싱하는데, 뷰가 주는 `frameIdx` 는 **표시 순서**다. 디코딩 경로가 별도의
  `readAnnexBFrameCounterCodingOrder` 를 쓰는 이유가 이것. B 프레임이 있으면 다른 프레임의
  바이트가 나온다 (`test.h264` 로 확인: display frame 1 → byte 5455, frame 4 → byte 3443).
- **libavformat**: 매 요청마다 파일 처음부터 `frameIdx+1` 개 패킷을 재demux. 마지막 프레임에서
  파일 끝을 넘어감 (`getDecodableFrameLimits()` 가 `nrFrames` 를 마지막 인덱스로 반환하는
  upstream off-by-one 때문에 유령 프레임이 하나 생긴다 — 이건 영향 범위가 커서 손대지 않고
  정확한 메시지만 내도록 했다).

---

## B. dav1d analyzer 디코더 소스 빌드 — **완료** (커밋 `616b7b2`)

`libdav1d-internals.so` 의 버전 문자열 `0.2.1.0.analyze-0-g0a7f210` 이
**ChristianFeldmann/dav1d 태그 `0.2.1.0.analyze` (커밋 `0a7f210`)** 와 정확히 일치.
무수정 셀프 빌드가 드롭인 교체품임을 회귀 통과로 먼저 확인한 뒤 패치했다.

업스트림 fork 에는 `export_bitsperblk` / `export_bitsused` 플래그가 있지만 주석에
`Not implemented yet` 이라고 적혀 있고 구현이 없다. `third_party/dav1d/0001-block-bitstream-range.patch`
가 그 자리를 채운다.

| 추가 | 위치 |
|---|---|
| `Av1Block::bitstream_start_bit` / `bitstream_end_bit` | `src/levels.h`, `src/decode.c` |
| `Av1Block::sb_qindex` | `src/levels.h`, `src/decode.c` |
| `Av1Block::sb_bit_count` | `src/levels.h`, `src/decode.c` |
| `MsacContext::buf_start` / `abs_bit_offset` + 헬퍼 | `src/msac.h`, `src/msac.c` |
| `dav1d_analyzer_block_data_size()` | `include/dav1d/dav1d.h`, `src/lib.c` |

빌드: `./scripts/setup-dav1d.sh` (meson 은 `pip3 install --user meson==1.2.3`, nasm 이 없어
`-Dbuild_asm=false`). 소스는 submodule 이 아니라 shallow clone 이며 gitignore 된다.

### 왜 pointer 가 아니라 offset 인가

사용자가 "file/buffer 포인터도 넣으면 더 빠르지 않겠냐" 고 물었으나 **오프셋만** 넘겼다.
포인터를 struct 에 넣으면 버퍼 해제 후에도 읽을 수 있어 use-after-free 가 되기 쉽고,
속도 이득(재파싱 회피)은 오프셋으로도 동일하다.

연결 방식이 핵심이다. YUView 는 packet 을 OBU 단위로 쪼개 push 하므로 dav1d 가 보는 버퍼는
packet 이 아니다. push 할 때 `Dav1dData.m.offset` 에 **그 OBU 의 packet 내 위치**를 넣으면
dav1d 가 `dav1d_data_ref` 에서 `m` 을 tile 데이터까지 복사해 들고 간다. 그래서 `decode_b` 가
보고하는 위치는 이미 **packet 좌표**이고, "디코딩된 프레임이 어느 push 버퍼에서 나왔는지"
추적할 필요가 없다 — 오프셋이 데이터와 함께 흘러간다.

### 정확도 — 산술 부호화의 한계

AV1 은 블록 syntax 를 multi-symbol 산술 부호기(msac)로 코딩하므로 **블록 고유의 비트 경계는
존재하지 않는다.** 보고값은 msac 이 그 블록을 디코딩하며 소비한 구간이다.

- 비트 소비량 계산 자체는 정확하다: `(buf_pos - buf_start)*8 - cnt - 15`.
  `dav1d_msac_init` 이 `cnt = -15` 로 시작하고 `ctx_norm` 이 심볼당 사용 비트만큼 깎기 때문.
- 검증: `test.ivf` frame 0 에서 블록 구간 합 **17323 bit** vs 실제 tile 데이터 약 17568 bit.
  frame 1(4개 frame OBU 중 마지막이 표시 프레임)은 **2994** vs 약 3208 bit.
- 한 블록이 **5 bit** 로 끝나는 것은 정상이다 (평탄 영역 skip 블록).

### 디코딩 정확성 — libaom/svt-av1 참고 수정 불필요

패치는 필드 추가와 위치 기록만 하므로 디코딩 경로를 바꾸지 않는다. ffmpeg 내장
libdav1d **1.x** 와 **bit-exact**:

```
test.ivf   e5b6aca92451…  MATCH      big.ivf    bdab98e18704…  MATCH
sb128.ivf  d94620f9baf9…  MATCH      test2.ivf  e5b6aca92451…  MATCH
```

### ⚠️ ABI — 반드시 짝을 맞출 것

세 필드가 `Av1Block` 끝에 붙어 `sizeof` 가 **32 → 56 바이트**로 바뀐다. YUView 는 `blk_data` 를
자기 `sizeof` 로 인덱싱하므로 불일치 시 **에러 없이** 모든 블록 통계가 틀어진다.

- `YUViewLib.pro` 가 `YUVIEW_DAV1D_AV1BLOCK_HAS_BITSTREAM_RANGE` 를 정의한다.
- `decoderDav1d::resolveLibraryFunctionPointers()` 가 `dav1d_analyzer_block_data_size()` 를
  호출해 자기 `sizeof` 와 비교하고, 다르면 **로딩을 거부**한다.
- 실제로 구버전 `.so` 를 넣어 검증: Dav1d 거부 → FFMpeg 폴백, 블록 통계 0개,
  hexdump 는 "Frame Bitstream" 폴백. **쓰레기 대신 조용한 부재.**

라이브러리와 `YUViewLib` 는 함께 재빌드해야 한다:
`./scripts/setup-dav1d.sh && ./scripts/build.sh && ./scripts/make-bin-bundle.sh`

---

## C. 비트스트림 hexdump — **완료** (패치 `0022`)

- **프레임 덤프**: libavformat 경로는 전용 리더를 아이템 수명 동안 유지하고 위치를 기억한다.
  앞으로 이동 = 패킷 1개, 뒤로 점프할 때만 `seekFileToBeginning()`. 프레임 인덱스 정의는
  기존 규칙(`YUView frame index` = video packet index, TASK-0006 E-1)을 그대로 따른다.
- **AnnexB**: `ParserAnnexB::getFrameStartEndPosDisplayOrder()` 를 추가했다. 두 인덱스 타입이
  모두 `unsigned` 라 오버로드가 불가능해 이름으로 구분했다.
- **블록 덤프**: 산술 부호화 특성상 블록이 1바이트로 끝나는 일이 흔해, 블록 바이트 앞뒤
  **문맥 32바이트**를 함께 보여주고 주소 컬럼을 **packet 절대 오프셋**으로 바꿨다.
  블록 자신의 바이트는 hex/ASCII 양쪽 모두 **빨간 볼드**로 표시한다
  (`QTextCursor::mergeCharFormat`, plain text 유지 → 복사 가능).

독립 검증 — 자체 OBU 파서로 TU 구조를 확인:

```
TU 1: TEMPORAL_DELIMITER, FRAME@2, FRAME@1471, FRAME@2182, FRAME@2738
      보고된 블록 범위 = byte 2758..  → 마지막(표시) FRAME OBU 안 ✓
```

숨겨진 ALTREF 3개가 섞인 temporal unit 에서도 표시 프레임의 OBU 를 정확히 지목한다.

---

## D. 통계 타입 변경 — **완료** (패치 `0022`)

### 현재 등록 타입 (13개, 소스 실측)

```
12 sb_qindex          13 sb_bitcount        (← 신규, 목록 맨 앞)
 0 Pred Mode           1 Segment ID          2 skip            3 skip_mode
 4 intra pred mode (Y)                       5 intra pred mode (UV)
14 ref frame index 0                        15 ref frame index 1
24 Motion Vector 0 (빨강 w2)                25 Motion Vector 1 (파랑 w2)
26 Transform Size
```

TASK-0006 대비: `chroma from luma alpha U/V` (12, 13) **제거**, 그 ID 를 재사용해
`sb_qindex` / `sb_bitcount` 추가.

- **`sb_qindex`** — 스펙의 `CurrentQIndex`. ffmpeg `-bsf:v trace_headers` 와 대조:
  frame 0 (KEY) `base_q_idx=22` → 보고 **22**; frame 1 은 TU 안 4개 OBU 의 72/100/114/**128**
  중 표시 프레임 값 **128** 을 정확히 골라낸다.
- **`sb_bitcount`** — `decode_sb()` 호출 전후 msac 위치 차이. 슈퍼블록 심볼은 tile 데이터에서
  연속하므로 **정확한 총합**이고 파티션 트리 비용까지 포함한다. `test.ivf` frame 0 에서
  슈퍼블록 9개 합 **17569 bit** vs tile 데이터 약 17568 bit.

### 오버레이 체크박스 그룹화 — 13 타입 → **9 체크박스**

`StatisticsType::uiGroup` 을 추가하고 `StatisticUIHandler` 가 타입이 아니라 **그룹당 한 행**을
만든다. 타입은 분리된 채라 오버레이 색과 syntax 표 행은 그대로다.

```
sb_qindex, sb_bitcount, Pred Mode, Segment ID, skip, skip_mode,
ref frame index, Motion Vector, Transform Size
```

- `Pred Mode` ← Pred Mode + intra pred mode (Y) + (UV)
- `ref frame index` ← 0 + 1,  `Motion Vector` ← 0 + 1

**트레이드오프**: 스타일 편집 버튼은 그룹의 첫 타입을 편집한다. 즉 intra pred mode Y/UV 의
색상 맵은 UI 에서 직접 편집할 수 없다.

### syntax 표 정렬 기준 변경

`typeID` 정렬 → **등록 순서** 정렬. 두 패널 순서가 일치하고, 새 타입이 ID 숫자에 따라
엉뚱한 위치에 끼지 않는다.

### intra pred mode 중복 행 제거

`intra pred mode (Y)` 가 두 번 나오던 것은 버그가 아니라 **한 타입이 값(모드)과 벡터(예측 방향)를
동시에** 담기 때문이었다 (TASK-0006 B 에서 의도한 설계). 방향 벡터가 0 이 아닐 때만 나오므로
"경우에 따라" 중복돼 보였다. 기존 L0/L1 병합과 같은 방식으로 한 행에 접었다:

```
intra pred mode (UV)   VERT_PRED (1)  → (0,16)
```

---

## E. 그 밖의 기능/수정 — **완료**

| 항목 | 내용 |
|---|---|
| **raw AV1 확장자** | `.av1` / `.obu` 가 확장자 목록에 없어 **아무 일도 안 일어났다** (`guessFileTypeFromFileAndCreatePlaylistItem` 의 hard gate). 추가 후 `.ivf` 와 픽셀 해시 동일 확인 |
| **픽셀 분석이 프레임을 따라가지 않던 버그** | 히스토그램은 frame 0 만, 블록 픽셀 통계는 "computing..." 에서 멈춤. 원인은 `showRawData()` 가 **고배율일 때만** true 라 `currentFrameRawData` 가 갱신되지 않은 것. `setRawValuesRequested()` 추가(Frame Info 독 표시 여부에 연동) + `FrameInfoWidget` 이 로드 완료 후 오는 **두 번째 알림을 버리던 것** 수정. 둘 다 필요했다 |
| **레이아웃** | 블록 픽셀 통계 → 좌측 Block Syntax 아래 / 히스토그램 → 우측 하단 Frame Info / Info pane 시작 시 창 높이 1/2 상한(`clampInitialLeftDocks`, 축소만) / Info 스크롤 영역 `AdjustToContents` → `AdjustIgnored` (OBU 트리 도착 시 패널이 커지던 원인) |
| **clean 빌드 실패** | `playlistItemRawFile.cpp` 가 컴파일되지 않는데 `.o` 의 mtime 이 더 최신이라 make 가 건너뛰고 **예전 오브젝트를 링크**하고 있었다. 그 ABI 불일치가 회귀 12번을 crash 시키고 있었음 |
| **배포 번들 커밋** | `bin/` 을 커밋 대상으로 전환 (사용자 지시). 전역 `*.so` 규칙이 번들 라이브러리를 전부 삼키던 것과 런타임 캐시 `.YUViewBD` 혼입을 잡았다 |

---

## 완료 상태

- A~E 전부 구현·검증 완료. bd_analyzer `main` 및 GitHub `origin/main` 반영 완료 (`262b882`).
- 케이스 스냅샷(`ai-program` 저장소)에도 반영 완료 (`acf0b35`). 단 그쪽 `main` 은 GitLab
  **protected branch** 라 직접 push 불가 — MR 필요.
- `/fs2/install_devel/YUViewer/` 배포본 복사·검증 완료. 다만 이전 설치본에 있던 샘플
  `test.ivf` 는 빠져 있다 (`bin/` 에 포함되지 않으므로).

## 남은 것 / 추후 개선 후보

1. **`getDecodableFrameLimits()` off-by-one** — `range.second = nrFrames` 라 프레임이 하나
   더 있는 것처럼 보인다. 재생 범위·캐싱·슬라이더 전체에 영향이 가서 손대지 않았다.
2. **재생 중 fd 누수 (별개 건)** — 시퀀스 끝마다 bitstream analysis 가 전체 재파싱된다.
   `avformat_close_input` 수정으로 누수 자체는 막혔지만 **매번 전체 재파싱하는 낭비**는 남아 있다.
3. **그룹 내 타입별 스타일 편집** — 위 D 의 트레이드오프.
4. **작은 창에서의 Info pane 상한** — 사용자가 "충분히 큰 창에서만 해당" 이라고 범위를 정했다.
   작은 창에서는 playlist/info dock 이 이미 Qt 최소 크기라 더 줄지 않는다.

## 검증

- `./scripts/build.sh` 통과 (clean 빌드 포함), `./tests/run-regression.sh` → **PASS 17 FAIL 0 SKIP 0**.
- 신규 회귀 테스트 5종:
  - `13-frame-bitstream-dump` — 프레임별 패킷 일치(ffprobe 대조), 랜덤 액세스 = 순차 결과,
    300회 덤프 후 fd 증가 0, annexB display-order (B 프레임 스트림 생성해서 검사)
  - `14-block-bitstream-range` — 구간이 패킷 안에 있고 span 의 80% 이상을 설명, 클릭 시 슬라이스
  - `15-raw-av1-extension` — `.av1`/`.obu` 인식 + `.ivf` 와 픽셀 해시 동일
  - `16-statistics-ui-grouping` — 그룹 체크박스가 소속 타입 전부를 켜고 다른 그룹은 불변
  - `17-frame-info-follows-frame` — 히스토그램이 frame 5/10/2 를 따라감 (**수정 전 빌드에서 실패**)
- 패치 `0022` 3종 검증 통과: `0001`..`0022` 순차 clean 적용 / 워킹트리와 **바이트 차이 0** /
  재적용 거부.
- 배포본: `diff -rq --no-dereference` 차이 0, symlink 37개 보존·해석 정상,
  프로세스 매핑이 전부 `/fs2/install_devel/YUViewer/` 에서 로드(개발 트리 유입 0),
  GUI 블록 클릭 시 `sb_qindex 22` / `sb_bitcount 4819` 표시 확인.

> 검증 중 실제로 당한 것: 패치 `0022` 재생성 중 명령이 중간에 실패해 **이전 버전 패치가 남아
> 있었다.** 3종 검증의 "바이트 차이 0" 에서 `4` 로 잡혔다. 이 검증을 건너뛰었으면 clean clone
> 에서 변경이 통째로 빠진 채 빌드됐을 것이다. **패치를 만들면 반드시 3종 검증을 돌릴 것.**

## 인수인계

TASK-0006 의 「인수인계」 절(프로젝트 구조 / 패치 레시피 / 헤드리스 검증 패턴 / GUI 자동화
함정 / 코드 규약)이 그대로 유효하다. 이번 세션에서 추가로 확인한 함정 두 가지:

- **stale `.o` 가 컴파일 에러를 가린다.** `.o` 의 mtime 이 `.cpp` 보다 최신이면 make 가
  건너뛴다. 내 변경과 무관한 곳에서 회귀가 crash 하면 먼저 `rm -rf build/YUViewLib` 후 재빌드.
- **GUI 자동화는 이 환경에서 신뢰하기 어렵다.** 창이 터미널 뒤로 가려져 `import -window` 가
  실패하고, `kill -9` 로 종료하면 다음 실행에서 "Restore Playlist" 모달이 뜬다.
  `MainWindow` 를 직접 띄우는 헤드리스 프로브(테스트 17 참고)가 훨씬 결정적이다.
