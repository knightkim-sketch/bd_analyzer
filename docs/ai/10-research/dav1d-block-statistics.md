---
title: dav1d 0.2.2 fork 소스 포함 검토 — AV1 블록 단위 통계 (기능 A')
status: active
created: 2026-07-31
updated: 2026-07-31
author: claude-opus-5
verified: yes
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
dav1d-fork: ChristianFeldmann/dav1d @ 0a7f21078baa2acd308943288a93b89adec8e27a (master, 2019-03-22)
---

## 결론

**소스 레벨 포함 가능하다. 실측으로 끝까지 검증했다.**

| 질문 | 답 |
|---|---|
| 2019년 fork 가 현재 툴체인에서 빌드되는가 | ✅ gcc 8.5 + nasm 2.16.03 + meson 1.11.2 로 **깔끔히 빌드** (exit 0, deprecation 경고만) |
| 라이선스가 포함 가능한가 | ✅ **BSD 2-clause** (VideoLAN). GPLv3 프로젝트에 벤더링 문제 없음 |
| 블록 단위 통계가 실제로 나오는가 | ✅ **나온다.** 1080p 프레임에서 블록 값 141,205개 + MV 11,210개 |
| static 으로 YUView 에 직접 링크 가능한가 | ⚠️ **가능해 보인다** — FFmpeg 과 달리 패치가 작다(~20줄). 단 **미구현/미검증** |

이게 [project-brief.md](../00-context/project-brief.md) 가 "알려진 최대 리스크"로 적어 둔 기능 A' 의 핵심 관문이었다.

## 왜 이 fork 여야 하는가 (ABI 가 2019년에 고정)

YUView 는 dav1d 헤더를 `src/decoder/externalHeader/dav1d/` 에 **벤더링해 두고 컴파일 타임에 구조체로 쓴다.**
실측 diff 결과 `dav1d.h` / `picture.h` / `headers.h` / `data.h` / `common.h` 5개가
fork master 와 **바이트 동일**(trailing whitespace 제외)하다.

- fork 의 `Dav1dPicture` 는 구조체 끝에 analyzer 확장 필드가 붙어 있다 —
  `pred[3]`, `pre_lpf[3]`, `blk_data`, `invisible` (`externalHeader/dav1d/picture.h:88-101`)
- `blk_data` 를 `Av1Block[]` 로 읽는다 (`externalHeader/dav1d/blockData.h:195`)
- **`blockData.h` 는 fork 에도 없다** — dav1d 내부 `src/levels.h:273` 의 `Av1Block` 을 YUView 가 손으로 베낀 미러다.
  실측 대조 결과 두 정의는 **필드 순서·타입이 완전히 일치**한다 (`mv` vs `motionVector` 는 같은 구조의 typedef 이름 차이)

→ 최신 libdav1d 1.x 는 `Dav1dPicture` 레이아웃이 달라 **쓸 수 없다.** 통계는 물론 디코딩도 위험하다.
→ 그리고 0.2.2 는 FFmpeg 7.1 의 최소 요건(`dav1d >= 0.5.0`, ffmpeg configure:6904)에 못 미쳐
   **FFmpeg 내장용과 이 통계용은 하나의 dav1d 로 겸할 수 없다.** → [ffmpeg-integration.md](ffmpeg-integration.md)

## 빌드 (실측 2026-07-31)

```bash
git clone --branch master https://github.com/ChristianFeldmann/dav1d.git
# meson_version: '>= 0.47.0' (2018년 문법) 이지만 meson 1.11.2 가 그대로 처리한다.
# 경고만 나온다: "extract_all_objects called without setting recursive keyword argument"
PATH=~/opt/tools/bin:$PATH meson setup build-shared --default-library=shared --buildtype=release
PATH=~/opt/tools/bin:$PATH meson compile -C build-shared
```

- 컴파일러: **시스템 gcc 8.5 로 충분** (순수 C. gcc-toolset 불필요)
- nasm: `~/opt/tools/bin/nasm` 2.16.03 재사용 (fork 요구사항은 >= 2.13.02).
  `src/x86/itx.asm:1262` 에서 legacy 매크로 경고 1건 — 무해
- 산출물: `build-shared/src/libdav1d.so.1.0.1` (soname `libdav1d.so.1`), 1.4 MB
- meson 은 python3.11 venv 에 설치 (`~/.venv-meson`). 시스템 python3.6 으로는 불가

### 배포 위치

YUView 는 `libdav1d-internals` 를 **먼저** 찾고 없으면 `libdav1d` 를 찾는다 (`decoderDav1d.cpp:581-583`).
탐색 경로에 `applicationDirPath()/decoder/` 가 있다 (`decoderBase.cpp:138`).

```
build/YUViewApp/decoder/libdav1d-internals.so     <- 여기에 두면 잡힌다
```

## 실측 검증 — 프로덕션 코드 경로

`decoderDav1d` + `FileSourceFFmpegFile` 을 헤드리스로 직접 구동해 확인했다 (`QT_QPA_PLATFORM=offscreen`).

```
필수 심볼 11개        : 전부 resolve 성공 (analyzer 2개 포함)
로드된 라이브러리     : Dav1d deoder Version 0.2.1.0.analyze-0-g0a7f210   <- "analyze" = fork 확인
statisticsSupported   : TRUE        <- internalsSupported. 블록 통계 관문
nrSignalsSupported    : 3           <- reconstruction + prediction + pre-loopfilter
declared stat types   : 27

test.ivf   (176x144)  : 프레임 176x144 / 38,016 B, 데이터 있는 타입 20개,
                        블록 값 4,915 / MV 186
sample.av1 (1920x1080): 프레임 1920x1080 / 3,110,400 B, 데이터 있는 타입 20개,
                        블록 값 141,205 / MV 11,210
```

프레임 크기가 `width*height*1.5` 와 정확히 일치 → YUV420 8bit 정상 디코딩.
통계 타입 예: `Pred Mode`, `Segment ID`, `skip`, `skip_mode`, `intra pred mode (Y/UV)`, `palette size (Y/U)` …

> 선언된 타입은 27개인데 데이터가 실린 것은 20개다. 나머지 7개는 이 스트림이 쓰지 않는 도구
> (palette / CfL / compound / interintra 등)로 보인다 — **미검증 추정.**

## ⚠️ 함정 — 통계를 켜면 반드시 디코더를 리셋해야 한다

`decoderDav1d::allocateNewDecoder()` 는 `statisticsEnabled()` 가 **이미 참일 때만**
`analyzerSettings.export_blkdata = 1` 을 세팅한다 (`decoderDav1d.cpp:276-280`).

생성자가 디코더를 먼저 할당하므로, 순서를 지키지 않으면 `Dav1dPicture.blk_data` 가 NULL 인 채로
`parseBlockRecursive()` 가 이를 역참조해 **SIGSEGV** 로 죽는다 (실측: 하네스 첫 시도가 여기서 코어 덤프).

```cpp
decoderDav1d dec(0, false);
dec.enableStatisticsRetrieval(&statsData);
dec.resetDecoder();               // <- 필수. 빠뜨리면 segfault
```

`decoderBase.h:125-126` 주석이 이 순서를 명시하고 있다("activate it, reset the decoder and decode
to the current frame again"). 다만 **YUView 에 널 체크가 없는 것은 upstream 견고성 버그**다 —
우리 코드에서 이 경로를 쓰게 되면 방어 코드를 넣을 것.

## 포함 방식 두 가지

### (a) shared `.so` — 오늘 검증된 방식
서브모듈 + `scripts/setup-dav1d.sh` 로 빌드해 `build/YUViewApp/decoder/libdav1d-internals.so` 배치.
upstream 을 **한 줄도 건드리지 않는다.** AppImage 에는 `.so` 를 같이 번들.

### (b) static `.a` 를 YUView 에 직접 링크 — 가능해 보이나 미검증
FFmpeg 과 달리 **패치가 작다.** 이유:
- YUView 트리에 이미 실제 dav1d 헤더가 있고 `DAV1D_API` 선언 10개가 그대로 들어 있다
  → 심볼을 직접 호출할 타입 정보가 이미 존재
- 지원해야 할 버전이 **하나뿐**이다 (FFmpeg 은 4.x~7.x 6조합 + 버전별 구조체 사본 35개)
- dlopen 결합부가 얇다: `decoderBaseSingleLib` 의 `QLibrary` 하나 +
  `decoderDav1d::resolveLibraryFunctionPointers()` 의 `resolve()` 호출 11개 (`decoderDav1d.cpp:187-213`)

예상 작업: `resolve(ptr, "name")` 11개를 `this->lib.x = &x;` 직접 대입으로 바꾸고
`loadDecoderLibrary()` 를 우회 + `libdav1d.a` 링크(PIC 빌드 필요). **~20줄 규모로 추정.**

부작용: `getLibraryPaths()` 가 빈 값이 되고(`decoderBase.h:186`), 설정 다이얼로그의
사용자 지정 라이브러리 검증(`decoderDav1d::checkLibraryFile`)이 무의미해진다.

**아직 구현·검증하지 않았다.** 추정치로만 취급할 것.

## 감수해야 하는 리스크

- **디코더가 7년 됐다** (2019-03, dav1d 0.2.x 세대). 보안 업데이트 없음.
  최신 AV1 스트림/도구 조합에서 디코딩 실패 가능성이 실재한다.
  실측 범위는 **테스트 스트림 2개뿐** — 넓은 검증이 필요하다
- 통계용(0.2.2)과 FFmpeg 내장용(>=0.5.0)이 갈리므로 **dav1d 빌드를 2개 유지**하게 될 수 있다
- fork 는 사실상 유지보수가 끝난 저장소다. 상위 dav1d 의 개선을 못 받는다
- 대안으로 최신 dav1d 에 analyzer 패치를 재이식하는 길이 있으나, `Dav1dPicture` 확장 +
  내부 `Av1Block` 노출을 다시 만들어야 해서 작업량이 크다 — **별도 조사 필요**

## 다음 단계 (미착수)

- 서브모듈 + `scripts/setup-dav1d.sh` 로 (a) 방식 상설화
- GUI 에서 블록 오버레이 렌더링 확인 (`stats::paintStatisticsData`) → TASK-0002
- 넓은 AV1 스트림 셋으로 0.2.2 디코딩 정확성 회귀 (VQAnalyzer 교차검증)
