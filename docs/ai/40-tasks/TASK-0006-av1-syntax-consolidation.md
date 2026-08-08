---
title: TASK-0006 AV1 syntax 항목 통합 + OBU frame 이동
status: implemented
created: 2026-08-07
updated: 2026-08-08
author: claude-opus-5
verified: "B/D/E 빌드·회귀·헤드리스 smoke 확인, GUI 직접 클릭 확인은 미수행"
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 배경

AV1 analyzer 의 syntax 표와 오버레이 목록이 너무 길다. 사용자가 항목 제거/통합과
OBU 탐색 기능을 요청했다. 제거(A)와 레이아웃(C)은 이미 끝났다 (커밋 `1dde6d3`, `e6d6f2e`).
이 문서는 **B/D/E 구현 기록**이다. 설계 결정은 모두 사용자와 합의됐다.

정정: OBU 더블클릭의 목표는 OBU 상세 정보를 별도로 보여주거나 Bitstream Analysis 트리로
이동하는 것이 아니다. 해당 OBU 가 속한 **YUV frame 을 보여주고 하단 frame slider/spinbox 를
현재 위치로 갱신**하는 것이다.

## 현재 등록된 통계 타입 (제거 후, 실측)

```
0 Pred Mode          1 Segment ID        2 skip              3 skip_mode
4 intra pred mode (Y)                    5 intra pred mode (UV)
8 intra angle delta (Y)                  9 intra angle delta (UV)
10 intra direction luma (벡터)           11 intra direction chroma (벡터)
12 chroma from luma alpha U              13 chroma from luma alpha V
14 ref frame index 0                     15 ref frame index 1
24 Motion Vector 0 (벡터, 빨강 w2)       25 Motion Vector 1 (벡터, 파랑 w2)
26 Transform Size
```

정의 위치: `decoder/decoderDav1d.cpp` `fillStatisticList()` / emit 은 `parseBlockPartition()`.

## B. 항목 통합 — **완료** (커밋 `1258d86`, 패치 `0019`)

문서에 적어둔 두 "확인 지점" 의 결론:

1. **angle delta 를 숨기는 별도 장치는 필요 없었다.** `getValueTxt()` 가 값→이름 매핑이라
   **mode 와 delta 를 한 값으로 인코딩**하면 타입 자체가 하나가 된다.
   값 = `mode * 8 + (delta + 4)`, 이름 = `CFL_PRED (13) d+2`.
   컬러맵은 같은 mode 면 delta 가 달라도 같은 색 → blending 은 mode 만 반영.
2. **블록 내부 값 텍스트 경로는 존재한다** (`StatisticsDataPainting.cpp:426-498`,
   `drawStatTexts` 가 지점별로 모아 `\n` 으로 합쳐 그림). 인코딩 방식이면 이 경로가
   그대로 mode+delta 를 출력하므로 그리기 코드는 건드리지 않았다.

부수 발견: `getValueTxt()` 가 이름 뒤에 raw 값을 붙여서 인코딩 값(110)이 새어 나왔다.
`StatisticsType::showRawValueInText` 를 추가해 이 타입에서만 껐다.

delta 인코딩 범위는 시그널 범위(-3..3) 보다 넓은 **-4..3** 이다. 실제 스트림이 범위를 벗어난
값을 낸 적이 있어서(`calculateIntraPredDirection` 의 bounds check 가 그 때문에 있다) 넘으면 clamp 한다.

ref frame index / motion vector 는 **표에서만** 병합했다 (`StatisticsData::getBlockInfoAt` 의
`mergeReferencePair`). 오버레이를 합치면 `vectorStyle` 이 하나뿐이라 빨강 L0 / 파랑 L1 구분이
사라지기 때문이다.

실측 결과: 체크박스 13개, `intra pred mode (Y) = CFL_PRED (13) d+2`,
compound 블록 `ref frame index = L0 0  L1 4` / `Motion Vector = L0 (0,0)  L1 (0,0)`,
MV 오버레이는 변경 전과 동일(빨강 3046px / 파랑 789px).

**남은 미검증 1건**: 블록 내부 텍스트는 `zoomFactor >= STATISTICS_DRAW_VALUES_ZOOM` 일 때만
그려진다. 높은 배율에서 실제로 화면에 찍히는지는 GUI 로 확인하지 않았다.

## D. 좌측 Info listbox 에 OBU 목록 탭 — **완료** (패치 `0021`)

