---
title: TASK-0005 뷰 클릭 시 블록 경계 하이라이트 + 블록 모드 정보 pane
status: in-progress
created: 2026-07-31
updated: 2026-08-01
author: claude-opus-5
verified: yes
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 요구

AV1 디코딩 뷰에서 화면을 클릭하면
1. 그 위치의 **활성 블록 경계를 사각형으로 하이라이트**
2. **블록 모드 정보를 별도 pane** 에 표시

## ✅ 구현 완료 (2026-08-01) — GUI 실측 확인

3개 서브에이전트(데이터 계층 / 뷰 계층 / pane)의 계획을 취합해 구현했다.

```
Block Info                        <- Ctrl+B 로 여는 신규 dock (좌측)
Frame 0 · pixel (68, 108)
Syntax Element              | Value
▾ Block 32×32 @ (64, 96) — coding block
     Pred Mode              | INTRA (0)
     Segment ID             | 0
     skip                   | 0
     skip_mode              | 0
```
그리고 영상 위에 해당 32×32 블록이 **딥핑크 외곽선 + 어두운 halo** 로 표시된다
(VQAnalyzer 의 "selected CU is outlined with a pink box" 와 동일).

회귀 스위트 9/9 통과. 신규 테스트 `tests/regression/09-block-info-query.cpp` 추가.

## 시각화 기준 — VQAnalyzer (사용자 요청)

VQAnalyzer 6.6.0 User Guide 에서 확인한 실제 동작. 이것을 목표로 삼는다.

**메인 패널**
- **"The selected CU is outlined with a pink box."** ← 하이라이트 요구사항의 원형
- "Clicking and dragging moves the picture around, and the mouse wheel zooms in or out
  around the mouse cursor." → 드래그는 패닝, 클릭이 선택
- "CTB row and column values are displayed along the left and top border of the picture,
  slices are indicated with thick red lines, dependent slice boundaries are dashed red lines
  and tile boundaries are thick green lines."
- "Most modes can also show details of the current selection (CU, MB …). This can be toggled
  with the right mouse button or the main panel's button strip."
- 좌상단에 현재 모드, 우상단에 배율 표시

**Syntax Info 패널**
- View 메뉴에서 on/off, 좌측에 resizable dock, **하단에 탭**
- AV1 탭 구성: `AV1 IVF` / `AV1 OBU` / `Sequence` / `Frame` / **`AV1 Block`** / `AV1 Refs` / `AV1 Stats`
  → 우리가 만들 pane 은 **`AV1 Block` 탭**에 대응한다
- "Bringing the mouse to the tab name shows explanation of its abbreviated name." → 툴팁으로 설명
- 패널은 detach / hide 가능, 타이틀바 더블클릭으로 재부착

> 우리 쪽에서 **바로 따라갈 수 있는 것**: 선택 블록 외곽선, dock+탭 구조, 값 설명 툴팁
> (`StatisticsType::description` 이 이미 있다).
> **데이터가 없어 못 하는 것**: slice/tile 경계선과 CTB 행·열 인덱스는 통계가 아니라
> 파서(`ParserAV1OBU`)에서 나온다. AV1 은 `tile_info` 파싱은 있으나 `StatisticsData` 로
> 연결되어 있지 않다 → **이번 범위 밖** (별도 태스크)

## 결론 — 가능하다. 침습도는 중간, 그리고 대부분의 부품이 이미 있다

| 필요한 것 | 상태 |
|---|---|
| 픽셀 → 블록 찾기 | ✅ **이미 있다.** `StatisticsData::getValuesAt()` (`statistics/StatisticsData.cpp:197`) 가 `QRect(pos, size).contains(pos)` 로 정확히 이 판정을 한다 |
| 블록 사각형 정보 | ✅ `StatsItemValue` / `StatsItemVector` 가 `pos[2]`, `size[2]` 를 들고 있다 (`statistics/FrameTypeData.h:70-85`) |
| 사람이 읽을 모드 이름 | ✅ `StatisticsType::setMappingValues({"INTRA","INTER"})` + `getValueTxt()` (`decoderDav1d.cpp:592`) |
| 타입 설명 문구 | ✅ `StatisticsType::description` (`decoderDav1d.cpp:591`) |
| 위젯 좌표 → 픽셀 좌표 변환 | ⚠️ 있지만 **재사용 불가 형태** — `SplitViewWidget::paintEvent` 안에 인라인 (`ui/views/SplitViewWidget.cpp:565-580`) |
| 클릭 이벤트 | ⚠️ `splitViewWidget::mousePressEvent` (`:979`) 존재. 단 **좌클릭이 이미 점유됨** (split line 드래그 / `MOUSE_LEFT_MOVE` 패닝) |
| 정보 pane | ⚠️ dock 5개 선례 있음 (`ui/mainwindow.ui`: playlist/properties/playback/fileInfo/cachingInfo). 신규 dock 추가 필요 |
| 하이라이트 사각형 그리기 | ❌ 신규. 단 `stats::paintStatisticsData()` 호출 지점(`playlistItemCompressedVideo.cpp` `drawItem`) 바로 옆이라 훅은 깨끗하다 |

