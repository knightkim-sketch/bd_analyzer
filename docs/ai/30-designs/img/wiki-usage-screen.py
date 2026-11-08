#!/usr/bin/env python3.11
"""Figures for the wiki page "화면 구성과 메뉴·단축키" (bd-analyzer 0.5.4).

Sources (third_party/yuview/upstream/YUViewLib with patches applied, and src/integration):
  menus / shortcuts   src/ui/Mainwindow.cpp  createMenusAndActions (File 778-836, View 838-975,
                      Analysis 978-995, Tools 997-1005, Playback 1007-1033, Help 1035-1088)
  default placement   src/ui/Mainwindow.cpp  placeDocksByDefault (2372-)
  perspectives        src/integration/Perspectives.cpp  definitions()

Regenerate:  python3.11 docs/ai/30-designs/img/wiki-usage-screen.py <outdir>
"""
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]
OUT = sys.argv[1] if len(sys.argv) > 1 else "."


def box(ax, x, y, w, h, title, body="", fc="#F2F2F2", tfs=10, bfs=8.3, ls="-"):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.01,rounding_size=0.06",
                                fc=fc, ec="#555", lw=1.1, ls=ls))
    ax.text(x + 0.12, y + h - 0.12, title, ha="left", va="top", fontsize=tfs, fontweight="bold")
    if body:
        ax.text(x + 0.12, y + h - 0.42, body, ha="left", va="top", fontsize=bfs, color="#333",
                linespacing=1.35)


# ---------------------------------------------------------------------------
# 1. Default layout (Help > Reset Window Layout / perspective Default, Alt+1)
# ---------------------------------------------------------------------------
def fig_layout():
    fig, ax = plt.subplots(figsize=(14, 8.4), dpi=130)
    ax.set_xlim(0, 14); ax.set_ylim(0, 8.4); ax.axis("off")
    box(ax, 0.1, 7.75, 13.8, 0.5, "메뉴:  File · View · Analysis · Tools · Playback · Help", fc="#E8E8E8", tfs=9.5)
    box(ax, 0.1, 7.15, 13.8, 0.5, "Overlay toolbar  —  overlay 묶음 버튼, 앞 8개에 F1–F8  (View → Show Overlay Toolbar)",
        fc="#FFF3D6", tfs=9.5)
    box(ax, 0.1, 6.2, 13.8, 0.85, "Frame Timeline  (Ctrl+Shift+T, 위쪽)",
        "프레임(TU)별 크기 막대 · 타입 색 · 클릭 = 이동 · Shift+드래그 = 구간 집계 · 두 item 선택 시 B 를 윤곽선으로 겹침",
        fc="#E3F1DE")
    # left column
    box(ax, 0.1, 4.55, 3.2, 1.55, "Playlist  (Ctrl+L)", "Open · Load Org YUV\nRec / Org 표시 선택")
    box(ax, 0.1, 2.95, 3.2, 1.5, "Info  (Ctrl+I)", "탭: File / Frame / OBU\nFrame = coding_order·order_hint")
    box(ax, 0.1, 1.35, 3.2, 1.5, "Block Info  (Ctrl+B)", "클릭한 블록의 통계·syntax\n(예전 이름 Caching Info)")
    # centre
    box(ax, 3.4, 3.3, 7.0, 2.8, "중앙 탭: Item View  |  Bitstream Analysis",
        "그림 (split view)\n· 클릭 = 블록 선택 → Block Info · hex dump 가 따라감\n· Shift+클릭 = ME block compare 탭\n"
        "· 오른쪽 위 범례 (Ctrl+Shift+L)\n· Fullscreen Ctrl+F", fc="#FFFFFF")
    box(ax, 3.4, 1.35, 7.0, 1.85, "아래 dock 탭 묶음 (처음엔 숨김)",
        "RD Plot (Ctrl+Shift+R) · SB BD-rate · Find diff · Global motion ·\nME block compare · Y4M sequence generator · "
        "Problems (Ctrl+Shift+P)\n→ Analysis / Tools 메뉴나 perspective 로 열림", fc="#DDE8F5")
    # right column
    box(ax, 10.5, 3.6, 3.4, 2.5, "Properties  (Ctrl+P)",
        "탭 Video:\n  형식 · 필터 체인 · Decoder 콤보\n탭 Overlays:\n  통계 overlay 체크박스\n  (ME · GM 포함)")
    box(ax, 10.5, 1.35, 3.4, 2.15, "Frame Info  (Ctrl+Shift+F)",
        "현재 프레임 정보\n\n숨김 시작 (오른쪽):\nMotion Estimation (Ctrl+M)\nAI Assistant (Ctrl+K)\nYouTube Transcode (Ctrl+Y)",
        bfs=8.0)
    box(ax, 0.1, 0.35, 13.8, 0.85, "Playback  (Ctrl+D)",
        "Play/Pause Space · ◀ step back / step forward ▶ (Left / Right) · Stop · 이전/다음 item Up / Down", fc="#F2F2F2")
    ax.set_title("bd-analyzer 0.5.4 기본 배치 — 왼쪽은 스트림·블록, 오른쪽은 표시 방법, 아래는 분석 도구 탭",
                 fontsize=13, fontweight="bold")
    fig.tight_layout()
    fig.savefig(f"{OUT}/wiki-usage-layout.png", bbox_inches="tight")
    plt.close(fig)


