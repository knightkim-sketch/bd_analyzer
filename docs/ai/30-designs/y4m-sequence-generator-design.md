# Y4M sequence generator — 설계

캔버스에 도형(사각·원·삼각·별)이나 이미지 패치를 그려 레이어마다 하나의 물체로 두고, 물체마다 움직임을 주어
설정한 프레임 수만큼 합성한 y4m 을 만든다. 만든 y4m 은 playlist 에 바로 열어 재생·ME·Global motion·비교에 쓴다.

- 요청: Knight, 2026-10-15. 결정은 Knight 부재(자동 선택 모드) 중 agent 검토(Plan, Feedy)의 권장안으로 내렸다.
  내일 검토에서 바뀔 수 있는 결정은 아래 표에 근거와 함께 남긴다.

## 1. 결정: 별도 창 (YUV 뷰 안이 아님)

| 근거 | 내용 |
|---|---|
| 분할 뷰는 보기 전용 | `splitViewWidget` 의 마우스 입력은 블록 선택(0063 Shift+클릭 포함)에 쓰인다. 도형 생성·이동·크기 조절을 넣으려면 이미 0058·0062·0063·0066·0067 이 걸린 파일에 큰 패치가 필요하다 |
| 기존 창 패턴 | `GlobalMotionWindow`·`StreamDiffWindow`·`MeCompareWindow` 와 같은 QDialog. upstream 변경은 메뉴 한 줄 + 슬롯 + `.pro` glob |
| 결과는 기존 뷰어로 | 생성 → `.y4m` 저장 → `MainWindow::loadFiles` 로 playlist 에 열림. 재생·ME·GM·비교를 그대로 쓴다 |
| 장면은 아이템이 아님 | 속성 패널은 playlist 아이템마다 있다. 편집 중인 장면은 아이템이 아니다 (나중에 ADR-0003 의 playlist generator 아이템은 별도 과제) |
| Qt 없는 코어 | `src/me` 처럼 `src/generator` 를 Qt 없이 두어 단위 테스트·CLI 에서 같은 렌더를 쓴다 |

## 2. 기본값과 범위 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| 메뉴 | File 메뉴, 옆 작업(0066)의 "Export Filtered Y4M..." 다음 "Generate Y4M Sequence..." | View 메뉴의 ME 블록(0066·0067)과 충돌 회피 |
| 기본 크기 | 1280×720, 30 fps, 60 프레임 — 모두 편집 가능 | 일반적인 테스트 크기 |
| 형식 | 4:2:0 8-bit, `C420jpeg` | ME 입력 관례 (8-bit luma) |
| 색 | YUV 로 저장, RGB 선택은 BT.709 limited 로 변환 | ME 비교 팝업(0063)과 같은 관례 |
| 배경 | 회색 단색 (Y 128) | ME 정답 확인이 쉬움 |
| 움직임 | 정수 MV(x, y) 픽셀/프레임 + 시작·끝 프레임이 기본. 회전·배율/프레임, wrap·bounce 는 코어에 있으나 기본값은 꺼짐 | GT 와 full-pel ME 를 바로 비교할 수 있게 |
| 경계 | 기본은 화면 밖으로 잘림 | 예측 가능 |
| 합성 | 아래 레이어부터 위로 그리고 위가 덮는다. 알파 블렌딩 `(a·src + (255−a)·dst + 127) / 255` | 결정적, 단순 |
| 래스터 | 픽셀 중심이 도형 안이면 칠함 (안티앨리어싱 없음), chroma 는 2×2 의 왼쪽 위 화소 기준 | 정확한 화소 검증 |
| 이미지 패치 | PNG/JPG/BMP 파일 또는 클립보드, `QImage` 디코딩, 최근접 샘플링 | 새 의존성 없음 (OpenCV 불필요) |
| 출력 | 저장 대화상자(마지막 디렉토리 기억) → 저장 후 playlist 에 자동 로드 | 데모 흐름 |
| 장면 저장 | JSON (이미지는 base64 RGBA 로 포함) | 재현성 |
| GT MV 출력 | 이번 범위 밖 (후속 과제) | 지시 밖 |

## 3. 구조

| 파일 | 역할 |
|---|---|
| `src/generator/Scene.{h,cpp}` | 장면 모델 (레이어·도형·움직임), `layerStateAt` (닫힌 식 움직임), `renderFrame` (래스터 + 합성), `writeY4m` |
| `src/integration/SequenceGeneratorWindow.{h,cpp}` | `GeneratorCanvas` (미리보기, 마우스 생성·선택·이동·크기 조절, MV 화살표) + 창 (시퀀스·레이어·속성·움직임 폼, 재생, Generate, 장면 저장/불러오기) |
| 패치 0068 | `YUViewLib.pro` 에 `src/generator` glob, `Mainwindow.{h,cpp}` 에 메뉴·슬롯 (`generated` → `loadFiles`) |

## 4. 검증

- `tests/unit/generator-scene.cpp` — 사각형 경계 화소, MV(3, 2) 이동, 프레임 간 이동 일치, 레이어 순서·알파·숨김, wrap, 시작 프레임, 원·별·삼각 안팎, 이미지 알파, y4m 왕복(`YuvLumaReader`), 홀수 크기 거절
- 회귀 48번 — 창을 메뉴로 열고 장면을 채워 생성 → playlist 에 열린 아이템의 크기·프레임 수·화소 확인
- 회귀 49번 — 같은 패치 묶음의 ME 패널 adv 경로 (생성기와 별개, 0067)
- 데모 (2026-10-15, scratch) — 640x360 30 프레임, 레이어 5개 (사각·원·삼각·별·이미지 패치, 각자 MV). 생성된 y4m 을 playlist 에서 adv ME (GF 8) 로 돌려 frame 10 = B, layer 3, L0 → 8 / L1 → 12. Shift+클릭 비교 팝업에서 이미지 패치 블록 MV(-4, -4) = 2 프레임 × MV(2, 2), SAD 0 으로 생성 움직임과 일치

## 5. 남은 것 (구현하지 않음)

- 정답 MV(ground truth) 출력 — 생성기는 레이어별 MV 를 알지만 파일로 내지 않는다. ME 검증용으로 다음 후보
- 부화소 움직임은 nearest 샘플링 (보간 없음), 도형 가장자리 anti-aliasing 없음 (화소 중심 포함 판정)
- 레이어 미리보기와 속성 폼은 frame 0 기준 값을 편집한다 (움직임은 닫힌 식으로 계산)