⚠️ 아래는 **코드 조사 기반이며 구현·실측하지 않았다.**

## 핵심 이점 — 판정 로직을 새로 만들 필요가 없다

`getValuesAt()` 이 이미 이렇게 동작한다:

```cpp
for (const auto &valueItem : this->frameCache.at(it->typeID).valueData)
{
  auto rect = QRect(valueItem.pos[0], valueItem.pos[1], valueItem.size[0], valueItem.size[1]);
  if (rect.contains(pos))            // <-- 우리가 필요한 바로 그 판정
  {
    auto valTxt = it->getValueTxt(value);   // <-- 이미 "INTRA"/"INTER" 로 변환해 준다
    valueList.append(QStringPair(it->typeName, valTxt));
  }
}
```

**버리는 것이 `rect` 뿐이다.** 즉 `getValuesAt` 을 rect 까지 돌려주는 형태로 확장하면
하이라이트와 pane 이 동시에 해결된다.

### 그리고 `getPixelValues` 가 이미 절반을 한다

`playlistItemCompressedVideo::getPixelValues()` 가 YUV 픽셀값 + `statisticsData.getValuesAt()`
결과를 합쳐 돌려준다. 지금은 **줌 박스 오버레이**가 이걸 쓴다 (hover 기반, `SplitViewWidget` 이
직접 텍스트를 그린다). pane 으로 보내는 배선만 새로 만들면 된다.

## 설계상 결정해야 할 것 4가지

### 1. ✅ 결정 — 지정 타입 방식 (기하학적 추론은 **틀렸다**)

**채택: 디코더가 코딩/변환 블록 타입 ID 를 명시 지정한다** (`setBlockRectTypeIDs`).
dav1d 는 `(0 = Pred Mode, 26 = Transform Size)`.

원래 "가장 큰 rect = 코딩 블록" 이라는 기하학적 규칙을 고려했는데,
**데이터 계층 에이전트가 이걸 배제했다**: libde265 는 typeID 0 이 CTB rect 위의 slice index 이고
CU 보다 크다 → 기하학으로는 CTB 를 고른다. 그래서 명시 지정 + (미지정 시) 최대 rect 폴백.

실측 근거 (전 픽셀 스캔): 인트라 프레임에서 **변환 블록 ≠ 코딩 블록인 픽셀이 47%** →
둘을 합치면 정보를 잃는다. 그래서 코딩 블록(굵은 선) + 변환 블록(얇은 점선) 을 모두 그린다.

아래는 원래 검토 기록:

실측: `decoderDav1d.cpp` 의 `addBlockValue`/`addBlockVector` 호출 21곳 중
**20곳이 `cbPosX/cbPosY/cbWidth/cbHeight`(코딩 블록)** 를 쓰고, 1곳(타입 26, transform size)만
`x_abs`(변환 블록) 를 쓴다.

→ **코딩 블록 rect 가 사실상 "그 블록" 이다.** 정책 후보:
- (a) cb 기반 타입 중 하나를 기준으로 삼는다 (예: 타입 0 `Pred Mode` — 항상 채워진다)
- (b) 클릭 지점을 포함하는 **모든** rect 를 모아 가장 작은 것을 하이라이트
- (c) 코딩 블록(굵은 선) + 변환 블록(얇은 선) 을 **둘 다** 그린다 → 정보량이 가장 많다

→ **(c) 를 권장.** AV1 은 coding block 과 transform block 이 다른 것이 요점이므로,
분석 도구로서 둘을 구분해 보여주는 게 맞다. 1차 구현은 (a) 로 시작해도 된다.

