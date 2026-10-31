---
title: UI 개편 자체 승인 이력 (Phase 0–3)
status: active
created: 2026-10-31
updated: 2026-10-31
author: claude-opus-5
---

# UI 개편 자체 승인 이력

Knight 지시 (2026-10-31): "phase 3 까지 쭉 진행하고, 승인이 필요한 시점에는 제안·권장을 선택하고, 자체 승인 이력을
별도의 테이블이나 페이지로 남겨줘. 중요한 결정이라고 판단되면 전문가 agent 들의 의견을 수렴해서 결정해줘."

- 이 표는 Knight 에게 묻는 대신 Claude 가 스스로 고른 결정만 적는다. Knight 가 직접 정한 것(hex dump 자리 유지 등)은
  `ui-redesign-review.md` 에 있다.
- **중요도**: 상 = 되돌리기 비싸거나 새 의존성·구조 변경, 중 = 사용자에게 보이는 동작 변경, 하 = 구현 세부.
  상은 전문가 agent 의견을 받아 결정한다.
- 되돌리려면 "되돌리는 법" 열을 본다. 패치 번호는 `third_party/yuview/patches/`.

| # | Phase | 결정 | 고른 안 (권장) | 다른 안 | 근거 | 중요도 | 의견 수렴 | 되돌리는 법 |
|---|---|---|---|---|---|---|---|---|
| D-01 | 추가 | step 버튼 아이콘 형식 | 기존과 같은 24/48 px 회색조 PNG + `convertIcon` (테마 색 적용), 패치에 git binary hunk 로 포함 | `QStyle` 표준 아이콘 / 코드로 그리기 | 옆의 play·stop 과 모양·색이 같아야 한다. `build.sh` 는 `git apply` 라 binary hunk 를 적용한다 | 하 | — | 0085 의 PNG 4개 교체 |
| D-02 | 추가 | step 버튼 위치·동작 | stop 바로 오른쪽에 back, forward 순. 기존 `previousFrame`/`nextFrame` 슬롯에 연결 (재생 중이면 멈추고 한 프레임 이동, 양 끝에서는 그대로). 단축키는 기존 Left/Right 그대로, tooltip 에 표기 | 새 단축키 추가 | 메뉴의 Left/Right action 과 같은 슬롯이라 동작이 하나로 유지된다 | 하 | — | 0085 제거 |
| D-03 | 추가 | step 버튼 회귀 | 새 테스트 대신 63-mainwindow-layout 에 추가하고 y4m 을 인자로 넘김 | 새 테스트 64 | 같은 MainWindow 를 띄우는 비용을 한 번만 낸다. 파일 없이 돌리면 그 부분만 SKIP | 하 | — | 63 의 step 절 삭제 |
| D-04 | 전체 | 커밋 단위 | Phase 마다 로컬 커밋 (전체 회귀 통과 후), push·release 는 하지 않음 | 끝에 한 번 / 커밋 안 함 | Phase 단위로 되돌릴 수 있어야 한다. 전역 규칙상 push 는 지시가 있을 때만 | 중 | — | `git revert <phase 커밋>` |
| D-05 | 2 | docking 방식 | **native `QDockWidget`** + perspective 를 코드로 정의하는 관리자(`src/integration`), 사용자 수정분만 schema 버전과 함께 저장. Qt ADS 는 보류 | Qt ADS 도입 (LGPL-2.1) / 혼합 | ADS 는 Mainwindow.cpp(패치 29개가 건드림) 대수술, .ui dock 정의 이전, LGPL 동적 링크용 별도 .so·번들·RPM 작업, 테스트 25·63 재작성, 저장 레이아웃 무효화를 부른다. Phase 2 가 요구하는 것(이름 붙은 레이아웃, 탭, popup 의 dock 화)은 native 로 된다. 잃는 것은 auto-hide 뿐 | **상** | Progy (B 를 C 구조로, 80%), architect (native, 75%) — 둘 다 native | perspective 관리자 뒤에서 ADS 로 교체 가능 |
| D-06 | 2 | popup 5개 dock 화 | 기존 QDialog 를 그대로 두고 `QDockWidget` 으로 감싼다 (src/integration 의 factory). Esc 의 `reject()` 가 빈 dock 을 남기지 않게 처리, 늦게 생성되는 dock 은 `restoreDockWidget` | 창 클래스를 다시 작성 | 5개 모두 `QDialog` 파생이고 MainWindow 가 한 번 만들어 재사용한다. 감싸면 코드 변경이 작다 | **상** | architect (wrap) | factory 연결 제거 → 이전 popup 동작 |
| D-07 | 3 | SelectionContext | `src/integration` 의 QObject 하나, MainWindow 가 배선. `QPointer` 로 item 보관, 같은 값이면 신호 없음, source tag. 블록 통계 도착(data-ready)은 별도 신호. upstream 위젯은 context 를 모름 | 각 위젯이 context 를 직접 구독 | 현재 `blockSelected` 연결 약 10개를 한 곳으로 모은다. upstream 헤더가 integration 을 include 하지 않게 한다. 되먹임 루프 방지 | **상** | architect | MainWindow 배선을 이전 connect 로 복원 |
| D-08 | 1 | overlay 색 체계 | 색은 **도구별 색 계열**로 나눈다. 비트스트림(디코더) 색은 그대로: Pred Mode 초록·빨강·파랑·보라, MV0 빨강, MV1 파랑, SB grid 파랑, 선택 분홍 — VQ Analyzer 와 같아 팀이 익숙하다. 우리 overlay 만 바꾼다: ME = 따뜻한 계열(SVT 흰색, open loop 주황, closed loop 연두, open loop adv 노랑), GM = 청록 계열(accepted 청록, model 옅은 청록, rejected 회색). 색 값은 `src/integration/OverlayPalette.h` 한 곳에 둔다 | 전체 팔레트 교체(Okabe-Ito 등) | 충돌 4건 중 3건(ME 초록=INTRA, ME 보라≈COMPOUND, ME 청록≈GM)은 우리 overlay 쪽 색을 바꿔 없앤다. MV1 파랑 ≈ SB grid 는 남긴다 — 화살표와 격자선으로 모양이 다르고 범례가 생긴다. 디코더 색을 바꾸면 기존 스크린샷·습관과 어긋난다 | 중 | — | `OverlayPalette.h` 값 되돌림 |
| D-09 | 1 | 범례 | 그림 오른쪽 위에 **켜진 overlay 만** 범례로 그린다 (discrete 는 값 이름별 색 칸, vector 는 색 선, gradient 는 색 막대 + 최소·최대). View > Show Overlay Legend (Ctrl+L) 로 끄고 켬, 기본 켬, QSettings 저장 | 별도 dock | 그림을 보면서 바로 읽혀야 한다. 꺼진 것까지 그리면 범례가 그림을 가린다 | 중 | — | 메뉴에서 끔 |
| D-10 | 1 | overlay toggle toolbar | 그림 위 toolbar 에 현재 item 의 overlay 묶음(Properties 의 행 단위 = uiGroup)마다 체크 버튼, 앞의 8개에 **F1–F8**. 버튼 아이콘은 그 overlay 의 대표 색 칸. Properties 체크박스와 양방향 동기 | 고정 목록 toolbar | 디코더마다 overlay 목록이 다르다(dav1d/JM/HM/ME/GM). F1–F8 은 지금 아무 단축키에도 쓰이지 않는다(grep 확인) | 중 | — | toolbar 숨김 (View 메뉴) |
| D-11 | 1 | 아이콘 | 메뉴 전체 아이콘 세트는 만들지 않고, toolbar 의 색 칸 아이콘과 step 버튼(D-01)만 추가 | 아이콘 테마 도입 | 아이콘 세트는 라이선스·일관성 비용이 크고 메뉴 가독성 이득은 작다 | 하 | — | — |
| D-12 | 1 | 테마 | 기존 3개(Default, Simple Dark/Blue, Simple Dark/Orange) 유지 + **"Dark (Fusion)"** 추가: Fusion style + 어두운 QPalette, qss 없음. 그림 위 글자는 하드코딩 "helvetica" 대신 앱 기본 글꼴 | Default 를 Fusion 다크로 바꿈 | 기본 모양을 바꾸지 않고 선택지를 더한다. helvetica 는 Linux 에 없어 대체 글꼴로 떨어진다 | 중 | — | Settings 에서 테마 선택 |
| D-13 | 1 | 문구 규칙 | 메뉴에서 창을 여는 항목은 끝에 `...` (U+2026 대신 ASCII), 영국/미국 철자 혼용은 미국식(Stabilized). **회귀 42/43/45/53 이 찾는 문구는 바꾸지 않는다** | 전 메뉴 재작성 | 테스트가 문구로 action 을 찾는다(Phase 0 에서 확인) | 하 | — | 문구 되돌림 |
