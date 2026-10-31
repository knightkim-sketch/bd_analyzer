# Draws the four figures for the Confluence page "bd-analyzer 보완 작업 검토와 화면 구성 개선 제안":
#   ui-review-current.png   current main-window composition (docks, central panes, popups) with problem tags
#   ui-review-proposed.png  proposed layout (frame timeline, overlay toolbar + legend, perspectives)
#   ui-review-roadmap.png   Phase 0-3 order with items and touched files
#   ui-review-gaps.png      follow-up work by effect x cost
# Source: review of 2026-10-30 (patched tree a72eb348 + patches 0001-0083, src/integration),
# see docs/ai/30-designs/ui-redesign-review.md for file:line evidence.
# Run: python3.11 ui-review-diagrams.py   (writes next to this script)
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


def tag(ax, x, y, n):
    ax.add_patch(plt.Circle((x, y), 0.17, fc="#de350b", ec="white", lw=1.2, zorder=5))
    ax.text(x, y, str(n), ha="center", va="center", fontsize=8, color="white", weight="bold", zorder=6)


def arrow(ax, x0, y0, x1, y1, color=INK):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=13, lw=1.2, color=color))


def save(fig, name):
    fig.savefig(os.path.join(HERE, name), dpi=130, bbox_inches="tight")
    plt.close(fig)


# ---- 1. current -----------------------------------------------------------------------------
fig, ax = new(16, 10)
ax.text(8, 9.75, "현재: 처음 켜면 dock 6개 + 중앙 pane 3개가 모두 열리고, 분석 도구는 떠 있는 창 5개로 흩어집니다",
        ha="center", fontsize=13, weight="bold", color=INK)
# window frame
ax.add_patch(FancyBboxPatch((0.3, 0.4), 11.4, 9.0, boxstyle="round,pad=0.02", fc="white", ec=INK, lw=1.6))
ax.text(0.45, 9.2, "창 제목 \"YUView\"   |  File(17)  View(7, 4단 중첩)  Playback(5)  Help(9, IENT 링크)   |  toolbar 없음",
        fontsize=8.6, color=INK, va="center")
tag(ax, 11.4, 9.2, 4)
# left column
box(ax, 0.5, 6.4, 2.9, 2.5, "Playlist (L)", "tree · Open · Load Org YUV\nRec/Org combo", OK)
box(ax, 0.5, 3.9, 2.9, 2.4, "Info (L)", "File / Frame / OBU 탭\nH.264·HEVC 에서도 \"OBU\"", WARN)
tag(ax, 3.2, 6.1, 2)
box(ax, 0.5, 1.4, 2.9, 2.4, "Caching Info (L)", "캐시 진행 막대\n+ Block Syntax (2번째 사본)\n+ 블록 픽셀 통계", BAD)
tag(ax, 3.2, 3.6, 2)
ax.text(1.95, 1.05, "왼쪽 3개 dock 적층 → 높이 보정 hack\n(Mainwindow.cpp:608-653)", ha="center", fontsize=7.5, color="#bf2600")
# central
box(ax, 3.6, 4.6, 5.0, 4.3, "중앙 탭 \"Item View\"  (Bitstream Analysis 탭 5개는 뒤에)",
    "video view (stretch 2)\n\noverlay: MV0 red · MV1 blue · SB grid 0,112,255\n"
    "ME green = INTRA green · ME violet ≈ COMPOUND\n범례 없음, \"helvetica\" 하드코딩", BLUE)
tag(ax, 8.4, 5.0, 7)
box(ax, 3.6, 1.4, 2.45, 3.05, "Hex dump", "블록 클릭 전에도\n중앙 공간 차지\n(최대 220 px / 1/3)", WARN)
box(ax, 6.15, 1.4, 2.45, 3.05, "RD plot", "항상 표시", WARN)
tag(ax, 8.4, 4.2, 3)
# right column
box(ax, 8.8, 4.8, 2.7, 4.1, "Properties (R)", "item format\n+ Convolution filter ~10 위젯\n  (setFixedHeight 104)\n+ overlay 목록·opacity\n→ 긴 스크롤", BAD)
tag(ax, 11.3, 5.1, 6)
box(ax, 8.8, 2.4, 2.7, 2.25, "Frame Info (R)", "Analysis grid\nluma histogram\n(Info>Frame 탭과 이름 충돌)", WARN)
box(ax, 8.8, 1.4, 2.7, 0.85, "Playback (B)", "", OK)
ax.text(6.0, 0.6, "status bar 숨김 → \"Original YUV of …\" 메시지가 보이지 않음 (Mainwindow.cpp:129, 2209)",
        ha="center", fontsize=8, color="#bf2600")
