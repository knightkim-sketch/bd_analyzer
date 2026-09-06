# AI 문서 저장소 (`docs/ai/`)

이 폴더는 **AI 에이전트와 사람이 함께 읽고 쓰는 문서**를 체계적으로 보관한다.
소스코드에서 유추할 수 없는 정보(조사 결과, 결정 이유, 설계 의도, 진행 상황)만 남긴다.

## 폴더 규칙

| 폴더 | 용도 | 수명 | 누가 씀 |
|---|---|---|---|
| `00-context/` | 프로젝트 불변 컨텍스트 — 목표, 용어, 제약. 새 세션 시작 시 가장 먼저 읽는 문서. | 영구 | 사람 |
| `10-research/` | 외부 조사 결과 (오픈소스 분석, 스펙 요약, 벤치마크). 사실 기록. | 조사 시점 고정 | AI |
| `20-decisions/` | ADR (Architecture Decision Record). "왜 이렇게 했는가". 한번 쓰면 수정하지 않고 새 ADR로 대체. | 영구 | 사람+AI |
| `30-designs/` | 기능별 설계 문서. 구현 전에 쓰고, 구현 후 실제와 맞춰 갱신. | 코드와 동기화 | AI |
| `40-tasks/` | 작업 계획 / 체크리스트 / 진행 상황. | 완료 시 `90-archive/` | AI |
| `50-sessions/` | 세션 로그. 긴 작업의 인수인계 메모. | 30일 후 archive | AI |
| `90-archive/` | 완료·폐기된 문서. 삭제하지 않고 이동. | 영구 | AI |

## 파일명 규칙

```
10-research/  <주제>.md                      예) yuview-architecture.md
20-decisions/ ADR-NNNN-<제목>.md             예) ADR-0001-fork-vs-library.md
30-designs/   <기능>-design.md               예) yuv-generator-design.md
40-tasks/     TASK-NNNN-<제목>.md            예) TASK-0001-bootstrap-build.md
50-sessions/  YYYY-MM-DD-<주제>.md           예) 2026-07-27-yuview-survey.md
```

## 모든 문서 필수 front-matter

```yaml
---
title: 문서 제목
status: draft | active | superseded | archived
created: YYYY-MM-DD
updated: YYYY-MM-DD
author: human | claude-opus-5 | ...
verified: yes | partial | no      # 도구 출력으로 교차검증했는가
---
```

`verified: partial|no` 인 문서의 수치·주장은 **미검증**으로 취급하고 재확인 후 인용할 것.

## 작성 원칙

1. **코드가 답할 수 있는 것은 쓰지 않는다.** 디렉토리 목록, 함수 시그니처 나열 금지. "왜"와 "어디서 막히는지"를 쓴다.
2. **파일 경로에는 `file.cpp:123` 형태로 줄 번호를 붙인다.** 나중에 검증 가능해야 한다.
3. **추측과 사실을 분리한다.** 검증 안 된 것은 명시적으로 표시.
4. **upstream 버전을 고정 기록한다.** YUView 관련 문서는 반드시 커밋 해시를 남긴다.

## 현재 인덱스

### 00-context
- [project-brief.md](00-context/project-brief.md) — 무엇을 만드는가
- [constraints.md](00-context/constraints.md) — 빌드/배포/라이선스 제약
- [glossary.md](00-context/glossary.md) — 용어

### 10-research
- [yuview-architecture.md](10-research/yuview-architecture.md) — 라이브러리 재사용성 / 확장점 분석
- [yuview-build-license.md](10-research/yuview-build-license.md) — 빌드 시스템 / GPLv3 / 배포 아티팩트
- [yuview-feature-gap.md](10-research/yuview-feature-gap.md) — 기능 A~D 대비 있는 것 / 없는 것
- [ffmpeg-integration.md](10-research/ffmpeg-integration.md) — AV1 분석 요건 / 버전 상한 7.1 / static 링크 불가 근거
- [dav1d-block-statistics.md](10-research/dav1d-block-statistics.md) — dav1d 0.2.2 fork 소스 포함 검토 / AV1 블록 통계 실측 (기능 A')

### 20-decisions
- [ADR-0001-fork-vs-library.md](20-decisions/ADR-0001-fork-vs-library.md) — YUView를 라이브러리로 쓸 것인가 fork 할 것인가
- [ADR-0002-build-and-deploy.md](20-decisions/ADR-0002-build-and-deploy.md) — 빌드 툴체인과 다중 머신 배포 방식
- [ADR-0003-plugin-strategy.md](20-decisions/ADR-0003-plugin-strategy.md) — 플러그인 시스템을 어디까지 만들 것인가
- [template.md](20-decisions/template.md)

### 40-tasks
- [TASK-0001](40-tasks/TASK-0001-bootstrap-build.md) — 빌드 부트스트랩 (**완료** — 빌드 성공, 이식성 실증)
- [TASK-0002](40-tasks/TASK-0002-feature-verification.md) — A~D 기능 실동작 검증 (GUI 세션 필요)
- [TASK-0003](40-tasks/TASK-0003-av1-parsing-dav1d.md) — AV1 분석 + dav1d 블록 통계 + 크래시 수정 7건 (**진행중** — 상설화 남음)
- [TASK-0004](40-tasks/TASK-0004-decoded-yuv-cache.md) — 디코딩 YUV + 블록통계를 `.YUViewBD` 에 저장 / playlist 아이템 / 종료 시 삭제 (검토 완료, 미착수)
- [TASK-0005](40-tasks/TASK-0005-block-info-on-click.md) — 클릭 시 블록 경계 하이라이트 + 블록 모드 정보 pane (**구현 완료**, GUI 확인)
- [TASK-0006](40-tasks/TASK-0006-av1-syntax-consolidation.md) — AV1 syntax 항목 통합 + OBU 목록에서 YUV frame 이동 (**구현 완료** — GUI 직접 클릭 확인만 남음)
- [TASK-0007](40-tasks/TASK-0007-block-bitstream-hexdump.md) — 블록/프레임 비트스트림 hexdump + 슈퍼블록 통계(`sb_qindex`, `sb_bitcount`) + dav1d 소스 빌드 + 오버레이 체크박스 그룹화 (**구현 완료** — 회귀 17/17, 배포본 실행 확인)
- [TASK-0008](40-tasks/TASK-0008-installable-package.md) — 설치형 패키지 RPM + tar.gz, Rocky 8 (**구현 완료** — 정리된 환경 실행 확인)
- [TASK-0009](40-tasks/TASK-0009-motion-estimation.md) — Motion Estimation 분석 기능: SVT-AV1 정수 ME + odyssey open-loop ME 재현, 통계 오버레이 통합, ME dock, 클릭 시 블록 MV (**구현 완료** — raw + compressed stream, 회귀 32/32. 알고리즘 정리는 Confluence)