### 2. ✅ 결정됨 — 상호작용은 VQAnalyzer 방식 (2026-08-01, 사용자 확정)

**이동 없는 좌클릭 = 선택, 드래그 = 패닝.** VQAnalyzer 가 같은 규칙이다
("Clicking and dragging moves the picture around" + 클릭으로 CU 선택).
`mouseMode` 설정으로 패닝 버튼이 좌/우로 바뀌므로 **두 설정 모두 동작해야 한다.**

아래는 검토 기록으로 남긴다 — 다른 후보와 그 이유:

`splitViewWidget::mousePressEvent:1006` 이 좌클릭을 split line 드래그에 쓰고, 그 외에는
`MoveAndZoomableView::mousePressEvent` 로 넘겨 **패닝**에 쓴다 (`mouseMode == MOUSE_LEFT_MOVE`).
`mouseMode` 는 설정으로 좌/우가 바뀐다 (`MoveAndZoomableView.h:136-141`).

후보:
| 방식 | 평가 |
|---|---|
| **press→release 사이 이동이 없으면 "클릭"** | 패닝과 공존한다. 드래그 임계값(수 px)만 두면 됨. **1차 권장** |
| 더블클릭 | 충돌 없음. 단 발견성이 낮다 |
| 수정키 (Ctrl+클릭 등) | 확실하지만 조작이 번거롭다 |
| 우클릭 | ❌ 컨텍스트 메뉴 / `MOUSE_RIGHT_MOVE` 와 충돌 |

(결정 완료. 위 표는 근거 기록용.)

### 3. ✅ 해결 — `blockInfoRequested` 플래그 (내가 문서에 적었던 진단은 **틀렸다**)

이 문서 초판은 "`getValuesAt` 의 `renderGrid` 필터를 우회하면 된다"고 적었다. **틀렸다.**
데이터 계층 에이전트가 실측으로 밝힌 것:

- `renderGrid` 는 기본값이 **true** (27/27) 이라 실제 게이트가 아니다
- 진짜 게이트는 `StatisticsData::getTypesThatNeedLoading` 의 `statsType.render` 이고,
  `render` 는 기본값 **false** → **`frameCache` 자체가 비어 있다**. 필터 문제가 아니라 **데이터 부재**

→ `StatisticsData::blockInfoRequested` 를 추가해 `needsLoading()` 에서 `render` 와 OR 한다.
그러면 기존 `needsLoading → loadFrame → loadStatistics` 경로가 그대로 돌아 수집이 시작되고,
**오버레이는 켜지지 않는다** (실측: 수집 후에도 `rendered types = 0`).
비용은 디코더 리셋 + GOP 재디코딩 1회 (176x144 에서 2 ms 실측).

⚠️ **구현 중 발견한 함정**: 첫 클릭 시점에는 아직 수집이 안 끝나 뷰의 캐시가 빈 rect 로 남는다.
그래서 `update()` 에서 "선택은 유효한데 rect 가 비어 있으면" 재조회하도록 했다.
이것 없이는 **첫 클릭에 하이라이트가 안 나온다** (실제로 그랬다).
또한 뷰가 직접 `setBlockInfoRequested(true)` 를 호출한다 — pane 을 닫아도 하이라이트는 나와야 한다.

아래는 원래 검토 기록:

블록 정보는 `statisticsData` 에서 나오고, 그건 디코더가 채운다. 그런데:
- `playlistItemCompressedVideo::loadStatistics()` 는 `statisticsEnabled()` 가 false 면
  `enableStatisticsRetrieval()` 후 **디코더를 리셋하고 다시 디코딩**한다
- `decoderDav1d::allocateNewDecoder()` 는 `statisticsEnabled()` 가 **이미 참일 때만**
  `export_blkdata = 1` 을 세팅한다 (`decoderDav1d.cpp:276-280`)
- 게다가 `getValuesAt()` 은 `if (!it->renderGrid) continue;` — **사용자가 렌더링을 켠 타입만** 본다

→ 클릭했는데 아무 통계도 활성이 아니면 **빈 결과**가 나온다. 대응:
- `getValuesAt` 의 `renderGrid` 필터를 우회하는 변형이 필요하다 (pane 은 렌더링 여부와 무관하게
  전 타입을 보여줘야 한다)
- 통계 수집 자체를 on-demand 로 켜는 경로가 필요할 수 있다. **동작 확인 필요 (미검증)**