tag(ax, 0.75, 0.6, 8)
# hidden docks + popups
ax.text(13.85, 9.2, "숨김 dock (View > Dock Panels)", ha="center", fontsize=10, weight="bold", color=INK)
for i, (t, b) in enumerate([("Block Info", "Caching Info 와 같은 위젯"), ("Motion Estimation", "combo 7 · checkbox 6"),
                            ("AI Assistant", "text 3 · checkbox 3"), ("YouTube Transcode", "버튼 7 · 입력 다수")]):
    box(ax, 12.0, 8.25 - i * 0.75, 3.7, 0.65, t, "", GREY, True, ts=8.5)
    ax.text(15.6, 8.25 - i * 0.75 + 0.22, b, ha="right", fontsize=7, color="#5e6c84")
ax.text(13.85, 5.0, "떠 있는 QDialog 5개 (각자 크기·스타일)", ha="center", fontsize=10, weight="bold", color=INK)
for i, (t, b) in enumerate([("SB BD-rate  Ctrl+R", "1100×720 · ~20 위젯"), ("Find diff  Ctrl+J", "1100×760 · ~27"),
                            ("Global motion  Ctrl+Shift+G", "1200×800 · ~30"), ("ME compare  Shift+클릭", "1150×700 · ~15"),
                            ("Y4M generator  (File 메뉴)", "1400×820 · ~53")]):
    box(ax, 12.0, 4.05 - i * 0.75, 3.7, 0.65, t, "", BAD, ts=8.3)
    ax.text(15.6, 4.05 - i * 0.75 + 0.2, b, ha="right", fontsize=7, color="#5e6c84")
tag(ax, 11.95, 4.7, 5)
ax.text(13.85, 0.2, "분석 도구는 View 메뉴, export·generator 는 File 메뉴\nCtrl+F 중복: Show Frame Info / Fullscreen",
        ha="center", fontsize=8, color="#bf2600")
save(fig, "ui-review-current.png")

# ---- 2. proposed ----------------------------------------------------------------------------
fig, ax = new(16, 10)
ax.text(8, 9.75, "제안: 위 frame timeline · 가운데 그림 · 좌 syntax · 우 block details · 아래 hex/오류 — 선택은 한 곳에서 모든 패널로",
        ha="center", fontsize=12.5, weight="bold", color=INK)
ax.add_patch(FancyBboxPatch((0.3, 0.4), 15.4, 9.0, boxstyle="round,pad=0.02", fc="white", ec=INK, lw=1.6))
ax.text(0.45, 9.15, "bd-analyzer  |  File  View  Analysis  Tools  Help", fontsize=9, color=INK, va="center")
for i, p in enumerate(["Bitstream debug", "YUV compare", "RD / BD-rate", "ME / GM"]):
    ax.add_patch(FancyBboxPatch((8.3 + i * 1.82, 8.98), 1.72, 0.34, boxstyle="round,pad=0.02",
                                fc="#0052cc" if i == 0 else "#ebecf0", ec=INK, lw=0.8))
    ax.text(8.3 + i * 1.82 + 0.86, 9.15, p, ha="center", va="center", fontsize=8,
            color="white" if i == 0 else INK)
ax.text(8.25, 9.15, "Perspective ▸", ha="right", va="center", fontsize=8, color="#5e6c84")
# timeline strip
box(ax, 0.5, 7.65, 15.0, 1.15, "Frame timeline (VQ Analyzer Stream View / StreamEye GOP 참조)", "", BLUE)
import random

random.seed(3)
for k in range(64):
    t = "I" if k % 16 == 0 else ("P" if k % 4 == 0 else "B")
    hgt = {"I": 0.62, "P": 0.36, "B": 0.18}[t] * (0.8 + 0.4 * random.random())
    c = {"I": "#de350b", "P": "#0052cc", "B": "#36b37e"}[t]
    ax.add_patch(plt.Rectangle((0.7 + k * 0.205, 7.72), 0.16, hgt, fc=c, ec="none"))
ax.add_patch(plt.Rectangle((0.7 + 21 * 0.205 - 0.02, 7.7), 0.2, 0.72, fc="none", ec="#ff991f", lw=2))
ax.text(14.3, 8.05, "막대 = 프레임 bit\n색 = I / P / B\n드래그 = 구간 집계", fontsize=7.3, color=INK, ha="center")
# left
box(ax, 0.5, 2.7, 3.3, 4.8, "좌: Stream / Syntax",
    "Playlist (위)\nSyntax tree (OBU / NAL / LCEVC)\n  - 선택 노드 ↔ 그림 블록\n  - 해당 없는 노드 숨김\n\nInfo (File · Frame)\n  - codec 별 탭 이름", OK)
