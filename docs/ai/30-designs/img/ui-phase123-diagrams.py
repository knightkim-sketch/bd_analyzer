# Draws the figures for the Confluence page "bd-analyzer UI 개편 Phase 0-3 결과와 자체 승인 이력":
#   ui-phase-layout.png      the main window after patches 0084-0088 (docks, toolbar, timeline, tabs)
#   ui-phase-selection.png   micro architecture: click -> SelectionContext -> panels (signal names)
#   ui-phase-perspective.png flowchart: View > Perspective -> saved blob or code definition
#   ui-phase-timeline.png    micro architecture: where the timeline's bars and types come from
#   ui-phase-results.png     per phase: patch, new regression test, regression PASS/FAIL
#   ui-phase-decisions.png   the self-approved decisions by phase and weight, with who was consulted
# Sources: patches 0084-0088, src/integration (Perspectives, ToolDock, PropertyTabs, OverlayControls,
# FrameTimeline, ProblemsWidget, SelectionContext, SessionFile), regression logs of 2026-10-31/11-01,
# docs/ai/30-designs/ui-redesign-decisions.md.
# Run: python3.11 ui-phase123-diagrams.py   (writes next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

HERE = os.path.dirname(os.path.abspath(__file__))
plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]

INK = "#172b4d"
OK = "#e3fcef"
WARN = "#fffae6"
BAD = "#ffebe6"
GREY = "#f4f5f7"
BLUE = "#deebff"
PURPLE = "#eae6ff"


def new(w, h):
    fig, ax = plt.subplots(figsize=(w, h))
    ax.set_xlim(0, w)
    ax.set_ylim(0, h)
    ax.axis("off")
    return fig, ax


def box(ax, x, y, w, h, title, body="", fc=GREY, dashed=False, ts=9.5, bs=7.8):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.03", fc=fc, ec=INK, lw=1.0,
                                ls="--" if dashed else "-"))
    ax.text(x + 0.1, y + h - 0.1, title, ha="left", va="top", fontsize=ts, weight="bold", color=INK)
    if body:
        ax.text(x + 0.1, y + h - 0.42, body, ha="left", va="top", fontsize=bs, color=INK, linespacing=1.4)


def arrow(ax, x0, y0, x1, y1, label="", color=INK, ls="-"):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=13, lw=1.2,
                                 color=color, ls=ls))
    if label:
        ax.text((x0 + x1) / 2, (y0 + y1) / 2 + 0.12, label, ha="center", va="bottom", fontsize=7.2,
                color=color, bbox=dict(fc="white", ec="none", pad=0.5))


def save(fig, name):
    fig.savefig(os.path.join(HERE, name), dpi=130, bbox_inches="tight")
    plt.close(fig)


# ---- 1. layout ------------------------------------------------------------------------------
fig, ax = new(16, 10.6)
ax.text(8, 10.4, "개편 후: 기본 dock 6개 + 위 timeline·toolbar, 분석 도구는 아래 탭, 작업별 perspective 5개",
        ha="center", fontsize=13, weight="bold", color=INK)
box(ax, 0.2, 9.35, 15.6, 0.6, "메뉴  File · View · Analysis(0084) · Tools(0084) · Playback · Help",
    "File: Open/Save Session (.bdsession, 0088)   View: Perspective Alt+1–5 (0087) · Dock Panels · Show Overlay Toolbar · Show Overlay Legend Ctrl+Shift+L (0086)",
    fc=BLUE, bs=7.4)
box(ax, 0.2, 8.6, 15.6, 0.6, "Overlay toolbar (0086)  objectName overlayToolBar",
    "현재 item 의 overlay 묶음(uiGroup)마다 버튼 + 색 칸, F1–F8, Properties 체크박스와 양방향 (StatisticUIHandler::setOverlayGroupRendered)",
    fc=OK, bs=7.4)
box(ax, 0.2, 7.55, 15.6, 0.9, "Frame Timeline dock (0088)  frameTimelineDock  Ctrl+Shift+T",
    "막대 = 프레임(높이 bytes, 색 KEY 빨강 · INTRA_ONLY 주황 · INTER 파랑 · SWITCH 보라 · B 초록) · 분홍선 = 현재 프레임\n"
    "클릭 = 이동 · Shift+드래그 = 구간 합계(bytes, kbps, max/min, 타입별, B 와의 차이 %) · 우클릭 Log Scale",
    fc=OK, bs=7.4)