### 4. 선택 상태의 수명

프레임이 바뀌면 같은 좌표라도 블록 분할이 달라진다. 무효화/재조회 트리거:
프레임 변경, 아이템 변경, **디코더 전환**(타입 목록이 바뀐다), 통계 on/off, 파일 재로드.

## 추가 기능 — 줌 박스 info panel 을 syntax 표시로 전환 (2026-08-01)

요구: "YUV view click 시 선택된 block 에 대해 details view 에 syntax 정보를 display 가능하도록
check box 를 추가하고, 선택하면 YUV pixel 정보 대신 해당 block 의 position 및 모든 syntax 를 표시".

**"details view" = 줌 박스 옆 info panel** 이다 (`splitViewWidget::paintZoomBox` 의
`drawInfoPanel` 분기). 지금까지 `Coordinates` + `YUV` 픽셀값 + `Stats` 를 그려 왔고, 이게
"YUV pixel 정보" 에 해당한다.

- 체크박스는 **split view 메뉴의 체크 항목** `Show Block Syntax` 로 넣었다 (`Zoom Box` 바로 아래).
  이 패널을 켜고 끄는 `Zoom Box` 가 이미 같은 자리에 있고, 뷰 설정은 이 코드베이스에서
  전부 checkable QAction 으로 다뤄진다 (`actionZoomBox` 패턴). 분리 창에도 링크 전파된다.
- 켜면 `getPixelValues()` 를 아예 호출하지 않고 **클릭으로 선택된 블록**의
  `Frame / Position / Size (+ Transform)` 과 **모든 syntax 항목**을 그린다.
  hover 위치가 아니라 클릭한 블록을 쓴다 — 마우스를 움직여도 읽고 있던 값이 흔들리지 않아야 한다.
- 이를 위해 `BlockSelection` 이 rect 두 개만 캐시하던 것을 **`stats::BlockInfo` 전체 캐시**로 바꿨다
  (entries 가 필요하다). 조회 비용 때문에 per-repaint 조회는 여전히 하지 않는다.

실측 (GUI):
```
Zoom Box 만 켠 상태      : Coordinates / YUV (Y,U,V) / Stats     <- 기존 동작
+ Show Block Syntax      : Block  Frame 0, Position 64,96, Size 32x32
                           Syntax Pred Mode INTRA(0), intra pred mode (Y) CFL_PRED(13),
                                  intra pred mode (UV) VERT_PRED(1), intra angle delta (Y) 1,
                                  Intra direction chroma (0,16), ...
```

## codex 리뷰 (2026-08-01) — `VERDICT: FIX`, 8건 중 7건 반영

| 심각도 | 지적 | 처리 |
|---|---|---|
| High | `blockInfoRequested` 가 **모든** 타입이 `frameCache` 에 있기를 요구 → 조건부 타입(palette/CfL 등)이 없는 프레임에서 `needsLoading` 이 **영구히 NEEDED** → 프레임 무한 재디코딩 | ✅ 수정. "하나라도 수집됐으면 됨" 으로 변경. **실측 확인: `NEEDED` 영구 반복 → `not-needed`** (27개 중 21개만 데이터를 가진다) |
| High | 디코더 전환은 같은 프레임에 `itemRedraw` 만 내므로 **이전 디코더의 rect 가 계속 그려진다** | ✅ 수정. `itemRedraw` 시에도 재조회 |
| High | dav1d 통계 쓰기가 `accessMutex` 밖 → 새 리더와 데이터 레이스 | ✅ 수정. `cacheStatistics` 전체를 프레임 단위로 락 (블록 단위가 아니라 프레임 단위라 락 경합이 없다) |
| Medium | 벡터 중복 제거 키가 **같은 타입·rect 의 스칼라와 충돌** → libde265/HM 의 intra-dir 벡터가 사라진다 | ✅ 수정. 벡터는 벡터값으로 키를 만들고 스칼라와 분리 |
| Medium | 미지정 디코더에서 "최대 rect" 폴백이 **CTB 를 하이라이트**할 수 있다 (libde265 typeID 0 = CTB) | ✅ 수정. 폴백을 **최소 rect** 로 변경 (전체 CTB 를 "그 블록" 이라 하는 것보다 낫다). 정확히 하려면 디코더가 지정해야 함을 주석에 명시 |
| Medium | 각 뷰가 자기 `blockSelection` 을 가져 **분리 창에서 클릭해도 주 뷰의 낡은 하이라이트가 남는다** | ✅ 수정. 한쪽에서 선택하면 다른 쪽을 지운다 (`getOtherWidget`) |
| Low | 선택 해제 시 `setBlockInfoRequested(false)` 를 안 불러 **수집이 계속 켜져 있다** | ✅ 수정. 뷰가 플래그를 소유(선택 시 on, 해제 시 off), pane 은 관여하지 않음 — 둘이 서로 끄려고 다투지 않게 |
| Medium | 더블클릭의 첫 release 가 전체화면 토글 전에 블록을 선택한다 | ⚠️ **미수정, 수용.** 무해하다(전체화면은 정상 동작하고 블록도 선택될 뿐). 억제하려면 타이머가 필요해 비용이 이득보다 크다 |