# centre
box(ax, 4.0, 2.7, 7.6, 4.8, "중앙: Picture view (split / diff)", "", BLUE)
for i, (k, on) in enumerate([("Part", 1), ("Mode", 1), ("MV", 1), ("QP", 0), ("Bits", 0), ("ME", 0), ("GM", 0), ("Grid", 1)]):
    ax.add_patch(FancyBboxPatch((4.15 + i * 0.66, 6.7), 0.58, 0.32, boxstyle="round,pad=0.02",
                                fc="#0052cc" if on else "white", ec=INK, lw=0.8))
    ax.text(4.15 + i * 0.66 + 0.29, 6.86, k, ha="center", va="center", fontsize=7.2, color="white" if on else INK)
    ax.text(4.15 + i * 0.66 + 0.29, 6.55, f"F{i + 1}", ha="center", fontsize=6.3, color="#5e6c84")
ax.text(9.55, 6.86, "← overlay toolbar (그림 위, F1–F8)", fontsize=7.3, color=INK, va="center")
ax.add_patch(plt.Rectangle((4.3, 3.0), 4.7, 3.3, fc="#c1c7d0", ec="none"))
ax.add_patch(plt.Rectangle((6.0, 4.2), 0.9, 0.9, fc="none", ec="#ff991f", lw=2.2))
ax.text(6.45, 3.95, "선택 블록", ha="center", fontsize=7, color=INK)
box(ax, 9.2, 3.0, 2.25, 2.1, "범례 (모서리)", "", "white", ts=8)
for j, (sym, lab, c) in enumerate([("■", "INTRA", "#36b37e"), ("■", "INTER", "#0052cc"), ("→", "MV0", "#de350b"),
                                   ("→", "MV1", "#6554c0"), ("┄", "SB grid", "#5e6c84")]):
    ax.text(9.35, 4.6 - j * 0.3, sym, fontsize=8, color=c, va="center", weight="bold")
    ax.text(9.6, 4.6 - j * 0.3, lab, fontsize=7.2, color=INK, va="center")
ax.text(10.4, 3.15, "색 토큰 1벌", fontsize=6.8, color="#5e6c84")
# right
box(ax, 11.8, 2.7, 3.7, 4.8, "우: Block details",
    "MI(row, col) · 크기 · mode\nMV(x, y) · ref · QP\n예측 / 잔차 / 계수 탭\nME·GM 비교 (Shift+클릭 대체)\n\nProperties 는 탭으로 분리\n  - 표시: overlay · opacity\n  - 처리: filter chain", OK)
# bottom
box(ax, 0.5, 0.6, 7.4, 1.65, "아래: Hex (선택 syntax 바이트 강조)", "블록·노드를 고를 때만 펼침 · Ctrl+H", WARN)
box(ax, 8.1, 0.6, 7.4, 1.65, "아래: Problems / Diff 목록 (Intel VPA 참조)",
    "decode 경고 · Find diff 결과 · first mismatch\n행 클릭 → frame · block · syntax 로 이동", WARN)
# selection bus
ax.text(8.0, 2.47, "SelectionContext: frame → block → syntax node → hex 범위  (한 번 고르면 모든 패널이 따라감)",
        ha="center", fontsize=8.6, weight="bold", color="#0052cc")
save(fig, "ui-review-proposed.png")

# ---- 3. roadmap -----------------------------------------------------------------------------
fig, ax = new(16, 7.4)
ax.text(8, 7.15, "개선 순서: 패치 1~2개로 끝나는 정리부터, 구조 변경(Qt ADS)은 Phase 2 에서",
        ha="center", fontsize=13, weight="bold", color=INK)