box(ax, 0.2, 2.2, 3.4, 5.15, "왼쪽 열", "", fc=GREY)
box(ax, 0.35, 5.6, 3.1, 1.35, "Playlist  Ctrl+L", "playlistDockWidget", fc="white")
box(ax, 0.35, 4.05, 3.1, 1.4, "Info  Ctrl+I", "fileInfoDock\nFile / Frame / OBU 탭", fc="white")
box(ax, 0.35, 2.35, 3.1, 1.55, "Block Info  Ctrl+B (0084)", "cachingInfoDock\n블록 syntax 표 1개\n(중복 dock 삭제)", fc="white")
box(ax, 3.8, 3.95, 8.1, 3.4, "중앙: 그림 (splitViewWidget)", "", fc="white")
box(ax, 9.0, 6.25, 2.75, 0.95, "범례 (0086)", "켜진 overlay 만\n\"fixed 0..255\" / \"frame a..b\"", fc=WARN, bs=7.2)
ax.text(4.0, 5.3, "overlay = 디코더 통계 + ME/GM\n색: OverlayPalette.h (D-08/08a)\n글꼴: 앱 글꼴 (helvetica 제거)",
        fontsize=7.8, color=INK, va="top")
box(ax, 3.8, 2.2, 8.1, 1.6, "Hex dump (그림 아래, Knight 지정으로 위치 유지)", "hexdumpViewer  클릭한 블록의 비트 범위 강조", fc=GREY)
box(ax, 12.1, 2.2, 3.7, 5.15, "오른쪽 열", "", fc=GREY)
box(ax, 12.25, 4.25, 3.4, 2.7, "Properties  Ctrl+P",
    "propertiesDock\n탭 2개 (0087, PropertyTabs)\n  Video: 형식·변환·필터·디코더\n  Overlays: 통계 체크박스\n마지막 탭 기억", fc="white")
box(ax, 12.25, 2.35, 3.4, 1.75, "Frame Info  Ctrl+Shift+F", "frameInfoDock\n(Ctrl+F 는 Fullscreen, 0084)", fc="white")
box(ax, 0.2, 0.85, 15.6, 1.2, "아래: 탭 묶음 (기본 숨김, perspective 가 열어 줌) — ToolDock (0087)", "", fc=PURPLE)
tabs = ["RD Plot\nCtrl+Shift+R", "SB BD-rate\nbdRateDock", "Find diff\nstreamDiffDock", "Global motion\nglobalMotionDock",
        "ME compare\nmeCompareDock", "Y4M generator\nsequenceGeneratorDock", "Problems (0088)\nproblemsDock"]
for i, t in enumerate(tabs):
    box(ax, 0.35 + i * 2.21, 0.95, 2.1, 0.75, t.split("\n")[0], t.split("\n")[1], fc="white", ts=7.8, bs=6.8)
box(ax, 0.2, 0.05, 15.6, 0.65, "재생 컨트롤  playbackControllerDock  Ctrl+D",
    "▶  ■  |◀ step back  ▶| step forward (0085, Left/Right 와 같은 슬롯)  ─── 프레임 slider ───  fps  repeat", fc=OK, bs=7.4)
save(fig, "ui-phase-layout.png")

# ---- 2. selection micro architecture --------------------------------------------------------
fig, ax = new(15, 7.2)
ax.text(7.5, 7.0, "블록 선택: view 2개 × 패널 5개 = 연결 10개 → SelectionContext 하나로 (0088, D-07/07a)",
        ha="center", fontsize=12.5, weight="bold", color=INK)
box(ax, 0.2, 4.7, 3.0, 1.4, "displaySplitView", "splitViewWidget\nblockSelected(item, pixelPos, frameIdx)", fc=BLUE)
box(ax, 0.2, 2.6, 3.0, 1.4, "separateView", "SeparateWindow.splitView\nblockSelected(...)", fc=BLUE)
box(ax, 0.2, 0.5, 3.0, 1.4, "Find diff / Problems", "Jump (미래의 패널발 이동)\nsetBlock(..., Source::Jump)", fc=GREY, dashed=True)
box(ax, 4.7, 2.0, 4.0, 3.9, "SelectionContext (src/integration)",
    "setBlock(item, pixel, frame, source)\n\n"
    "• QPointer<playlistItem> — item 삭제 시 clear()\n"
    "• view 클릭: 같은 블록이어도 항상 전달\n   (hex dump 창을 다시 여는 동작 유지)\n"
    "• Jump: 같은 값이면 무시 (루프 방지)\n"
    "• forget(): playlist 선택 변경 시 조용히 초기화\n"
    "• source(): PrimaryView / SeparateView / Jump\n\n"
    "signal blockChanged(item, pixel, frame)", fc=OK, bs=7.6)