codex 평가: "Paint rect math/clip/save-restore looked consistent with video/stat painting."

## 3개 에이전트 계획의 상충과 해결

| 상충 | 해결 |
|---|---|
| **뷰**는 `paintEvent` 안에서 매 repaint 마다 조회하려 했다. **데이터**는 조회 비용을 1080p 1 ms / 4K 15 ms 로 측정하고 "클릭 시에만" 을 요구했다 | **캐시 방식 채택.** 클릭·프레임변경 시 1회 조회해 rect 를 저장하고, `paintEvent` 는 저장된 rect 만 그린다. 뷰가 원했던 "프레임 넘겨도 따라오는" 동작은 `update(newFrame)` 훅으로 유지 |
| 시그널 이름/인자 순서가 달랐다 (`blockSelected(item,pos,frame)` vs `selectedBlockChanged(item,frame,pos)`) | 뷰가 시그널 소유자이므로 **`blockSelected(item, pixelPos, frameIdx)`** 채택 |
| **뷰**는 가벼운 `{codingBlock, transformBlock}` 만, **pane**은 status+entries 를 원했다 | 캐시 방식이므로 풍부한 조회 1개로 충분 → 데이터 계층의 `BlockInfo` 하나로 통일 |
| **pane**은 `PlaybackController::currentFrameChanged` 신규 시그널을 요구. **데이터**는 "프레임 시그널이 아예 없다" 며 데이터 기반(frameIndex 비교) 방어를 권고 | **둘 다 불필요.** `splitViewWidget::update(newFrame, itemRedraw)` 가 이미 프레임 변경을 안다 → 거기서 재조회. `PlaybackController` 를 건드리지 않아 diff 가 줄었다. frameIndex 비교는 정합성 보장으로 유지 |
| **뷰** 요구 4 "`render` 여부와 무관하게 결과를 줘야 한다" 는 그대로는 **불가능** (데이터가 없음) | `blockInfoRequested` 로 수집을 켜서 요구를 실질적으로 만족 |

## 손대야 할 곳 (5곳)

| # | 파일 | 변경 | 성격 |
|---|---|---|---|
| 1 | `statistics/StatisticsData.{h,cpp}` | `getValuesAt` 옆에 **rect 까지 돌려주는** 변형 추가 (`getBlockInfoAt(pos)` → 타입ID/이름/값텍스트/설명/rect 목록). `renderGrid` 필터 없이. 기존 함수는 그대로 두어 줌 박스 회귀를 막는다 | 추가적 |
| 2 | `ui/views/SplitViewWidget.{h,cpp}` | ① `paintEvent:565-580` 의 좌표 변환을 **재사용 가능한 함수로 추출**(줌 박스도 그걸 쓰게) ② `mousePressEvent`/`mouseReleaseEvent` 에 이동 없는 클릭 판정 ③ 선택 좌표 보관 + `signalBlockSelected(QPoint pixelPos)` emit ④ 선택 블록 rect 를 그림 | **경미하게 침습적** (좌표 변환 추출이 핵심) |
| 3 | **신규** `ui/widgets/BlockInfoWidget.{h,cpp}` + `ui/blockInfoWidget.ui` | 타입/값/설명 테이블. `FileInfoWidget`(62+153줄) 이 가장 가까운 선례 | **추가적.** qmake glob 이 자동 인식 → 빌드 파일 수정 0 |
| 4 | `ui/mainwindow.ui` + `ui/Mainwindow.cpp:366-373` | `blockInfoDock` 추가 + `addDockViewAction(...)` 1줄 (기존 5개 dock 과 동일 패턴) | 추가적, ~5줄 |
| 5 | `playlistitem/playlistItemCompressedVideo.cpp` | `drawItem` 의 `stats::paintStatisticsData()` 호출 옆에 선택 블록 하이라이트 그리기 + 선택 좌표 → 블록 정보 조회 진입점 | 경미하게 침습적 |