# ---------------------------------------------------------------------------
# 2. Menu map (only the entries bd-analyzer added or moved, plus common ones)
# ---------------------------------------------------------------------------
MENUS = [
    ("File", ["Open File...  Ctrl+O", "Open LCEVC (base + enhancement)...", "Recent Files",
              "Add Text Frame / Difference Sequence / Overlay", "Delete Item  Del",
              "Save Playlist...  Ctrl+S", "Open Session... / Save Session...  (.bdsession)",
              "Save Playlist on Exit  (기본 켬)", "Save Screenshot...", "Settings...  (Decoders 탭 등)"]),
    ("View", ["Save / Restore View State  Ctrl+1–8", "Perspective ▸  Alt+1–5",
              "Dock Panels ▸  (각 dock 보이기, 단축키는 그림 1)", "Show Playback Controls  Ctrl+D",
              "Show Overlay Toolbar", "Show Overlay Legend  Ctrl+Shift+L", "Split View ▸", "Zoom ▸  1:1 Ctrl+0, Fit Ctrl+9"]),
    ("Analysis", ["SB BD-rate from Selection  Ctrl+R", "Find diff between two Streams  Ctrl+J",
                  "Global motion of the selected Item\n  Ctrl+Shift+G"]),
    ("Tools", ["Generate Y4M Sequence...", "Export Filtered Y4M...", "Export Stabilized Y4M..."]),
    ("Playback", ["Play/Pause  Space", "Next / Previous Frame\n  Right / Left",
                  "Next / Previous\n  Playlist Item  Down / Up"]),
    ("Help", ["About YUView / Help", "Downloads ▸", "Reset Window Layout", "Clear Settings"]),
]
MOVED = {"SB BD-rate from Selection  Ctrl+R": "← 예전 View", "Find diff between two Streams  Ctrl+J": "← 예전 View",
         "Global motion of the selected Item\n  Ctrl+Shift+G": "← 예전 View",
         "Generate Y4M Sequence...": "← 예전 File", "Export Filtered Y4M...": "← 예전 File",
         "Export Stabilized Y4M...": "← 예전 File"}


