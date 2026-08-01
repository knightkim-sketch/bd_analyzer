---
title: TASK-0002 A~D 기능 실동작 검증
status: todo
created: 2026-07-28
updated: 2026-07-28
author: claude-opus-5
verified: no
---

## 목표

[TASK-0001](TASK-0001-bootstrap-build.md)에서 빌드와 기동은 확인됐다.
이제 **10-research 의 기능 갭 분석이 실제 바이너리에서 맞는지** 확인한다.
GUI 상호작용이 필요하므로 사람이 X 세션에서 수행해야 한다.

## 전제

```bash
./scripts/setup-ffmpeg.sh    # AV1 분석에 필수. 미설치 시 아래 A-3 은 반드시 실패한다
```

> 이전 판에는 `sudo dnf install -y ffmpeg-libs` 로 적혀 있었으나 **틀렸다.**
> 이 머신에는 sudo 가 없고, rpmfusion 판(4.4.8)은 의존 패키지를 48개 끌어온다.
> → [TASK-0003](TASK-0003-av1-parsing-dav1d.md), [ffmpeg-integration.md](../10-research/ffmpeg-integration.md)

## 테스트 스트림

`ffmpeg`(static, `/usr/local/bin/ffmpeg`)로 생성. 176x144, 25fps, 1초:
```bash
ffmpeg -f lavfi -i testsrc2=size=176x144:rate=25:duration=1 -pix_fmt yuv420p \
       -f rawvideo test_176x144_yuv420p.yuv
ffmpeg -f lavfi -i testsrc2=... -c:v libx264   -g 10 -f h264 test.h264
ffmpeg -f lavfi -i testsrc2=... -c:v libx265   -g 10 -f hevc test.h265
ffmpeg -f lavfi -i testsrc2=... -c:v libaom-av1 -cpu-used 8 -g 10 test.ivf
ffmpeg -f lavfi -i testsrc2=... -c:v libaom-av1 -cpu-used 8 -g 10 -f obu test.obu
```

## 체크리스트

### B) YUV 분석 — 있다고 조사된 것
- [ ] `test_176x144_yuv420p.yuv` 열기. 파일명에서 176x144 / yuv420p 자동 추론되는가
- [ ] 픽셀 값 검사 (고배율에서 raw 값 오버프린트)
- [ ] 같은 파일 2개로 difference 아이템 → PSNR 텍스트가 나오는가
- [ ] split view SIDE_BY_SIDE / COMPARISON

### A) 비트스트림 분석
- [ ] `test.h264` → Bitstream Analysis 탭 4개 (Stream Info / Packet Analysis / Bitrate Plot / HRD)
- [ ] SPS/PPS/slice header 신택스 트리가 5컬럼(name/value/coding/code/meaning)으로 뜨는가
- [ ] `test.h265` 동일 확인
- [ ] **A-3** `test.ivf` (AV1) → ffmpeg-libs 설치 후에만 동작해야 함. 미설치 시 실패를 재현해 가설 확인
- [ ] **A-4** `test.obu` (raw OBU) → **실패가 예상 결과.** `InputFormat` enum에 raw AV1 항목이 없다.
      실패하면 조사 결론이 맞은 것 → 우리가 추가해야 할 첫 기능으로 확정

### 블록 오버레이 (최대 리스크)
- [ ] `test.h264` 에서 통계 오버레이 → **MV 4종만** 보이는가 (파티션/intra-mode 없음이 예상)
- [ ] `test.ivf` 에서 → upstream dav1d 로는 통계가 안 나오는 것이 예상.
      나온다면 조사 결론을 수정해야 한다
- [ ] libde265 upstream 설치 후 `test.h265` → internals 없이 통계가 안 나오는지 확인

## 산출물

각 항목의 실제 결과를 이 문서에 기록하고, 조사 문서
([yuview-feature-gap.md](../10-research/yuview-feature-gap.md))와 어긋나는 부분을 정정한다.

**특히 확인이 필요한 조사 주장** (전부 코드 정적 분석 기반, 런타임 미검증):
1. AV1은 컨테이너 경유로만 분석 가능 (`ParserAV1OBU.h:58-62` `assert(false)`)
2. H.264 블록 통계는 MV 4종뿐 (`decoderFFmpeg.cpp:430-439`)
3. 풍부한 AV1/HEVC 통계는 fork된 `-internals` 디코더 빌드를 요구

## 다음
→ TASK-0003 AppImage 패키징 및 타 머신 실행 검증
→ TASK-0004 A 기능 리스크 해소 (internals 디코더 빌드 여부 결정)