- 현재 Info pane 은 `playlistItem::getInfo()` 의 key/value 목록 (`ui/widgets/FileInfoWidget.*`).
- OBU 트리의 실제 소유자는 **`BitstreamAnalysisWidget`** 이다:
  `parser::Parser` 를 만들고 `runParsingOfFile()` 을 백그라운드로 돌린 뒤
  `parser->getPacketItemModel()` 을 `ui.dataTreeView` 에 붙인다
  (`ui/widgets/BitstreamAnalysisWidget.cpp:245,256`).

### 조사에서 나온 제약 (중요)

1. **파싱은 Bitstream Analysis 탭이 보일 때만 돈다.**
   `restartParsingOfCurrentItem()` 이 `if (!this->isVisible()) return;` 로 시작한다
   (`BitstreamAnalysisWidget.cpp:223`). 좌측 Info 탭에서 OBU 목록을 보여주려면
   **파싱을 그 탭과 무관하게 시작시킬 방법**이 필요하다. 선택지:
   - ⓐ Info 탭이 열릴 때 같은 파서를 별도로 돌린다 (파일을 두 번 파싱 → 느리고 메모리 2배)
   - ⓑ `BitstreamAnalysisWidget` 의 파서를 공용으로 끌어올려 두 뷰가 같은 모델을 공유한다
     (권장. 다만 소유권/수명 정리가 필요)
   - ⓒ Info 탭을 열면 Bitstream Analysis 파싱을 트리거하고 결과를 공유한다
2. **`TreeItem` 자체에는 프레임 번호도 파일 위치도 없다** (`parser/common/TreeItem.h`:
   name / value / coding / code / meaning / streamIndex / error 뿐).
   → **해결됨(구조로):** OBU 는 독립적으로 있는 게 아니라 **패킷 트리 아이템의 자식**이다.
   `ParserAVFormat::parseAVPacket()` 이 패킷마다 `itemTree` 를 만들고
   그 아래에서 `obuParser->parseAndAddOBU(obuID, data, itemTree, ...)` 를 돈다
   (`ParserAVFormat.cpp:419-432`). 패킷 아이템은 자식으로
   `Global AVPacket Count`(= packetID), `pts`, `dts`, `flag_keyframe` 등을 갖는다
   (`ParserAVFormat.cpp:384-395`).
   → OBU 노드에서 **부모를 타고 올라가 패킷 아이템**을 찾고 그 자식에서 값을 읽으면 된다.

3. **하지만 패킷 인덱스 ≠ 프레임 인덱스다.** `Global AVPacket Count` 는 디코드 순서이고
   모든 스트림을 통틀어 센다. YUView 의 프레임 인덱스는 비디오 스트림의 표시 순서다.
   기존 매핑은 **없다** — `FileSourceFFmpegFile` 에는 `nrFrames` 와 DTS 기반 seek 경로만 있고
   packet→frame 표는 없다 (`FileSourceFFmpegFile.h:183`).
   E-1 은 우선 ⓐ 로 구현했다 (패치 `0020`): 파싱 중 `streamPacketID` 를
   `YUView frame index` 로 패킷 아이템에 넣고, OBU 더블클릭 시 그 값을
   `PlaybackController::setCurrentFrameAndUpdate()` 에 넘긴다. 기존 `FileSourceFFmpegFile::scanBitstream()`
   도 비디오 패킷을 세서 YUView 프레임 수를 만들기 때문에 현재 구조와 가장 잘 맞는 작은 변경이다.
   단, 재정렬(B-frame 등)이 없으면 프레임 인덱스와 일치한다. AV1 은 show_existing_frame 때문에
   완전히 일치한다는 보장은 없다.
   - ⓑ 패킷의 **pts 를 프레임 인덱스로 환산**한다 (timeBase + framerate). 표시 순서라 더 정확.
   - ⓒ `show_frame` / `show_existing_frame` 을 OBU 파싱에서 읽어 표시 프레임을 직접 센다.
     가장 정확하지만 파서 수정이 필요하다.
   향후 더 정확한 seek 가 필요하다고 판단되면 ⓑ 또는 ⓒ 로 승격한다.

### 구현 결과 (패치 `0021`)

- `FileInfoWidget` 가 내부적으로 `File` / `OBU` 탭을 가진다. 기존 key/value 정보는
  `File` 탭에 유지하고, `OBU` 탭은 `QTreeView` 로 `BitstreamAnalysisWidget` 의 packet model 을
  공유한다. 이 탭은 OBU 를 고르는 목록 역할이며, 더블클릭 결과 화면은 YUV view 이다.