arrow(ax, 3.2, 5.4, 4.7, 4.6, "PrimaryView")
arrow(ax, 3.2, 3.3, 4.7, 3.6, "SeparateView")
arrow(ax, 3.2, 1.2, 4.7, 2.4, "Jump", ls="--")
panels = [("BlockInfoWidget::setSelectedBlock", "Block Info 표", 5.6),
          ("FrameInfoWidget::setSelectedBlock", "블록 픽셀 통계", 4.5),
          ("HexdumpViewerWidget::setSelectedBlock", "+ revealItemDumpPane()", 3.4),
          ("AssistPanel (PrimaryView 만)", "pushAssistStream / setSelectedPosition", 2.3),
          ("BdRatePlotWindow (PrimaryView 만)", "setSelectedPosition", 1.2)]
for title, body, y in panels:
    box(ax, 10.3, y - 0.1, 4.5, 0.95, title, body, fc="white", ts=8.2, bs=7.2)
    arrow(ax, 8.7, 3.95, 10.3, y + 0.35)
ax.text(9.5, 0.3, "upstream 위젯은 SelectionContext 를 모른다 — MainWindow 가 배선 (upstream 헤더가 integration 을 include 하지 않음)",
        ha="center", fontsize=7.6, color=INK, style="italic")
save(fig, "ui-phase-selection.png")

# ---- 3. perspective flowchart ---------------------------------------------------------------
fig, ax = new(14, 7.4)
ax.text(7, 7.2, "Perspective 적용 순서 (0087, D-05/D-14): 코드 정의가 기준, 사용자 배치는 schema 번호와 함께",
        ha="center", fontsize=12.5, weight="bold", color=INK)
box(ax, 0.3, 5.6, 3.2, 1.0, "View > Perspective", "Alt+1 Default · Alt+2 Bitstream debug\nAlt+3 YUV compare · Alt+4 RD/BD-rate · Alt+5 ME/GM", fc=BLUE, bs=6.9)
box(ax, 4.4, 5.6, 4.0, 1.0, "QSettings Perspectives/<name>/state ?", "사용자가 \"Save Layout to This Perspective\" 로 저장한 blob", fc=WARN, bs=7.2)
box(ax, 9.4, 5.6, 4.3, 1.0, "restoreState(blob, kSchema=2)", "true → 그 배치 그대로\nfalse (dock 이름·schema 변경) → 아래로", fc=OK, bs=7.2)
box(ax, 4.4, 3.4, 4.0, 1.4, "applyDefinition(name)", "1) placeDocksByDefault() — MainWindow\n2) shown 목록 외 dock 숨김 (재생 컨트롤 제외)\n3) front 목록 raise()\n4) 아래 탭 묶음 높이 320 px", fc=OK, bs=7.2)
box(ax, 9.4, 3.4, 4.3, 1.4, "markCurrent(name)", "메뉴 체크 표시\nQSettings Perspectives/current\n(값이 바뀔 때만 씀 — D-26)", fc=GREY, bs=7.2)
box(ax, 0.3, 0.5, 13.4, 2.3, "perspective 별 보이는 dock (frameTimelineDock 은 모두 포함)", "", fc="white")
rows = [("Default", "Playlist · Info · Block Info · Properties · Frame Info"),
        ("Bitstream debug", "Playlist · Info · Block Info · Properties · Problems"),
        ("YUV compare", "Playlist · Block Info · Properties · Frame Info · Find diff(앞) · Problems"),
        ("RD / BD-rate", "Playlist · Block Info · Properties · RD Plot · SB BD-rate(앞)"),
        ("ME / GM", "Playlist · Block Info · Properties · Motion Estimation(앞) · ME compare · Global motion(앞)")]
