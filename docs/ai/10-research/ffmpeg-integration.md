---
title: FFmpeg 연동 — AV1 분석 요건, 버전 상한, static 링크 검토
status: active
created: 2026-07-30
updated: 2026-07-31
author: claude-opus-5
verified: yes
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 요약

| 질문 | 답 |
|---|---|
| AV1 분석에 FFmpeg 이 필요한가 | **필수.** 없으면 AV1 분석 경로가 아예 열리지 않는다 |
| static lib(.a)로 static build 가능한가 | **불가.** YUView 는 FFmpeg 를 `dlopen` 으로만 쓴다. `.a` 는 dlopen 대상이 될 수 없다 |
| 그럼 무엇을 했는가 | FFmpeg 7.1.2 를 **shared 로 소스 빌드** + dav1d 는 static 내장 — 외부 의존이 OS 기본뿐인 자기완결 `.so` 4개 |
| 버전 상한 | **FFmpeg 7.1.** 8.x 는 `av_init_packet` 제거로 필수 심볼 바인딩이 깨진다 |
| 검증 | 실제 AV1 스트림 2개를 YUView 프로덕션 코드 경로로 파싱 성공 (아래 "실측") |

설치 스크립트: [`scripts/setup-ffmpeg.sh`](../../../scripts/setup-ffmpeg.sh)

---

## 1. AV1 분석은 FFmpeg 없이는 전면 불가

`ParserAV1OBU` 에 도달하는 경로가 **하나뿐**이고 그 경로가 libavformat 을 통과한다.

- `filesource/FileSource.h:48-55` — `InputFormat` enum 이 `AnnexBHEVC / AnnexBAVC / AnnexBVVC / Libav` 4개. **raw AV1 OBU 분기가 없다.**
- `parser/AVFormat/ParserAVFormat.cpp:597` — `else if (this->codecID.isAV1()) this->obuParser.reset(new ParserAV1OBU())`.
  즉 OBU 파서는 **컨테이너를 libavformat 으로 demux 한 뒤 그 payload 에만** 붙는다.
- `parser/AV1/ParserAV1OBU.h:58-62` — `runParsingOfFile()` 이 `assert(false)`. 파일을 직접 열 수 없다.