> upstream 을 건드리는 fork 이므로 **패치 파일로 관리**해야 한다
> (`third_party/yuview/patches/`, `build.sh` 가 자동 적용). 좌표 변환 추출(2①)이
> 리베이스 충돌 가능성이 가장 높은 부분이다.

## 작업 순서 (제안)

### 1단계 — 데이터 경로 ✅
- [x] `statistics/BlockInfo.h` 신규 + `StatisticsData::getBlockInfoAt(QPoint)` (rect 포함, `render`/`renderGrid` 무시)
- [x] 중복 제거 (fork 가 블록을 2~4회 내보낸다), 겹치는 블록은 **작은 것 우선**
- [x] `blockInfoRequested` 로 수집 강제 + `clear()` 에서 타입 지정 리셋
- [x] `playlistItem::getBlockInfoAt` / `setBlockInfoRequested` 가상 함수 + 압축비디오 override (staleness 체크 포함)
- [x] 회귀 테스트 `09-block-info-query.cpp` — 기본 상태에서 빈 결과 → 요청 후 데이터, `renderGrid` 무관, 범위 밖/stale 프레임 처리

### 2단계 — 하이라이트 ✅
- [x] 좌표 변환: **추출하지 않고 복제** (`updatePixelPositions` 는 하나의 diff 를 양쪽 아이템에 적용하는
      줌 박스 전용 의미라 선택에는 틀리다. 뷰 에이전트가 691,200 케이스 등가성 프로브로 확인)
- [x] 이동 없는 클릭 판정 — upstream 의 기존 임계값 재사용 (새 상수 없음), 두 `mouseMode` 모두
- [x] `paintEvent` 3곳에서 그리기. cosmetic pen (없으면 zoom 32 에서 선이 48px), `moveCenter` rect 수학

### 3단계 — pane ✅
- [x] `BlockInfoWidget` (`.ui` 없음 — `FORMS` glob 이 **비재귀**이고 `YUViewLib/ui/` 를 가리킨다)
- [x] `mainwindow.ui` dock + `Ctrl+B` view action + `panelsVisible[6]`
- [x] 배선: 양쪽 뷰의 `blockSelected`, playlist 선택 변경, `selectedItemChanged` → `refresh()`

### 4단계 — 검증
- [x] GUI 실측: Ctrl+B → 클릭 → 분홍 박스 + pane 값 표시
- [x] 회귀 9/9
- [ ] **VQAnalyzer 교차검증** — 같은 스트림·좌표의 블록 모드가 일치하는가 (미실시)
- [ ] 디코더 전환 / 프레임 이동 / 통계 on-off 중 클릭 안정성 (미실시)
- [ ] 4K 스트림에서 조회 비용 체감 확인 (측정치 15 ms, 실사용 미확인)

## 전제

- **AV1 블록 통계가 나와야 한다** → dav1d analyzer fork 필요.
  → [dav1d-block-statistics.md](../10-research/dav1d-block-statistics.md), [TASK-0003](TASK-0003-av1-parsing-dav1d.md)
- HEVC(libde265) / VVC(VTM) 도 같은 `StatisticsData` 구조를 쓰므로 **이 기능은 코덱 중립**이다.
  AV1 전용으로 만들 이유가 없다
- H.264 는 FFmpeg 경유 통계가 4종(Source/MV)뿐이라 블록 모드 정보가 거의 없다
  → [yuview-feature-gap.md](../10-research/yuview-feature-gap.md) A절

## 관련

- 기능 A' (블록 단위 오버레이) 의 사용성 확장이다. 오버레이 자체는 이미 동작한다
  (통계 24종 렌더링 확인) — 이 태스크는 **"어느 블록인지 집어서 읽는" 부분**을 더한다
- [TASK-0004](TASK-0004-decoded-yuv-cache.md) 가 통계를 `.YUViewBD` 에 CSV 로 저장하는 계획인데,
  저장된 통계를 되읽어도 `StatisticsData` 로 들어오므로 **이 기능이 그대로 동작해야 한다**