for i, (n, d) in enumerate(rows):
    ax.text(0.5, 2.3 - i * 0.36, n, fontsize=8, weight="bold", color=INK, va="center")
    ax.text(2.6, 2.3 - i * 0.36, d, fontsize=7.8, color=INK, va="center")
arrow(ax, 3.5, 6.1, 4.4, 6.1)
arrow(ax, 8.4, 6.1, 9.4, 6.1, "있음")
arrow(ax, 6.4, 5.6, 6.4, 4.8, "없음")
arrow(ax, 11.5, 5.6, 8.4, 4.4, "false", color="#de350b")
arrow(ax, 8.4, 4.1, 9.4, 4.1)
arrow(ax, 11.5, 5.6, 11.5, 4.8, "true")
save(fig, "ui-phase-perspective.png")

# ---- 4. timeline data sources ---------------------------------------------------------------
fig, ax = new(15, 6.6)
ax.text(7.5, 6.4, "Frame Timeline 의 데이터: 디코드 없이, 파일을 열 때 이미 읽은 것만 쓴다 (0088, D-18)",
        ha="center", fontsize=12.5, weight="bold", color=INK)
box(ax, 0.2, 3.9, 4.2, 1.9, "컨테이너 (ivf / obu / mp4)", "FileSourceFFmpegFile::scanBitstream()\n열 때 전체 packet 순회 (기존)\n+ packetIndex {pts, bytes, keyframe}\n   버리던 값을 저장 (추가 I/O 0)", fc=BLUE, bs=7.4)
box(ax, 0.2, 1.4, 4.2, 1.9, "Annex-B (.h264 / .hevc)", "ParserAnnexB (열 때 전체 파싱, 기존)\ngetFrameStartEndPosDisplayOrder(i)\nbytes = end − start (end = 다음 NAL 시작)", fc=BLUE, bs=7.4)
box(ax, 5.2, 2.6, 4.2, 2.2, "playlistItem::getCodedFrames()", "playlistItemCompressedVideo 구현\n컨테이너: pts 순 정렬 (pts 없으면 파일 순)\nraw YUV: 빈 목록 → 같은 높이 막대\n프레임 수 ≠ startEndRange → 크기 버림", fc=OK, bs=7.4)
box(ax, 5.2, 0.3, 4.2, 1.8, "BitstreamAnalysisWidget 모델", "packetItemModelChanged(model)\nFrameObuIndex / FrameAvcIndex\n(첫 item, 기본 500 프레임까지)", fc=GREY, bs=7.4)
box(ax, 10.3, 1.4, 4.5, 3.9, "FrameTimeline (src/integration)", "Bar {bytes, type}\n막대 높이 = bytes (선형 / log)\n색 = 처음엔 key/비key 2색\n   → 모델이 채워지는 대로 1초마다 갱신\n   AV1 KEY/INTER/INTRA_ONLY/SWITCH,\n   show_existing 얇은 막대\n   H.264 I/P/B\n두 item 선택 시 B 를 윤곽선으로\nseekRequested(frame) → PlaybackController", fc=OK, bs=7.4)
arrow(ax, 4.4, 4.85, 5.2, 4.1)
arrow(ax, 4.4, 2.35, 5.2, 3.2)
arrow(ax, 9.4, 3.7, 10.3, 3.7, "크기")
arrow(ax, 9.4, 1.2, 10.3, 2.0, "타입")
ax.text(7.5, -0.05, "검증: test.ivf 25 프레임의 막대 합 + 32 + 12×25 = 파일 크기 18933 B (회귀 66)",
        ha="center", fontsize=8, color=INK)
save(fig, "ui-phase-timeline.png")

# ---- 5. results per phase -------------------------------------------------------------------
phases = ["Phase 0\n0084", "step 버튼\n0085", "Phase 1\n0086", "Phase 2\n0087", "Phase 3\n0088"]
# The last full regression run of each step, as measured (TIMEOUT=300, load average 15-20).
passed = [84, 84, 83, 85, 87]
failed = [1, 1, 3, 2, 1]
notes = ["", "", "26: 색 조건 수정\n→ 단독 PASS\n51: 부하 타임아웃\n→ 단독 68 s PASS",
         "25: tool dock 을\nDock Panels 에 추가\n→ 단독 PASS", ""]