→ FFmpeg 이 없으면 raw YUV / H.264·HEVC AnnexB 분석은 되지만 **AV1 은 어떤 형태로도 열리지 않는다.**
(`.obu` 직접 입력 지원은 별개 과제 — [yuview-feature-gap.md](yuview-feature-gap.md) A''' 절)

## 2. static 링크 검토 — 원리적으로 불가

**YUView 는 FFmpeg 를 빌드 타임에 링크하지 않는다.** 컴파일 단위 어디에도 FFmpeg 헤더나 심볼 참조가 없다.

- `ffmpeg/FFmpegLibraryFunctions.cpp:185,270` — `QLibrary::setFileName()` + `load()`
- `ffmpeg/FFmpegLibraryFunctions.cpp:48` — `lib.resolve(symbolName)` 로 **심볼을 이름 문자열로** 가져와 함수 포인터에 담는다
- 필수 심볼 36개 (`FFmpegLibraryFunctions.cpp:60-175` 의 `bindLibraryFunctions` 4개 오버로드)

정적 라이브러리는 링커 시점의 아카이브일 뿐 `dlopen` 대상이 아니다. 따라서 `.a` 를 가져와도 **쓸 방법이 없다.**

### static 으로 바꾸려면 드는 비용

가능하기는 하지만 upstream 침습도가 크다:

1. `FFmpegLibraryFunctions.{h,cpp}` (496줄) 와 `FFmpegVersionHandler.{h,cpp}` (809줄) 의 바인딩 계층을 전부 제거하고 실제 심볼 직접 호출로 교체
2. **핵심 문제** — YUView 는 FFmpeg 구조체 레이아웃을 **메이저 버전별로 손으로 베껴 두고** 불투명 포인터를 `reinterpret_cast` 한다.
   `AVFrameWrapper.cpp:43,64,84` 의 `AVFrame_54 / AVFrame_55_56 / AVFrame_57_58_59` 가 대표 예이고,
   wrapper 전체에 이런 **버전별 구조체 사본이 35개** 있다 (`AVCodecContext_56/57/58/59/61`, `AVStream_56/57/58/59/60`, `AVFormatContext_56/57/58/59/61`, …).
   static 링크로 가면 이걸 전부 실제 헤더 기준으로 재작성해야 한다.
3. 런타임 다중 버전 관용성(FFmpeg 4.x~7.x 중 아무거나)을 잃고 빌드 시점 한 버전에 고정된다
4. 라이선스는 문제 없다 — YUView 가 GPLv3 이고 FFmpeg 은 LGPL2.1+ 이므로 정적 링크도 GPLv3 하에서 합법
   (단 `--enable-gpl` 없이 빌드해야 LGPL 로 남는다. 우리 빌드는 GPL 컴포넌트를 켜지 않았다 → configure 가 `License: LGPL version 2.1 or later` 로 확인)

**결론: 하지 않는다.** 얻는 것(단일 파일 배포)이 잃는 것(upstream diff 1300줄 + 리베이스 비용 + 버전 관용성)보다 작다.

### 대신 채택한 것 — "shared 껍데기 + 의존성 내장"

배포 편의라는 실제 목표는 static 링크 없이 달성된다. FFmpeg 을 **최소 구성 shared** 로 빌드하면
외부 의존이 OS 기본 라이브러리만 남는다. 실측:

```
libavutil.so.59      libm libpthread libc
libswresample.so.5   libm libpthread libc
libavcodec.so.61     libm liblzma libdl libz libpthread libc
libavformat.so.61    libm libz libpthread libc
```

즉 **자기들끼리 + glibc + libz/liblzma** 뿐이다. 외부 코덱 `.so` 의존이 0개다 —
dav1d 는 **static 으로 libavcodec.so 안에 흡수**했다 (`-Db_staticpic=true` + `--enable-libdav1d`).
4개 합쳐 **16.8 MiB** (dav1d 내장분 +1.9 MiB). AppImage 에 그대로 던져 넣으면 끝난다.

> ⛔ **정정 (2026-07-31).** 이 문서의 초판은 "FFmpeg 7.1 의 native AV1 디코더를 쓰므로
> libdav1d 조차 필요 없다"고 적었다. **틀렸다.** `CONFIG_AV1_DECODER=yes` 로 빌드되는
> native `av1` 디코더는 **hwaccel 전용**이고 소프트웨어 디코딩을 하지 못한다:
> `libavcodec/av1dec.c:659-668` 이 `!avctx->hwaccel` 이면
> `"Your platform doesn't support hardware accelerated AV1 decoding."` 를 찍고
> **`AVERROR(ENOSYS)` = -38** 을 돌려준다 (소스 주석: *"Since now the av1 decoder doesn't
> support native decode"*). 그 결과 첫 빌드에서는 YUView 의 FFmpeg 디코더로 AV1 을 열면
> `avcodec_send_packet` 이 즉시 실패해 **빈 화면**이 됐다.
> → **`--enable-libdav1d` 가 필수다.** `CONFIG_AV1_DECODER=yes` 만 보고 판단하면 안 된다.
> FFmpeg 7.1 의 최소 요건은 dav1d >= 0.5.0 이라, YUView 블록 통계용 fork(0.2.2)로는
> 겸할 수 없다 → **dav1d 빌드가 2개 필요하다.**
> → [dav1d-block-statistics.md](dav1d-block-statistics.md), [TASK-0003](../40-tasks/TASK-0003-av1-parsing-dav1d.md)

> 참고: upstream 도 같은 판단을 한다. `de.rwth_aachen.ient.YUView.yaml` 의 flatpak 매니페스트가
> `--enable-shared --disable-static` 으로 FFmpeg **7.1.2** 를 빌드한다. 우리가 쓴 tarball 이
> 그 매니페스트의 sha256 과 동일하다 (`089bc60f…c304`).

### 왜 rpmfusion `ffmpeg-libs` 를 쓰지 않았는가

`ffmpeg-libs-4.4.8` 은 지원 조합 `(56,58,58,3)` 에 들어가므로 **동작은 한다.** 그런데:

- **sudo 가 없다** (실측: `sudo -n` 실패). 이것만으로 이미 막힌다
- 의존 패키지 **48개** — `libmfx`, `ocl-icd`, `vapoursynth-libs`, `librsvg2`, `libmysofa` … 를 시스템에 깔고
  AppImage 에도 전부 끌고 가야 한다. 위의 6개와 비교가 안 된다
- FFmpeg 4.4 는 native AV1 디코더가 없어 AV1 **디코딩**이 EPEL 의 `libdav1d 0.5.2`(2019년) 에 의존한다

## 3. 버전 상한은 FFmpeg 7.1 — 8.x 는 깨진다

`ffmpeg/FFmpegVersionHandler.cpp:104-110` 이 지원 조합을 (avutil, avcodec, avformat, swresample) 메이저로 열거한다:

```
(59,61,61,5)  = FFmpeg 7.x   <- 최신
(58,60,60,4)  = FFmpeg 6.x
(57,59,59,4)  = FFmpeg 5.x
(56,58,58,3)  = FFmpeg 4.x
(55,57,57,2) (54,56,56,1)
```

FFmpeg 8.x(avutil 60 / avcodec 62)는 목록에 없고, 추가해도 **동작하지 않는다**:
필수 심볼 `av_init_packet` 이 `FF_API_INIT_PACKET (LIBAVCODEC_VERSION_MAJOR < 62)` 로 묶여 있어
avcodec 62 에서 제거된다 (실측: n6.1.1/n7.0.2/n7.1 헤더 대조). 바인딩이 실패하면 라이브러리 전체가 거부된다.

> `av_register_all`(avformat<59), `av_frame_get_metadata`(avutil<57), `avcodec_decode_video2`(신 API 없을 때)
> 3개는 조건부 필수라서 7.1 에는 없어도 무해하다. 실측으로 확인했다.

## 4. ⚠️ 알려진 잠재 문제 — avutil 59 에서 AVFrame 레이아웃이 어긋난다

`AVFrameWrapper.cpp:84` 의 `AVFrame_57_58_59` 는 **avutil 59 와 맞지 않는다.**
FFmpeg 7.0(avutil 59)이 제거한 필드 4개가 이 사본에는 아직 남아 있다:

| 사본에 있으나 실제 avutil 59 에 없는 필드 | 제거 시점 |
|---|---|
| `coded_picture_number`, `display_picture_number` | `FF_API_FRAME_PICTURE_NUMBER (< 59)` |
| `reordered_opaque` | `FF_API_REORDERED_OPAQUE (< 59)` |
| `channel_layout` | `FF_API_OLD_CHANNEL_LAYOUT (< 59)` |
| `pkt_duration` | `FF_API_PKT_DURATION (< 59)` |

→ `time_base` **다음 필드부터 전부 오프셋이 밀린다.**

**그러나 현재는 무해하다 (실측 확인):** `AVFrameWrapper` 가 외부에 노출하는 게터는
`getFrame / getWidth / getHeight / getKeyFrame / getPictType / getPTS / getSize` 뿐이고
**전부 `time_base` 앞쪽 필드**를 읽는다. 밀린 영역을 읽는 유일한 소비자는
`FFmpegVersionHandler.cpp:489` `getMetadata(AVFrameWrapper&)` 인데 **호출자가 없는 죽은 코드**다.
(`ParserAVFormat.cpp:626` 의 `getMetadata()` 는 `AVFormatContext` 쪽이라 무관.)
읽기가 실제 구조체 할당 범위 안이라 OOB 도 아니다.

**리베이스/기능 확장 시 재확인할 것.** `AVFrameWrapper` 에 게터가 추가되거나 위 죽은 코드가 살아나면
FFmpeg 7.x 에서 조용히 잘못된 값을 읽는다. 그때는 (a) `AVFrame_59` 사본을 새로 만들거나
(b) FFmpeg **6.1(avutil 58)** 로 내리면 된다 — 6.1 은 위 4개 필드가 모두 살아 있어 사본과 정확히 일치한다.
upstream flatpak 이 7.1.2 를 쓰므로 이건 upstream 에도 있는 잠재 버그다.

## 5. 설치 결과 (실측 2026-07-31)

```
빌드   : 시스템 gcc 8.5 (FFmpeg 은 순수 C -> gcc-toolset 불필요, libstdc++ 누출 문제 원천 회피)
nasm   : 2.16.03 소스 빌드 -> ~/opt/tools  (Rocky 8 활성 repo 에 없고 sudo 도 없음.
         --disable-x86asm 우회는 디코딩 성능 손실이 커서 택하지 않았다)
FFmpeg : 7.1.2 -> ~/opt/ffmpeg-7.1
dav1d  : 1.4.3 static PIC -> libavcodec.so 에 내장 (외부 .so 증가 0)
설정   : --enable-shared --disable-static --disable-doc --disable-programs
         --disable-encoders --disable-muxers --enable-encoder=png
         --enable-libdav1d --pkg-config-flags=--static
         (upstream flatpak 매니페스트 기준. gnutls 만 제외 — 네트워크 스트림을 쓰지 않고
          gnutls-devel 이 sudo 를 요구한다. libdav1d 는 우리가 추가 — 위 정정 참조)
License: LGPL version 2.1 or later   (configure 출력)
버전   : avUtil 59.39.100  avFormat 61.7.100  avCodec 61.19.101  swresample 5.3.100
활성   : LIBDAV1D(=실제 AV1 SW 디코딩), IVF/MOV/MATROSKA_DEMUXER, H264/HEVC_DECODER
         (AV1_DECODER 도 yes 지만 hwaccel 전용이라 단독으로는 쓸모없다)
크기   : .so 4개 합계 16.8 MiB (dav1d 내장 포함)
```

### 배포 위치

`ffmpeg/FFmpegVersionHandler.cpp:362-370` 의 탐색 순서:

1. `Decoders/FFmpeg.{avformat,avcodec,avutil,swresample}` 설정값 (4개 전부 있을 때만)
2. `Decoders/SearchPath` 설정값
3. `QDir::currentPath()` → `currentPath()/ffmpeg/`
4. `applicationDirPath()` → **`applicationDirPath()/ffmpeg/`**  ← 우리가 쓰는 곳
5. 빈 문자열 (시스템 경로)

`build/YUViewApp/ffmpeg/` 에 심볼릭 링크를 놓는다. 설정 파일을 건드리지 않아도 잡힌다.
파일명은 `lib<name>.so.<major>` 로 **정확히** 맞아야 한다 (`FFmpegLibraryFunctions.cpp:222`) → soname 링크 필수.

⚠️ `build/` 는 gitignore 대상이다. **clean 빌드 후 `scripts/setup-ffmpeg.sh` 재실행 필요**
(FFmpeg 재빌드는 건너뛰고 링크만 복구한다). 패키징 시에는 심볼릭이 아니라 실제 파일을 번들할 것.

## 6. 실측 — 프로덕션 코드 경로로 검증

GUI 없이 `FFmpegVersionHandler::loadFFmpegLibraries()` → `ParserAVFormat::runParsingOfFile()` 를
직접 구동하는 하네스로 확인했다 (`QT_QPA_PLATFORM=offscreen`).

```
필수 심볼 36개    : 전부 resolve 성공 (조건부 3개 제외, 위 3절)
로드 순서         : avutil.59 -> swresample.5 -> avcodec.61 -> avformat.61  전부 성공
Binding functions : successfull

test.ivf (176x144, AV1)       : runParsingOfFile=true, 1 stream, "Video av1 (176, 144)",
                                top-level 26개
sample.av1 (1920x1080, AV1)   : runParsingOfFile=true, 1 stream, "Video av1 (1920, 1080)",
                                top-level 101개
```

OBU 분해가 실제로 돌았다는 증거 (packet 이름은 `ParserAVFormat.cpp:398,462` 가 OBU 타입을 집계해 만든다):

```
packet[1] "Stream 0 - AVPacket 0 - Keyframe - OBUs: Frame Sequence Header Temporal Delimiter"
packet[2] "Stream 0 - AVPacket 1 - OBUs: Frame(x4) Temporal Delimiter"
packet[4] "Stream 0 - AVPacket 3 - OBUs: Frame Temporal Delimiter"
```

> 하네스 주의: `PacketItemModel::rowCount()` 는 캐시된 카운터(`nrShowChildItems`)를 반환하므로
> 파싱 후 `Parser::updateNumberModelItems()` 를 호출해야 0 이 아닌 값이 나온다.
> GUI 는 `modelDataUpdated()` 시그널로 이걸 돌린다.

## 남은 것 (이 문서 범위 밖)

- **GUI 실동작 확인** — Bitstream Analysis 탭에서 OBU 트리 렌더링. → TASK-0002
- **AV1 블록 단위 통계** — FFmpeg 경로로는 안 나온다. YUView 가 fork 한 dav1d
  (`dav1d_set_analyzer_flags` 확장, `decoderDav1d.cpp:210-212` 에서 optional resolve)가 필요하다.
  현 설치는 FFmpeg native AV1 디코더라 이 경로와 무관. → [yuview-feature-gap.md](yuview-feature-gap.md) A' 절
- **`.obu` raw 입력** — `InputFormat` enum 확장 필요 (1절)