- OBU 모델은 새 signal/slot 인 `BitstreamAnalysisWidget::packetItemModelChanged()` →
  `FileInfoWidget::setObuModel()` 로 전달한다. 파서가 사라질 때는 먼저 OBU 탭 모델을 `nullptr`
  로 지워 dangling model 을 피한다.
- `BitstreamAnalysisWidget::restartParsingOfCurrentItem()` 의 `isVisible()` early return 을 제거하고,
  `hideEvent()` 에서 파서를 삭제하지 않게 했다. 그래서 Bitstream Analysis 중앙 탭을 열지 않아도
  선택된 compressed video 의 파싱 모델이 유지되고, Info pane 의 OBU 탭과 중앙 packet tree 가
  같은 model 을 본다.
- 새 파서를 따로 만들지 않는다. 파일을 두 번 파싱하지 않고, 소유권은 기존처럼
  `BitstreamAnalysisWidget` 의 `parser` 가 가진다.

## E. OBU double-click → YUV frame 이동

- 해당 OBU 가 속한 **프레임으로 seek** 한다 (`PlaybackController::setCurrentFrameAndUpdate()`).
  이 함수가 내부에서 YUV view 를 redraw 하고 하단 frame slider/spinbox 값을 갱신한다
  (`PlaybackController.cpp:662-691`).
- Bitstream Analysis 탭의 OBU 하위 노드를 더블클릭해도 부모 OBU/패킷을 타고 올라가 같은
  프레임으로 이동한다 (패치 `0020`).
- Info pane 의 `OBU` 탭에서 더블클릭해도 중앙 탭을 Bitstream Analysis 로 바꾸지 않는다.
  `FileInfoWidget::obuTreeIndexActivated` 는 `BitstreamAnalysisWidget::seekToPacketTreeIndex()` 로
  직접 연결되고, 이 함수는 source/proxy model index 에서 frame index 를 찾아 playback controller 로
  넘기는 일만 한다 (패치 `0021`).

## 완료 상태

- B: AV1 syntax 항목 통합 완료 (패치 `0019`).
- D: Info pane OBU 목록 탭 + Bitstream Analysis parser model 공유 완료 (패치 `0021`).
- E: OBU 더블클릭 시 해당 YUV frame 표시 + 하단 slider/spinbox 갱신 경로 완료
  (패치 `0020`, `0021`).
- 남은 확인: 실제 GUI 에서 Info pane 의 `OBU` 탭을 열고 더블클릭하는 수동 확인은 아직 하지 않았다.
  빌드, 회귀, `FileInfoWidget` headless smoke 는 통과했다.
- 추후 개선 후보: E-1 의 frame index 계산은 현재 `streamPacketID` 기반이다. AV1 표시 순서까지
  엄밀히 맞춰야 하는 스트림이 나오면 위 ⓑ/ⓒ 방식으로 승격한다.

## 다음 세션 착수 지점

- 실제 GUI 에서 `Info` dock → `OBU` 탭 → OBU 노드 더블클릭 시 YUV view 가 해당 frame 으로
  이동하고 하단 slider/spinbox 가 같은 frame 번호로 바뀌는지 확인한다.
- 필요 시 `ParserAVFormat.cpp` 의 `YUView frame index` 를 ⓑ/ⓒ 방식으로 교체한다.

## 검증

- 통계 타입 목록은 헤드리스로 찍어서 확인한다 (기존 프로브 패턴,
  `tests/regression/12-*` 와 세션 중 쓴 `probe-mvdraw.cpp` 참고).
- 오버레이 렌더는 오버레이를 끈 렌더와 **차분**해서 확인한다. 테스트 패턴 자체에 순수
  빨강/파랑이 있어 색 픽셀을 그냥 세면 아무것도 증명하지 못한다 (커밋 `953ebeb` 참고).
- E-1 은 `./scripts/build.sh` 통과, 임시 헤드리스 프로브로 `test.ivf` 파싱 후
  첫 OBU 패킷에 `YUView frame index=0` 이 들어가는 것을 확인했다.
- corrected D/E 는 `0021-obu-info-tab-frame-seek.patch` 를 `0020` 까지 적용한 임시 worktree 에서
  `git apply --check` / apply / 바이트 동일 재현 / 재적용 거부로 확인했다.
- corrected `0021` 에는 `setCurrentWidget(ui.bitstreamAnalysis)` / `scrollTo()` / packet tree 선택
  helper 가 남아 있지 않다.