phases = [
    ("Phase 0 — 정리 (quick win)", OK,
     "· Ctrl+F 충돌 해소\n· Block Syntax 사본 제거,\n  Caching Info 이름 정리\n· Hex · RD plot 을 dock 으로\n· status bar 복구\n· Reset Layout / Fullscreen 에\n  새 dock 5개 반영\n· Analysis · Tools 메뉴 신설\n· 창 제목 bd-analyzer",
     "Mainwindow.cpp, mainwindow.ui,\nSplitViewWidget.cpp\n패치 1~2개"),
    ("Phase 1 — 시각 체계", BLUE,
     "· overlay 색 토큰 1벌\n  (충돌 4쌍 해소)\n· 그림 모서리 범례\n· overlay toggle toolbar\n  + F1–F8\n· 아이콘 세트\n· Fusion + QPalette 테마\n· 문구 규칙 (콜론·… )",
     "StatisticsDataPainting.cpp,\nMe/GmStatisticsAdapter.cpp,\nassets/icons"),
    ("Phase 2 — 작업 공간", WARN,
     "· Qt ADS 도입 (LGPL-2.1)\n· Perspective 4종 기본 배포\n· 떠 있는 창 5개를 dock 화\n· Properties 를\n  표시 / 처리 탭으로 분리\n· 설정 dialog 에\n  흩어진 QSettings 모음",
     "Mainwindow.cpp 배선 분리,\nBdRatePlotWindow 등 5개,\nYUViewLib.pro"),
    ("Phase 3 — 연동", BAD,
     "· Frame timeline 띠\n· SelectionContext\n  (frame→block→syntax→hex)\n· Problems / Diff 목록\n· heatmap 범위 모드\n· 구간 선택 집계\n· .bdsession 세션 파일",
     "신규 src/integration 위젯,\nparser tree · hexdump 구독"),
]
for i, (t, fc, body, files) in enumerate(phases):
    x = 0.4 + i * 3.95
    box(ax, x, 3.2, 3.6, 3.5, t, body, fc, ts=10, bs=8.3)
    ax.text(x + 0.1, 3.0, files, va="top", fontsize=7.3, color="#5e6c84", linespacing=1.35)
    if i < 3:
        arrow(ax, x + 3.62, 4.95, x + 3.93, 4.95)
ax.set_ylim(1.5, 7.4)
fig.set_size_inches(16, 5.9)
ax.text(8, 1.75, "각 Phase 끝: run-regression.sh 전체 + feature-verification.md §2 수동 항목(창·단축키·overlay) 확인",
        ha="center", fontsize=8.6, color=INK)
save(fig, "ui-review-roadmap.png")

# ---- 4. gaps --------------------------------------------------------------------------------
fig, ax = plt.subplots(figsize=(13, 8))
items = [  # (label, cost 1-5, effect 1-5, priority)
    ("LCEVC 가속 commit + 0.5.3", 1.0, 3.6, "H"),
    ("a.out · YT 메모 정리", 0.6, 1.4, "H"),
    ("사용자 매뉴얼 docs/user", 2.6, 4.2, "H"),
    ("검증 문서 test 38–62 반영", 1.4, 3.2, "H"),
    ("회귀 skip 경고 / CI", 1.9, 4.5, "H"),
    ("패치 83개 정리 · upstream rebase", 4.6, 4.0, "H"),
    ("프레임 PSNR/SSIM plot+CSV", 2.9, 3.8, "M"),
    ("Scopes (histogram/waveform)", 3.3, 2.8, "M"),
    ("BD-rate CSV · heatmap", 2.1, 3.05, "M"),
    ("CLI 회귀 + golden", 2.6, 3.3, "M"),
    ("plugin SDK 또는 규칙 4 삭제", 3.6, 1.9, "M"),
    ("TASK-0004 결정", 0.9, 1.9, "M"),
    ("generator GT MV 출력", 2.0, 2.3, "L"),
    ("chroma 필터 · 상태 저장", 3.0, 1.75, "L"),
    ("LCEVC suffix NAL 페어링", 2.2, 1.5, "L"),
    ("3rd-party 라이선스 표기", 0.9, 2.4, "L"),
    ("빈 scaffold 디렉토리 정리", 0.6, 0.9, "L"),
]
col = {"H": "#de350b", "M": "#ff991f", "L": "#6b778c"}
for lab, c, e, p in items:
    ax.scatter(c, e, s=140, color=col[p], zorder=3, edgecolor="white", lw=1)
    ax.annotate(lab, (c, e), xytext=(7, 4), textcoords="offset points", fontsize=8.6, color=INK)
ax.axvline(2.5, color="#c1c7d0", lw=1, ls="--")
ax.axhline(2.6, color="#c1c7d0", lw=1, ls="--")
ax.text(0.15, 4.85, "먼저 할 것 (싸고 효과 큼)", fontsize=10, weight="bold", color="#006644")
ax.text(5.95, 4.85, "계획 필요 (비싸고 효과 큼)", fontsize=10, weight="bold", color="#bf2600", ha="right")
ax.set_xlim(0, 6.0)
ax.set_ylim(0.5, 5.0)
ax.set_xlabel("비용 (상대, 1 ≈ 반나절 · 5 ≈ 수 주)  — 추정")
ax.set_ylabel("효과 (상대) — 추정")
ax.set_title("보완 작업: 왼쪽 위의 정리·검증 항목부터, 패치 정리는 별도 계획으로", fontsize=12.5, weight="bold", color=INK)
for p, n in [("H", "High"), ("M", "Medium"), ("L", "Low")]:
    ax.scatter([], [], color=col[p], s=90, label=n)
ax.legend(loc="lower right", frameon=False)
ax.spines[["top", "right"]].set_visible(False)
save(fig, "ui-review-gaps.png")
print("wrote 4 figures to", HERE)
