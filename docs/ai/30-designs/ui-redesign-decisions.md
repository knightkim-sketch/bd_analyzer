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
| D-08a | 1 | D-08 개정: ME 색에서 흰색 제거 | ME = SVT 주황, open loop 금색, closed loop 연두, open loop adv 연어색 | D-08 의 흰색 유지 | 회귀 26 이 흰 바탕에 overlay 를 그려 화살표 픽셀 0개 — 밝은 영상에서 흰 벡터는 안 보인다. 실제 결함 | 중 | — | `OverlayPalette.h` |
| D-13a | 1 | 검토 반영 (cpp-reviewer) | toolbar action 은 `deleteLater`, 타입 목록은 `accessMutex` 안에서 복사, 연결은 `Qt::UniqueConnection`, F1–F8 은 MainWindow 에도 붙여 toolbar 를 숨겨도 동작, 테마의 원래 style 은 style sheet 적용 전에 기록, 범례 상자는 view 안으로 제한. 범례는 split 의 왼쪽 item 만 — 알려진 한계 | 그대로 둠 | 독립 검토의 HIGH 2건(action 누수, 디코더 스레드와의 data race)과 MEDIUM 3건 | 중 | cpp-reviewer | — |
| D-14 | 2 | perspective 구성 | 5개: Default / Bitstream debug / YUV compare / RD / BD-rate / ME / GM, View > Perspective, **Alt+1–Alt+5** (Alt+숫자는 미사용 확인). 각 perspective 는 "보일 dock 집합 + 앞에 둘 dock" 으로 코드에 정의, "Save Layout to This Perspective" 로 사용자 배치를 schema 번호와 저장, 복원 실패 시 코드 정의로 | Ctrl+Alt+숫자 / 사용자 정의만 | Ctrl+1–8 은 View State 가 쓴다. 코드 정의가 기준이어야 dock 이름이 바뀌어도 깨지지 않는다(architect) | 중 | Progy, architect (D-05 와 함께) | 메뉴 항목 제거 |
| D-15 | 2 | tool dock 생성·동작 | dock 5개는 시작할 때 만들어 아래쪽에 RD plot 과 탭으로 숨겨 둔다(안내 문구만). tool 은 처음 쓸 때 만들어 dock 에 넣는다. dock 을 닫으면 tool 을 닫는 것과 같다(실행 중 작업 취소 — 기존 closeEvent). tool 이 스스로 닫히면(Esc, Close) dock 을 숨긴다 | tool 까지 시작 시 생성 | 저장된 창 상태와 perspective 가 배치하려면 dock 이 처음부터 있어야 한다. tool 생성은 늦게 두어 테스트 45 의 "창을 만들지 않는다"를 지킨다 | 중 | architect | ToolDock 대신 이전 show/raise |
| D-16 | 2 | Properties 분리 | 압축 비디오·raw item 의 Properties 를 **Video / Overlays 탭**으로. 마지막에 고른 탭으로 다음 item 을 연다. helper 는 `src/integration/PropertyTabs` | 표시/처리 두 dock | 한 dock 안의 긴 스크롤이 문제였다. dock 을 늘리면 Phase 0 에서 줄인 패널 수가 다시 는다. overlay toolbar 가 생겨 Overlays 탭을 매번 열 필요는 줄었다 | 중 | — | helper 호출 제거 |
| D-17 | 2 | 설정 통합 범위 | 조사 결과 integration 의 QSettings 는 대부분 "마지막 사용값"(폴더, 체크 상태)이라 설정 창 대상이 아니다. 설정 창에 **H.264 기본 디코더**(JM / FFmpeg, 빠져 있던 항목)만 추가. 범례 표시는 View 메뉴에 이미 있고 저장되므로 설정 창에 두면 메뉴 체크와 어긋날 수 있어 뺐다 | 모든 키를 설정 창으로 | 마지막 사용값을 설정 창에 두면 오히려 헷갈린다 | 하 | — | 항목 제거 |
| D-17a | 2 | 발견한 기존 결함 (미수정) | Settings 에서 OK 를 한 번 누르면 `DefaultDecoderHEVC` 가 콤보 첫 항목(libde265)으로 저장돼 HM 우선 선택이 무시될 수 있다. 이번 범위 밖이라 기록만 | 이번에 수정 | Phase 범위 밖. 별도 작업으로 확인 필요 | — | — | — |
| D-18 | 3 | timeline 데이터 출처 | 컨테이너(ivf/obu/mp4)는 열 때 이미 도는 `FileSourceFFmpegFile::scanBitstream` 이 packet 마다 {크기, pts, keyframe} 를 **버리지 않고 저장**하게 하고 읽기 전용 accessor 로 낸다(추가 I/O 없음, 열 때 한 번 채우고 안 바뀜 → lock 불필요). AV1 프레임 = packet(TU) 순번. 타입은 비트스트림 parser 의 `FrameObuIndex`/`FrameAvcIndex` 가 채워지는 대로 갱신(그 전엔 key/비key 2색). Annex-B 는 parser 의 프레임 범위로 크기. raw YUV 는 같은 높이 막대 | hexdump 의 순차 demuxer / BitstreamAnalysis parser 만 사용 | demuxer 는 GUI 스레드 순차 읽기라 전체에 못 쓴다. parser 는 첫 item 만, 기본 500 프레임까지만 돈다(Progy 조사) | **상** | Progy (85%), Cody | accessor·timeline 제거 |
| D-19 | 3 | timeline 표시 규칙 | 막대 = 프레임(AV1 은 TU, 숨은 ARF 바이트는 다음 표시 프레임 막대에 포함), 높이 선형 기본 + log 전환, 색 AV1 KEY 빨강 / INTRA_ONLY 주황 / INTER 파랑 / SWITCH 보라, show_existing 은 얇은 막대+표식, H.264/HEVC I 빨강 / P 파랑 / B 초록. 클릭 = 이동, Shift+드래그 = 구간 선택. 두 item 이 선택되면 B 의 막대를 윤곽선으로 겹친다 | Cody 권장 decode 순서 + 숨은 프레임 빗금 | TU 안의 숨은 프레임을 따로 떼려면 OBU 단위 크기 파싱이 필요하다(이번 범위 밖). 재생 frame index 가 TU 순번이라 클릭 이동이 그대로 맞는다. A/B 겹침은 Cody 의 "이 팀에 가장 쓸모" 의견 | 중 | Cody (75%) | — |
| D-20 | 3 | Problems 목록 v1 | 항목 = {item, frame, 위치, 심각도, 출처, 문구}. 출처 v1: **Find diff 결과**(첫 차이 + 차이 난 프레임·SB 마다 한 줄, 클릭 = 그 위치로 이동)와 **디코더 오류**. parser 오류 행·stderr 경고·크기 이상치는 뒤로 미룸 | 범용 경고 수집 틀 | Cody 순위 1(find-diff 첫 분기)이 팀 실제 작업이다. dav1d 경고는 qWarning 문자열뿐이라 데이터가 없다(Progy). 이상치는 타입·시간 계층 정규화 없이는 KEY·ARF 만 잡힌다 | 중 | Cody, Progy | dock 숨김 |
| D-21 | 3 | heatmap 범위 | 타입마다 범위 방식: **고정(기본)** / **프레임에 맞춤(대비 모드)**. 맞춤은 그릴 때 그 프레임 값의 min/max 로 지역 사본 mapper 를 쓰고 저장된 colorMapper 는 건드리지 않는다. 범례에 방식과 실제 범위를 항상 표시. 전체 영상 범위는 미룸 | 맞춤을 기본으로 / 전체 영상 범위 | 프레임마다 같은 색이 다른 값을 뜻하면 프레임 비교가 틀린다(Cody: 가장 오해 소지). 전체 영상 범위는 전 프레임 디코드가 필요해 비싸다 | 중 | Cody, Progy | 방식을 고정으로 |
| D-22 | 3 | 구간 집계 | timeline 의 Shift+드래그 구간에 대해: 총 바이트, fps 기준 bitrate, 타입별 개수·바이트, 최대·최소 프레임(번호), B item 이 있으면 A/B 바이트 차이(%) | 평균 바이트 중심 | Cody 의 중요도 순서. 평균은 가장 덜 중요 | 하 | Cody | — |
| D-23 | 3 | session 파일 | **최소 형식** `.bdsession` (JSON): schema, playlist XML(경로는 session 파일 기준 상대), perspective, 창 상태. playlist XML 에 이미 현재 프레임·줌·split 이 들어 있다. File > Save Session / Open Session | 모든 상태 직렬화 | Cody: 전체 직렬화는 유지 비용이 더 크다. 필요한 조각(`getPlaylistString`, `loadPlaylistFromByteArray`)이 이미 있다(Progy) | 중 | Cody, Progy | 메뉴 제거 |
| D-07a | 3 | D-07 개정: 같은 클릭 | view 클릭은 같은 블록이어도 **항상 전달**, 같은 값 차단은 패널발 이동(Jump)에만 | 모든 출처에 차단 | 검토(cpp-reviewer): 같은 블록 재클릭이 hex dump 창을 다시 여는 기존 동작을 잃었다 | 중 | cpp-reviewer | — |
| D-24 | 3 | 검토 반영 (Phase 2·3) | 디코더 오류 행은 item 포인터 기준·문구가 바뀔 때만 갱신(재생 중 매 프레임 재생성 방지), `getDecoderError` 는 try-lock, 맞춤 범위는 다각형 값도 면적으로 나누고 유한값만, Find diff 창에 비교 대상이 없으면 Problems 클릭은 프레임 이동, Open Session 은 playlist XML 검증 후에만 기존 목록 삭제, Save Session 은 QSaveFile, 크기 목록이 프레임 수와 다르면 크기 없이 표시, pts 없는 packet 은 정렬 안 함 | 그대로 둠 | 독립 검토 MEDIUM 5건 + LOW 일부 | 중 | cpp-reviewer | — |
| D-25 | 3 | 알려진 한계 (미수정) | 숨긴 tool dock(perspective 전환·fullscreen)의 실행 중 작업은 계속 돈다(닫기만 취소 — 패치 전 창 숨김과 같음). 범례의 맞춤 범위는 마지막으로 그린 view 기준. `fitRangeToFrame` 은 playlist 에 저장되지 않음. AV1 숨은 ARF 바이트는 다음 표시 프레임 막대에 포함 | 이번에 처리 | 범위·비용 대비 효과가 작다. 문서에 남기고 다음 단계로 | 하 | cpp-reviewer | — |
| D-26 | 3 | 설정 쓰기 최소화 | perspective·범례 설정은 값이 바뀔 때만 QSettings 에 쓴다 | 매번 쓰기 | 측정: 이 머신의 홈 디스크에서 4 KB fdatasync 가 2–5초. QSettings 쓰기마다 동기화가 일어나 테스트 66 이 300초를 넘겼다(행처럼 보임). 원인은 디스크 부하(인프라)지만 불필요한 쓰기는 없앤다 | 하 | — | — |