- 전체 패치 시리즈 `0001`..`0021` 은 깨끗한 upstream worktree 에 순서대로 적용됨을 확인했다.
- 최종 `./scripts/build.sh` 통과, `./tests/run-regression.sh` 결과 `PASS 12 FAIL 0 SKIP 0`.
- 추가 headless smoke: `FileInfoWidget` 이 `File`/`OBU` 탭을 만들고, 모델 설정/해제와
  OBU tree double-click signal forwarding 이 동작함을 확인했다.

---

# 인수인계 (다른 에이전트가 이어받을 때)

이 절은 이 리포지토리를 처음 보는 사람이 위 작업을 이어받는 데 필요한 것만 모았다.
위 본문은 "무엇을/왜", 이 절은 "어떻게".

## 이 프로젝트의 구조 — 먼저 이해할 것

**소스를 직접 고치고 커밋하는 리포지토리가 아니다.**

- 실제 코드는 git submodule `third_party/yuview/upstream` 에 있다 (upstream YUView,
  **`a72eb348` 에 고정**).
- 우리 변경은 전부 `third_party/yuview/patches/NNNN-*.patch` 로 보관한다 (현재 **21개**).
- `scripts/build.sh` 가 빌드 전에 이 패치들을 순서대로 `git apply` 한다 (이미 적용돼 있으면 건너뜀).
- 따라서 **submodule 포인터는 절대 커밋하지 않는다.** `git status` 에 항상 뜨는
  `m third_party/yuview/upstream` 은 정상이다 (빌드가 패치를 적용해 놓은 상태).

작업 흐름은 이렇다:

1. `third_party/yuview/upstream/...` 안의 소스를 직접 수정한다
2. 빌드/검증한다
3. **수정분을 새 패치로 뽑는다** (아래 레시피)
4. 패치 파일만 커밋한다

## 빌드 / 테스트 / 배포 — 전부 리포지토리 루트에서

```bash
./scripts/build.sh            # 패치 적용 + qmake -r + make. gcc-toolset-13, Qt 6.5.3
./tests/run-regression.sh     # 회귀 12개. 테스트 데이터는 없으면 ffmpeg 로 생성
./scripts/make-bin-bundle.sh  # bin/ 배포 번들 재생성 (Qt/FFmpeg/X11 동봉, 약 89MB)
```

`cd` 로 하위 디렉터리에 들어간 뒤 `./scripts/...` 를 부르면 조용히 실패한다. 항상 루트에서.

## 패치 뽑는 레시피 (그대로 복붙 가능)

깨끗한 worktree 에 기존 패치를 전부 적용한 뒤, 수정한 파일만 덮어쓰고 diff 를 뜬다.

```bash
set -e
cd /home/knight2/project/bd_analyzer
SUB=$PWD/third_party/yuview/upstream
BASE=/tmp/patchbase
rm -rf "$BASE"; git -C "$SUB" worktree prune
git -C "$SUB" worktree add --detach "$BASE" HEAD >/dev/null 2>&1
for p in third_party/yuview/patches/00*.patch; do git -C "$BASE" apply "$(realpath $p)"; done
git -C "$BASE" add -A >/dev/null
git -C "$BASE" -c user.email=t@t -c user.name=t commit -qm base

FILES="YUViewLib/src/... (수정한 파일들)"
for f in $FILES; do mkdir -p "$BASE/$(dirname $f)"; cp "$SUB/$f" "$BASE/$f"; done
# 새 파일을 추가했다면: git -C "$BASE" add -A -N
git -C "$BASE" diff > third_party/yuview/patches/00NN-<이름>.patch

# 검증 3종 — 반드시 통과시킬 것
git -C "$BASE" checkout -- .
git -C "$BASE" apply --check third_party/yuview/patches/00NN-<이름>.patch   # 깨끗이 적용되는가
git -C "$BASE" apply       third_party/yuview/patches/00NN-<이름>.patch
for f in $FILES; do cmp -s "$SUB/$f" "$BASE/$f" || echo "DIFFERS $f"; done  # 바이트 동일 재현
git -C "$BASE" apply --check third_party/yuview/patches/00NN-<이름>.patch 2>/dev/null \
  && echo "IDEMPOTENCY-BROKEN" || echo IDEMPOTENT                          # 재적용은 거부돼야 함
git -C "$SUB" worktree remove --force "$BASE"; git -C "$SUB" worktree prune
```

**함정**: 새 파일을 `git add -A -N` 로 넣었으면 `git checkout -- .` 이 그 파일을 지우지 못한다.
검증은 반드시 **새 worktree** 에서 하거나 `git clean` 까지 해야 한다 (이 실수를 한 번 했다).

## 헤드리스 검증 패턴 — GUI 자동화보다 이쪽을 쓸 것

