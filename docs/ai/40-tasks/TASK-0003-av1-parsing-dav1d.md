---
title: TASK-0003 AV1 비트스트림 분석 활성화 + dav1d 블록 통계
status: in-progress
created: 2026-07-31
updated: 2026-07-31
author: claude-opus-5
verified: partial
---

## 목표

AV1 스트림을 YUView 에서 분석 가능하게 만든다. 두 층으로 나뉜다.

1. **신택스/OBU 분석** — FFmpeg 공유 라이브러리 필요 (없으면 AV1 이 아예 안 열린다)
2. **블록 단위 통계** (기능 A') — dav1d analyzer fork 필요

상세 근거는 조사 문서에 있다. 이 문서는 진행 상황만 추적한다.
→ [ffmpeg-integration.md](../10-research/ffmpeg-integration.md)
→ [dav1d-block-statistics.md](../10-research/dav1d-block-statistics.md)

## 완료 (실측 검증됨)

### 1. FFmpeg 7.1.2 shared 소스 빌드
- [x] nasm 2.16.03 로컬 빌드 (`~/opt/tools`) — Rocky 8 활성 repo 에 없고 sudo 도 없음
- [x] FFmpeg 7.1.2 → `~/opt/ffmpeg-7.1`, 외부 의존 6개(전부 OS 기본)뿐, `.so` 4개 14.9 MiB
- [x] `scripts/setup-ffmpeg.sh` 로 재현 가능 (idempotent, sudo 불필요)
- [x] 필수 심볼 36개 전부 resolve 확인
- [x] AV1 파싱 실동작: `test.ivf` packet 26개 / `sample.av1` packet 101개,
      OBU 분해 확인 (`OBUs: Frame Sequence Header Temporal Delimiter`)

### 2. dav1d analyzer fork 빌드 + 블록 통계
- [x] `ChristianFeldmann/dav1d` master (0.2.2, 2019-03-22) 가 gcc 8.5 + nasm 2.16.03 + meson 1.11.2 로 빌드됨
- [x] 벤더링된 헤더 5개가 fork 와 바이트 동일함을 diff 로 확인 (ABI 일치 근거)
- [x] `libdav1d-internals.so` 로 배치 → `statisticsSupported = TRUE`, 통계 타입 27개 선언
- [x] 블록 통계 실측: 176x144 → 값 4,915 / MV 186, 1920x1080 → 값 141,205 / MV 11,210

## AV1 디코딩 뷰 — dav1d 경로 **수정 완료 및 GUI 실측 확인** (2026-07-31)

두 디코더 경로가 각각 다른 이유로 막혀 있었다. **dav1d 경로는 고쳤고 GUI 에서 영상이 나온다.
FFmpeg 경로는 아직 깨져 있고, 그게 기본값이다.**

### ✅ 적용한 수정 — 코덱 ID 로 OBU 포맷 강제

패치: `third_party/yuview/patches/0001-av1-obu-packet-format-from-codec-id.patch` (9줄 추가)
`build.sh` 가 자동 적용한다 (idempotent).

```cpp
// FileSourceFFmpegFile::goToNextPacket()  (upstream :653)
if (this->getVideoStreamCodecID().isAV1())
  this->packetDataFormat = PacketDataFormat::OBU;   // 추측하지 않는다
else
  this->packetDataFormat = this->currentPacket.guessDataFormatFromData();
```

**수정 전후 실측:**
```
before:  getNextUnit[0..3] size=0  <-- EMPTY, atEnd=0 (무한 반복)   RESULT: BROKEN
after :  getNextUnit[0]=2  [1]=13  [2]=2205  [3]=2  [4]=1469 ...   RESULT: WORKS
         (2B = temporal delimiter, 13B = sequence header, 2205B = frame — 정상 OBU 분할)
```

**GUI 확인 (`Decoder = Dav1d`):** test.ivf 의 testsrc2 패턴이 정상 렌더링됨.
`Buffer 96%`, `Caching T2: 25` (25프레임 캐싱), 화면 타임코드가 `00:00:00.400` →
프레임 10 / 25fps = 0.4s 와 **정확히 일치** (디코딩 내용 정합성 확인).
dav1d 블록 통계 24종도 GUI 에 등록되고, 헤드리스 회귀도 그대로 PASS
(값 4,915 / MV 186).

### ✅ 해결 — `Decoder = FFMpeg` 의 AV1 실패는 **우리 FFmpeg 빌드 문제였다**

증상이었던 것:
```
decoder : FFmpeg   codec : av1
stopped : Error sending packet (avcodec_send_packet). Return code -38.
```

`-38` = `AVERROR(ENOSYS)`. **YUView 버그가 아니다.** FFmpeg 7.1 의 native `av1` 디코더는
**hwaccel 전용**이라 소프트웨어 디코딩을 못 한다 — `libavcodec/av1dec.c:659-668` 이
`!avctx->hwaccel` 이면 `"Your platform doesn't support hardware accelerated AV1 decoding."`
을 찍고 ENOSYS 를 돌려준다 (소스 주석: *"Since now the av1 decoder doesn't support native decode"*).
우리 첫 빌드에는 `libdav1d`/`libaom-av1` 둘 다 없었다.

→ `scripts/setup-ffmpeg.sh` 에 **dav1d 1.4.3 static 빌드 + `--enable-libdav1d`** 를 추가했다.
dav1d 는 `libavcodec.so` 안에 흡수되므로 외부 `.so` 는 늘지 않는다 (실측: NEEDED 에 libdav1d 없음).
`CONFIG_LIBDAV1D=yes` 검증 게이트도 스크립트에 넣었다.
→ 자세한 정정: [ffmpeg-integration.md](../10-research/ffmpeg-integration.md) 2절

**검증:** `decoderFFmpeg` 가 AV1 을 디코딩한다 — 176x144 및 1920x1080 모두 PASS.
부수 효과로 외부 의존이 오히려 줄었다 (libdrm 사라짐), 크기 14.9 → 16.8 MiB.

**래퍼 구조체 드리프트는 원인이 아니었다** (offsetof 실측): `AVCodecParameters_61`(24필드),
`AVCodecContext_61`(29), `AVPacket_59_60_61`(14), `AVStream_60_61`(17) 모두 **불일치 0**.
같은 래퍼로 VP9 디코딩이 정상 동작함도 확인됐다. `AVFrame_57_58_59` 만 16개 불일치
(4절의 기존 발견 재확인 — `quality` 실제 160 / 사본 168, `metadata` 실제 336 / 사본 368).

### ✅ AV1 기본 디코더 — dav1d 우선 + FFmpeg 폴백

패치: `patches/0004-av1-prefer-dav1d-with-ffmpeg-fallback.patch`

FFmpeg 이 AV1 을 디코딩할 수 있게 된 뒤에도 기본값은 바꿀 값이 있다:
**통계 타입이 dav1d 27종 vs FFmpeg 4종**이다 (기능 A' 의 본체).

단순 재정렬만 하면 **회귀가 된다.** `libdav1d-internals.so` 가 없을 때
`allocateDecoder` 실패 → 생성자가 `fillStatisticList`/`seekToPosition`/
`signalRequestRawData` 연결 **전에** `return` → **아이템이 영구히 죽는다.**
디코더 콤보박스로 재선택해도 살아나지 않는다 (연결이 애초에 안 만들어졌으므로).
그래서 **생성자 안에 1회 폴백**을 넣었다.

실측 (두 라이브러리 상태 모두):
```
libdav1d 있음 : engine=Dav1d   startEndRange=(0,25)  stat types=27
libdav1d 없음 : engine=FFMpeg  startEndRange=(0,25)  stat types=4    <- 아이템 살아있음
H.264        : engine=FFMpeg  stat types=4                          <- 불변
```

`codec == Codec::AV1` 로 가드했으므로 HEVC/VVC 기본값은 그대로다.
헤더의 `DecodersAV1` 순서도 함께 바꿨다 — 설정 다이얼로그가 index 0 을 기본으로 저장하기 때문이다.

### 해결된 근본 원인 기록 — `getNextUnit()` 이 빈 데이터만 반환했다

증상: `Decoder = Dav1d` 로 띄우면 통계 타입은 등록되는데 화면이 **"Loading..." 에서 멈췄다.**
원인은 디코더가 아니라 데이터 공급 경로였다. `playlistItemCompressedVideo.cpp:770` 의
"FFmpeg 입력 + 非FFmpeg 디코더" 분기가 `getNextUnit()` 을 쓴다.

**정확한 사슬 (실측):**
```
AVPacketWrapper::guessDataFormatFromData()  ->  Unknown        <-- 여기서 틀린다
  (AV1 패킷 첫 8바이트: 12 00 0a 0b 00 00 00 03 — 유효한 OBU. TD(size 0) + SEQ_HDR(size 11))
  네 검사가 모두 실패: checkForRawNALFormat(strict) / checkForMp4Format /
  checkForObuFormat / checkForRawNALFormat(loose)   (AVPacketWrapper.cpp:348-355)
        |
FileSourceFFmpegFile::getNextUnit()  ->  packetDataFormat 가 Unknown 이면
  RawNAL/MP4/OBU 세 분기 어디에도 안 걸리고 (FileSourceFFmpegFile.cpp:118,152,179)
  그냥 오래된 lastReturnArray(빈 값)를 반환한다
        |
dav1d 가 데이터를 못 받아 굶는다  ->  "Loading..." 무한 대기
```

> `checkForObuFormat` 이 **왜** false 를 돌려주는지는 함수 내부까지 좁히지 않았다 (미검증).
> 코덱 ID 로 우회했으므로 이 경로는 더 이상 타지 않지만, **함수 자체의 버그는 그대로 남아 있다.**
> 다른 코덱에서 같은 오판이 날 수 있으므로 별도로 파볼 가치가 있다.

남은 개선 여지:
- `getNextUnit()` 에 `Unknown` 방어 분기를 추가하면 좋다. 지금은 포맷이 Unknown 이면
  세 분기 어디에도 안 걸려 **빈 배열을 조용히 무한 반환**한다 (디코더가 굶는데 EOF 도 아님).
  최소한 에러를 내야 한다

### ✅ 수정한 크래시 — 통계 렌더링을 켜면 YUView 가 abort 했다

패치: `third_party/yuview/patches/0002-dav1d-intra-pred-direction-bounds-check.patch`

GUI 에서 통계 체크박스(`Motion Vector 0/1`)를 켜자 즉시 죽었다:
```
decoderDav1d.cpp:1060: calculateIntraPredDirection(IntraPredMode, int):
Assertion `deltaIndex >= 0 && deltaIndex < 8' failed.     -> Aborted (core dumped)
```

`calculateIntraPredDirection` (`decoderDav1d.cpp:1051`) 은 `angleDelta` 가 -4..3 이라 가정하고
`modeIndex`/`deltaIndex` 로 **고정 8x8 `vectorTable`** 을 인덱싱한다 (`:1091`).
실제 스트림이 그 범위를 벗어난다. 호출부는 `cacheStatistics` 안의
`decoderDav1d.cpp:958` (`b.y_angle` / `b.uv_angle` 를 그대로 넘긴다).

**assert 로는 부족한 이유 (중요):** 이 빌드는 `QT_NO_DEBUG` 만 정의하고 **`NDEBUG` 는 정의하지 않아**
지금은 깔끔히 abort 한다. 그러나 `-DNDEBUG` 로 빌드하면 assert 가 사라지고
**`vectorTable` 을 범위 밖으로 읽는다** (조용한 메모리 오류). 그래서 assert 를 고치는 대신
early-return 방어로 바꿨다 — 이 함수는 이미 `DC_PRED` 와 범위 밖 예측모드에 대해 `{}` 를 돌려주므로
동작이 일관된다 (방향 벡터를 그리지 않음).

**회귀 검증 — 통계 수치가 수정 전후 완전히 동일:**
```
test.ivf   (25프레임) : 타입 26개, 블록 값 94,824    / MV 10,420      (before == after)
sample.av1 (100프레임): 타입 27개, 블록 값 6,949,551 / MV 553,922     (before == after)
```
즉 방어 코드는 정상 경로를 전혀 건드리지 않는다. 수정 후 GUI 재현 시나리오도 abort 없이 통과.

> ⚠️ **왜 `angleDelta` 가 범위를 벗어나는지는 규명하지 못했다 (미검증).**
> 헤드리스로 두 스트림 125프레임을 전부 디코딩(통계 활성)해도 재현되지 않고 GUI 에서만 터졌다.
> 랜덤 액세스 시킹, 또는 CfL 모드에서 dav1d 가 `uv_angle` 필드를 각도 외 용도로 재사용하는 것이
> 후보다. 방어 코드가 abort/OOB 는 막지만 **근본 원인은 열린 항목**이다.

### ✅ 수정 — 디코더를 FFMpeg 으로 바꾸면 크래시 (patch 0007)

`./YUView test.ivf` 로 열고 속성 패널의 Decoder 를 Dav1d → FFMpeg 로 바꾸면 죽었다.
**독립된 원인이 3개 겹쳐 있었고, 고치는 과정에서 4번째(livelock)가 드러났다.** 전부 실측으로 확정.

| # | 원인 | 증거 |
|---|---|---|
| ① | `allocateDecoder()` 가 뮤텍스 없이 `loadingDecoder`/`cachingDecoder` 를 `reset()`. 워커 스레드는 `loadRawData()`(`:642` null 체크도 없음)에서 그 포인터를 역참조 | 백트레이스: Thread3 `loadRawData` SIGSEGV / Thread1 `allocateDecoder`→`avcodec_open2` |
| ② | `decoderFFmpeg` 하나를 파괴하면 ffmpeg 라이브러리가 **process-wide 언로드**되어, 아직 열려있는 `FileSourceFFmpegFile` 의 resolve 된 함수 포인터가 dangling | `/proc/self/maps`: libavcodec 매핑 **4 → 0**, 이후 호출 시 core dump |
| ③ | `decoderEngine` 을 `allocateDecoder()` **전에** 대입 → 워커가 "새 엔진 + 낡은 디코더" 를 보고 `dynamic_cast<decoderFFmpeg*>` 가 nullptr, 그리고 `ffmpegDec->pushAVPacket()` 에 null 체크 없음 | 백트레이스: Thread3 `pushAVPacket` SIGSEGV |
| ④ | `loadRawData()` 의 `while (!rightFrame)` 에 EOF 탈출구가 없어, 입력이 고갈되면 **락을 쥔 채 무한 루프** → GUI 정지 | 라이브 스택: 워커가 `getNextUnit` 스핀, 메인은 `decoderComboxBoxChanged` 에서 락 대기 |

④는 ①을 고친 뒤에야 보였다 (그전엔 크래시가 먼저 났다). 내부 `break` 만으로는 외부 루프가
재진입하므로 `outOfData` 플래그로 외부까지 빠져나온다.

**수정 내용**
- `QRecursiveMutex decoderAccessMutex` 신설. `allocateDecoder` / `loadRawData` / `loadStatistics` /
  `decoderComboxBoxChanged` / `displaySignalComboBoxChanged` 에서 획득
- 엔진 + 디코더 + `currentFrameIdx` + `decodingNotPossibleAfter` 를 **한 락 안에서** 교체
  (프레시 디코더 + 낡은 인덱스 조합은 시크를 건너뛰어 무한 루프가 된다)
- `QLibrary::PreventUnloadHint` 로 ffmpeg 라이브러리 언로드 금지
- null 가드: `loadRawData` 진입부, `ffmpegDec` cast 결과, `loadStatistics`, `getPixelValues`,
  `loadRawData` 후반부, `displaySignalComboBoxChanged` 의 `cachingDecoder`
- EOF 탈출구 2곳 (libavformat 입력 / annexB 입력, 둘 다 非FFmpeg 디코더 분기)

**같은 버그 클래스를 하나 더 발견** — `displaySignalComboBoxChanged` 는 `cachingDecoder->` 를
무가드로 쓰고(캐싱 비활성 시 null) 뮤텍스도 없었다. 사용자가 보고한 것과 같은 속성 패널의
다른 콤보박스다.

**검증**
```
실제 QComboBox 로 프로덕션 슬롯 구동 + 워커 동시 실행 : 3/3 PASS (수정 전 HANG)
                                                        워커 15,000~19,800회 로드
극단 스트레스 (엔진 40회 반복 전환)                   : 2/2 PASS (수정 전 crash→hang)
라이브러리 언로드                                     : 매핑 4 유지, file source 정상
AV1 회귀 5종 + 엔진 기본값                            : 전부 PASS, 수치 동일
GUI 실행                                              : 생존 + 정상 repaint (Buffer 96%→100%)
```

> ⚠️ **미검증**: xdotool 로 GUI 콤보박스를 실제로 조작하는 데 실패했다(창 검색/팝업 클릭 불안정).
> 전환 자체는 실제 `QComboBox::setCurrentIndex` 로 프로덕션 슬롯을 인프로세스 구동해 검증했다.
> GUI 스크린샷은 안정성·응답성만 증명한다.

### ✅ 수정 — frame rate 기본값 30fps (patch 0006)

IVF 는 `avg_frame_rate = 0/0` 이라 `FileSourceFFmpegFile` 이 `frameRate = -1` 을 돌려주고,
annexB 파서도 VUI timing 이 없으면 -1 을 준다. 그러면 `PlaybackController::getCurrentItemsFrameRate`
가 **0.01fps 로 클램프**해 정지한 것처럼 보였다.
`DEFAULT_FRAMERATE` 를 `24.0` → `30.0` 으로 바꾸고, 두 입력 분기가 합류하는 지점에서
`prop.frameRate > 0` 이 아니면 이 값을 쓴다. GUI 에서 `Framerate 30.00` 확인.

> **부작용 (의도적)**: `DEFAULT_FRAMERATE` 는 전역이라 `playlistItem::Properties::frameRate`
> 초기값(`playlistItem.h:96`), raw/이미지 시퀀스 기본값, HEVC/VVC annexB 파서 폴백
> (`ParserAnnexBHEVC.cpp:103`, `ParserAnnexBVVC.cpp:98`)도 24 → 30 이 된다.
> raw YUV 만 24 로 유지하려면 상수를 분리해야 한다 — **사용자 판단 대기**.

### codex 리뷰 (2026-07-31)

`codex exec` 로 전체 diff 리뷰. **`VERDICT: FIX`** — 지적 2건 모두 타당해 반영했다:
1. `getPixelValues()` 가 `loadingDecoder` 를 무가드 역참조 (페인팅 경로에서 호출됨) → 가드 추가
2. annexB + 非FFmpeg `getNextNALUnit` 분기의 EOF spin — 내가 의도적으로 남겼던 것을
   "truncated/malformed HEVC/VVC 에서 실제 위험" 으로 확인 → 같은 탈출구 추가

codex 가 확인해 준 것: 남은 비동기 디코더 접근 없음, 뮤텍스 순서 역전 없음
(`cachingMutex` → `decoderAccessMutex` 단방향), frame rate 폴백 위치가 두 입력 경로 모두 커버.

### 부수 발견
- 프레임 인덱싱 자체는 정상: `startEndRange (0, 25)` = 26프레임, `av1`, 176x144
- Qt TLS 경고(`qt.tlsbackend.ossl: Incompatible version of OpenSSL`)는 업데이트 확인용이며 무해

## 남은 일

### 상설화
- [ ] dav1d fork 를 서브모듈로 추가 (`third_party/dav1d/upstream`) — BSD 2-clause, 벤더링 가능
- [ ] `scripts/setup-dav1d.sh` 작성 (meson venv + nasm 재사용 + `decoder/` 배치).
      `setup-ffmpeg.sh` 와 동일 패턴
- [ ] `scripts/setup-toolchain.sh` 에 dav1d 점검 항목 추가
- [ ] clean 빌드 후 복구 절차 문서화 (`build/` 는 gitignore 대상)

### 검증
- [x] **GUI 기동 확인** — 파일 로드/속성 패널/통계 타입 등록까지 정상 (2026-07-31)
- [x] **AV1 디코딩 뷰 (dav1d)** — OBU 공급 경로 수정 후 GUI 렌더링 확인 (2026-07-31)
- [x] `decoderFFmpeg` AV1 실패 원인 규명 + 수정 (FFmpeg 빌드에 libdav1d 누락)
- [x] AV1 기본 디코더 = Dav1d + FFmpeg 폴백 (patch 0004), 두 라이브러리 상태 실측
- [ ] Bitstream Analysis 탭 OBU 트리 렌더링 확인 (아직 미확인 — 탭 클릭 자동화 실패)
- [~] 블록 오버레이 렌더링 — 통계 체크박스를 켜니 프레임 위에 블록 사각형이 그려지는 것을 확인.
      다만 값 기반 통계(Pred Mode 등)로 색칠되는 화면까지 본 것은 아니라 **부분 확인**
- [ ] 넓은 AV1 스트림 셋으로 dav1d 0.2.2 디코딩 정확성 회귀. **현재 실측은 스트림 2개뿐**.
      VQAnalyzer 교차검증 (`~/.claude/CLAUDE.md` 참조)
- [ ] 선언 27개 중 데이터가 실린 20개 — 나머지 7개가 스트림 미사용 도구 때문인지 확인 (현재 추정)

### 방어 코드
- [x] `calculateIntraPredDirection` 범위 방어 (patch 0002) — 위 절
- [ ] `angleDelta` 가 범위를 벗어나는 근본 원인 규명 (patch 0002 는 증상만 막는다)
- [x] 디코더 전환 크래시 (patch 0007) — 원인 4개, codex 리뷰 반영
- [x] frame rate 폴백 30fps (patch 0006)
- [ ] `DEFAULT_FRAMERATE` 전역 변경의 부작용 승인 여부 (raw YUV 기본값 24→30) — **사용자 판단 대기**
- [ ] GUI 에서 콤보박스를 실제 클릭해 디코더 전환 확인 (xdotool 자동화 실패, 인프로세스로만 검증)
- [ ] `blk_data` 널 체크 추가. 통계를 켠 뒤 `resetDecoder()` 를 빠뜨리면
      `parseBlockRecursive()` 가 NULL 을 역참조해 **SIGSEGV** (실측). upstream 견고성 버그
- [ ] avutil 59 에서 `AVFrame_57_58_59` 레이아웃이 어긋나는 문제 — 현재는 죽은 코드만
      영향받아 무해하지만, `AVFrameWrapper` 게터를 추가하면 조용히 깨진다. 주석/가드 필요

### 별도 판단 필요
- [ ] 최신 dav1d 를 FFmpeg 에 static 내장할지 (디코딩 속도용, 통계와 무관).
      0.2.2 는 FFmpeg 최소 요건 미달이라 **dav1d 빌드 2개**가 된다
- [ ] dav1d 를 YUView 에 static 링크할지 (~20줄 패치로 추정, **미검증**)
- [ ] raw `.obu` 직접 입력 지원 — `InputFormat` enum 확장 필요
- [ ] **AV1 기본 디코더를 Dav1d 로 바꿀 것인가.** 지금은 `playlistItemCompressedVideo.cpp:335-341`
      의 폴백이 "libde265 > FFMpeg > 나머지" 순서라 AV1 은 FFMpeg 을 고른다 → 깨진 경로.
      Dav1d 를 우선하면 즉시 동작하고 블록 통계까지 얻지만, `libdav1d-internals.so` 가 없는
      환경에서는 폴백이 필요하다. **제품 결정이라 미적용.**
      임시로는 설정으로 우회 가능: `[Decoders] DefaultDecoderAV1=Dav1d`

## 전제 (환경)

```bash
./scripts/setup-ffmpeg.sh     # FFmpeg 7.1.2. AV1 분석에 필수
# dav1d: 아직 스크립트 없음. 현재는 수동 배치 상태
#   build/YUViewApp/ffmpeg/    <- libav*.so.NN
#   build/YUViewApp/decoder/   <- libdav1d-internals.so
```

## 주의

- **FFmpeg 8.x 로 올리지 말 것** — `av_init_packet` 제거로 필수 심볼 바인딩이 깨진다. 상한은 7.1
- **최신 libdav1d 1.x 는 통계에 못 쓴다** — `Dav1dPicture` 레이아웃이 다르다
