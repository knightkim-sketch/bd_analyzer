---
title: 프로젝트 개요
status: active
created: 2026-07-27
updated: 2026-07-27
author: human + claude-opus-5
verified: partial
---

## 무엇을 만드는가

**bd_analyzer** — 비디오 코덱 개발·검증용 통합 분석 도구.

| # | 기능 | 설명 |
|---|---|---|
| **A** | 비트스트림 분석기 | AV1 / H.264(AVC) / H.265(HEVC). 신택스 트리, NAL·OBU 분해, 블록 단위 예측/MV/파티션 오버레이, 비트 코스트 통계 |
| **B** | YUV 분석기 | raw YUV 뷰어, 픽셀 검사, 두 파일 diff, PSNR/SSIM, 히스토그램/웨이브폼/벡터스코프 |
| **C** | YUV 생성기 | 테스트 패턴 / 테스트 시퀀스 합성 |
| **D** | 이미지 필터 | 프레임 필터 적용 + before/after 비교 |

## 기반

[YUView](https://github.com/IENT/YUView) (RWTH Aachen, GPLv3) 하드 fork.
→ [ADR-0001](../20-decisions/ADR-0001-fork-vs-library.md)

현재 기준 upstream: `a72eb3488097313511e60ed70db4af6071cbe9fe` (2026-04-28, `v2.14-301-ga72eb348`)

## 우선순위 (초안 — 사용자 확인 필요)

1. 빌드/배포 부트스트랩 — 로컬 빌드 + AppImage
2. B 기능 보강 (PSNR 누적·플롯, SSIM, 스코프) — upstream 자산이 가장 많아 ROI 최고
3. C·D — 추가적이고 리스크 낮음
4. A 블록 레이어 — 가장 어렵고 외부 의존이 큼

## 알려진 최대 리스크

**A 기능의 블록 단위 오버레이는 파서가 아니라 디코더가 만든다.**
풍부한 통계를 내려면 YUView가 fork한 `libdav1d-internals` / `libde265-internals` 빌드가 필요하고, upstream 바이너리로는 안 나온다.
**H.264는 FFmpeg 경유 MV 4종뿐 — 파티션/intra-mode 오버레이가 아예 없다.**
→ [yuview-feature-gap.md](../10-research/yuview-feature-gap.md) A절 참조. 별도 태스크로 조사 필요.

## 관련 사내 도구

`~/.claude/CLAUDE.md`에 기록된 VQAnalyzer (`/home/knight2/Bins/VQAnalyzer/VQAnalyzer_6.6.0/`) — AV1 신택스 검사 / 디코드 확인용 상용 도구. **레퍼런스 및 교차검증 기준으로 사용 가능.**