작은 `.cpp` 를 `libYUViewLib.a` 에 링크해 프로덕션 클래스를 직접 구동한다.
`tests/regression/*.cpp` 가 전부 이 방식이다.

```bash
QT=$HOME/Qt/6.5.3/gcc_64
SRC=$PWD/third_party/yuview/upstream/YUViewLib/src
LIB=$PWD/build/YUViewLib
scl enable gcc-toolset-13 -- bash -c "
g++ -std=gnu++2a -O1 -fPIC -I'$SRC' -I'$LIB' \
  -I'$QT/include' -I'$QT/include/QtCore' -I'$QT/include/QtGui' -I'$QT/include/QtWidgets' \
  -I'$QT/include/QtXml' -I'$QT/include/QtConcurrent' -I'$QT/include/QtNetwork' \
  -I'$QT/include/QtOpenGL' probe.cpp -o /tmp/probe -L'$LIB' -lYUViewLib \
  '$QT/lib/libQt6Widgets.so' '$QT/lib/libQt6OpenGL.so' '$QT/lib/libQt6Gui.so' \
  '$QT/lib/libQt6Xml.so' '$QT/lib/libQt6Concurrent.so' '$QT/lib/libQt6Network.so' \
  '$QT/lib/libQt6Core.so' -lpthread -lGL -static-libstdc++ -static-libgcc"

# 반드시 build/YUViewApp 에서 실행한다: ffmpeg/dav1d 를 applicationDirPath() 기준으로 dlopen 한다
cd build/YUViewApp && QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH=$QT/lib /tmp/probe test.ivf
```

**함정 (이 세션에서 실제로 당한 것들)**

- 프로브는 **정적 라이브러리를 링크**한다. `./scripts/build.sh` 를 다시 돌렸으면
  **프로브도 다시 컴파일해야 한다.** 안 그러면 예전 코드로 측정하고 "변경이 반영 안 됐다" 고
  오판한다. 한 번 이것 때문에 잘못된 결론을 냈다.
- 통계 타입은 `_exit()` 로 끝내라. dlopen 한 라이브러리 때문에 전역 소멸자에서 깨진다
  (기존 테스트들이 전부 `_exit` 를 쓰는 이유).
- 테스트용 스트림: `build/YUViewApp/test.ivf` (176x144 AV1),
  `test_176x144_yuv420p.yuv`, `big.ivf`(640x360), `sb128.ivf`(128 superblock).

## 오버레이 렌더 검증은 반드시 "차분" 으로

테스트 패턴(testsrc2)에 **순수 빨강·파랑 막대가 들어 있다.** 색 픽셀을 그냥 세면
아무것도 증명하지 못한다. 오버레이를 끈 렌더를 baseline 으로 두고 **달라진 픽셀만** 세라.
(커밋 `953ebeb` 에서 이 방식으로 MV 색을 확정했다.)

## GUI 로 확인해야 할 때

- 먼저 `~/.config/Institut*/YUView.conf` 에서 `Autosaveplaylist` 줄을 지운다.
  안 지우면 "Restore Playlist" 모달이 떠서 자동화가 막힌다 (`pkill` 로 죽이면 크래시로 인식됨).
- `xdotool` 클릭은 **창 상대 좌표**를 써라: `xdotool mousemove --window $W x y click 1`.
  화면 절대 좌표는 창 장식 오프셋 때문에 빗나간다.
- Qt 는 `xdotool key --window` 로 보낸 **합성 키 이벤트를 무시한다.** 키 입력은 신뢰하지 말 것.
- 창 찾기: 이름이 `YUView - <파일>` 이고, 크기가 `3x3`/`1x1`/`10x10` 인 것들은 더미다.

## 지금 남은 작업

본문 "완료 상태" 와 "다음 세션 착수 지점" 참조. 요약하면:

1. 실제 GUI 에서 Info pane OBU 탭 더블클릭 시 YUV frame 과 하단 slider/spinbox 가 이동하는지
   수동 확인한다.
2. 필요하면 E-1 의 frame index 계산을 ⓑ/ⓒ 로 승격한다.

## 코드 규약 (이 리포지토리)

- 코드 주석과 커밋 메시지는 **영어**로 쓴다 (사용자 지시). 문서/대화는 한국어.
- 요청받은 것만 고친다. 주변 코드 정리·리팩터링을 끼워 넣지 않는다 (패치가 커지면
  재적용과 리뷰가 어려워진다).
- 수치나 결론을 보고하기 전에 실제 도구 출력과 대조한다. 확인 못 한 것은 **미검증**이라고 밝힌다.