def fig_menus():
    fig, ax = plt.subplots(figsize=(14, 6.6), dpi=130)
    ax.set_xlim(0, 14.15); ax.set_ylim(0.5, 6.6); ax.axis("off")
    xs = [0.1, 3.0, 6.05, 8.75, 10.75, 12.55]
    ws = [2.8, 2.95, 2.6, 1.9, 1.7, 1.45]
    for (name, items), x, w in zip(MENUS, xs, ws):
        hl = name in ("Analysis", "Tools")
        ax.add_patch(FancyBboxPatch((x, 5.85), w, 0.45, boxstyle="round,pad=0.01", fc="#4C78A8" if hl else "#888",
                                    ec="none"))
        ax.text(x + w / 2, 6.07, name, ha="center", va="center", color="white", fontsize=11, fontweight="bold")
        for k, it in enumerate(items):
            y = 5.5 - k * 0.52
            ax.text(x + 0.05, y, it, fontsize=7.6, va="center", wrap=True)
            if it in MOVED:
                ax.text(x + 0.05, y - 0.27 - (0.12 if "\n" in it else 0), MOVED[it] + " 메뉴 (0.5.3 에서 이동)", fontsize=7, color="#C0392B", va="center")
    ax.set_title("메뉴 지도 — 분석 도구는 Analysis, 새 파일을 만드는 기능은 Tools 에 있습니다", fontsize=13,
                 fontweight="bold")
    fig.tight_layout()
    fig.savefig(f"{OUT}/wiki-usage-menus.png", bbox_inches="tight")
    plt.close(fig)


# ---------------------------------------------------------------------------
# 3. Perspectives: which docks each one shows
# ---------------------------------------------------------------------------
DOCKS = ["Frame Timeline", "Playlist", "Info", "Block Info", "Properties", "Frame Info", "Problems",
         "Find diff", "RD Plot", "SB BD-rate", "Motion Estimation", "ME block compare", "Global motion"]
PERS = [
    ("Default  Alt+1", ["Frame Timeline", "Playlist", "Info", "Block Info", "Properties", "Frame Info"], []),
    ("Bitstream debug  Alt+2", ["Frame Timeline", "Playlist", "Info", "Block Info", "Properties", "Problems"], []),
    ("YUV compare  Alt+3", ["Frame Timeline", "Playlist", "Block Info", "Properties", "Frame Info", "Find diff",
                            "Problems"], ["Find diff"]),
    ("RD / BD-rate  Alt+4", ["Frame Timeline", "Playlist", "Block Info", "Properties", "RD Plot", "SB BD-rate"],
     ["SB BD-rate"]),
    ("ME / GM  Alt+5", ["Frame Timeline", "Playlist", "Block Info", "Properties", "Motion Estimation",
                        "ME block compare", "Global motion"], ["Motion Estimation", "Global motion"]),
]


def fig_perspectives():
    fig, ax = plt.subplots(figsize=(14, 4.6), dpi=130)
    ax.set_xlim(-3.2, len(DOCKS)); ax.set_ylim(-0.6, len(PERS) + 1.4); ax.axis("off")
    for j, d in enumerate(DOCKS):
        ax.text(j + 0.5, len(PERS) + 0.15, d, rotation=35, ha="left", va="bottom", fontsize=8.6)
    for i, (name, shown, front) in enumerate(PERS):
        y = len(PERS) - 1 - i
        ax.text(-0.1, y + 0.45, name, ha="right", va="center", fontsize=9.5, fontweight="bold")
        for j, d in enumerate(DOCKS):
            on = d in shown
            fc = "#4C78A8" if d in front else ("#9ECAE9" if on else "#F2F2F2")
            ax.add_patch(FancyBboxPatch((j + 0.06, y + 0.08), 0.88, 0.78, boxstyle="round,pad=0.0,rounding_size=0.08",
                                        fc=fc, ec="white"))
            if d in front:
                ax.text(j + 0.5, y + 0.47, "앞", ha="center", va="center", color="white", fontsize=8, fontweight="bold")
    ax.text(-3.1, -0.45, "진한 파랑 = 열리고 탭 맨 앞 · 연한 파랑 = 열림 · 회색 = 숨김.  Playback 은 항상 열림.  "
            "View → Perspective → Save Layout to This Perspective 로 내 배치를 저장, Reset This Perspective 로 되돌림",
            fontsize=8.3, color="#444")
    ax.set_title("Perspective (View → Perspective, Alt+1–5) — 작업마다 필요한 dock 만 엽니다", fontsize=13,
                 fontweight="bold")
    fig.tight_layout()
    fig.savefig(f"{OUT}/wiki-usage-perspectives.png", bbox_inches="tight")
    plt.close(fig)


if __name__ == "__main__":
    fig_layout(); fig_menus(); fig_perspectives()
    print("wrote", OUT)