fig, ax = plt.subplots(figsize=(11, 4.6))
xs = range(len(phases))
p = [v or 0 for v in passed]
fl = [v or 0 for v in failed]
ax.bar(xs, p, color="#36b37e", label="PASS")
ax.bar(xs, fl, bottom=p, color="#de350b", label="FAIL (gm-estimate 는 개편 전부터 실패)")
for x, a, b in zip(xs, p, fl):
    ax.text(x, a + b + 0.8, f"{a} / {b}", ha="center", fontsize=9, color=INK)
tests = ["63 layout", "63 + step", "64 overlay", "65 perspective", "66 timeline"]
for x, t in zip(xs, notes):
    if t:
        ax.text(x, 45, t, ha="center", fontsize=7.5, color="white", va="bottom", weight="bold")
for x, t in zip(xs, tests):
    ax.text(x, 5, "신규 " + t, ha="center", fontsize=8, color="white", rotation=90, va="bottom")
ax.set_xticks(list(xs), phases)
ax.set_ylim(0, 120)
ax.set_ylabel("회귀 테스트 수")
ax.set_title("마지막 전체 회귀 PASS 87 / FAIL 1 — 남은 실패는 개편 전부터 있던 gm-estimate", fontsize=11.5, color=INK)
ax.legend(loc="upper left", fontsize=8, ncol=2, frameon=False)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
fig.savefig(os.path.join(HERE, "ui-phase-results.png"), dpi=130, bbox_inches="tight")
plt.close(fig)

# ---- 6. decisions ---------------------------------------------------------------------------
rows = [("D-01..03", "추가", "하", ""), ("D-04", "전체", "중", ""), ("D-05", "2", "상", "Progy · architect"),
        ("D-06", "2", "상", "architect"), ("D-07", "3", "상", "architect"), ("D-08", "1", "중", ""),
        ("D-08a", "1", "중", "회귀 26"), ("D-09..12", "1", "중", ""), ("D-13", "1", "하", ""),
        ("D-13a", "1", "중", "cpp-reviewer"), ("D-14", "2", "중", "Progy · architect"), ("D-15", "2", "중", "architect"),
        ("D-16", "2", "중", ""), ("D-17", "2", "하", ""), ("D-18", "3", "상", "Progy · Cody"),
        ("D-19..21", "3", "중", "Cody · Progy"), ("D-22", "3", "하", "Cody"), ("D-23", "3", "중", "Cody · Progy"),
        ("D-07a", "3", "중", "cpp-reviewer"), ("D-24", "3", "중", "cpp-reviewer"), ("D-25..26", "3", "하", "")]
fig, ax = new(12, 7.8)
ax.text(6, 7.75, "자체 승인 30건: 무게 '상' 4건(D-05·06·07·18)은 모두 전문가 agent 의견을 받아 결정", ha="center",
        fontsize=12.5, weight="bold", color=INK)
col = {"상": "#de350b", "중": "#ff991f", "하": "#97a0af"}
for i, (d, ph, w, who) in enumerate(rows):
    y = 7.1 - i * 0.33
    ax.add_patch(FancyBboxPatch((0.3, y - 0.13), 0.5, 0.26, boxstyle="round,pad=0.02", fc=col[w], ec="none"))
    ax.text(0.55, y, w, ha="center", va="center", fontsize=7.5, color="white", weight="bold")
    ax.text(1.0, y, d, va="center", fontsize=8, color=INK, weight="bold")
    ax.text(2.6, y, "Phase " + ph if ph not in ("추가", "전체") else ph, va="center", fontsize=8, color=INK)
    ax.text(4.2, y, who or "—", va="center", fontsize=8, color=INK)
ax.text(1.0, 7.4, "결정", fontsize=8, color="#6b778c")
ax.text(2.6, 7.4, "단계", fontsize=8, color="#6b778c")
ax.text(4.2, 7.4, "의견 수렴 / 계기", fontsize=8, color="#6b778c")
box(ax, 7.3, 3.6, 4.4, 3.6, "무게 기준", "상: 되돌리기 비쌈 · 새 의존성 · 구조 변경\n     → 전문가 agent 의견 필수\n중: 사용자에게 보이는 동작 변경\n하: 구현 세부\n\n전문가: Progy(구현), Cody(코덱),\narchitect(구조), cpp-reviewer(검토)\n\n전체 표: docs/ai/30-designs/\nui-redesign-decisions.md", fc=GREY, bs=7.6)
save(fig, "ui-phase-decisions.png")
print("done")
