#!/usr/bin/env python3.11
"""Figures for the Confluence page "ME 분석 사용법" (bd-analyzer 0.5.4).

  wiki-usage-me-layout.png    where the ME panel and its companion docks live, with menu paths
  wiki-usage-me-controls.png  panel rows shown per algorithm (defaults) and the overlay colours

Facts come from MotionEstimationWidget.cpp, Mainwindow.cpp (menus, docks), Perspectives.cpp,
MeStatisticsAdapter.cpp and OverlayPalette.h at HEAD dee1ea0.
"""
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]
OUT = os.path.dirname(os.path.abspath(__file__))

# OverlayPalette.h
ME_COLORS = [
    ("SVT-AV1 integer ME", "ME SVT integer", (255, 140, 0), "SAD"),
    ("Odyssey open-loop ME", "ME odyssey open-loop", (255, 215, 0), "SSE + rate^2"),
    ("Odyssey closed-loop ME", "ME odyssey closed-loop", (160, 255, 0), "sub-pel SAD"),
    ("Odyssey open-loop ME adv (GF + TMV)", "ME odyssey open-loop adv", (255, 128, 128), "SSE + rate^2"),
]


def box(ax, x, y, w, h, text, fc="#eef2f7", ec="#5b6b80", fs=10, weight="normal", ha="center"):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.08",
                                fc=fc, ec=ec, lw=1.2))
    tx = x + w / 2 if ha == "center" else x + 0.15
    ax.text(tx, y + h / 2, text, ha=ha, va="center", fontsize=fs, weight=weight)


def layout_figure():
    fig, ax = plt.subplots(figsize=(13, 7.4))
    ax.set_xlim(0, 13)
    ax.set_ylim(0, 7.2)
    ax.axis("off")
    ax.set_title("ME 패널과 관련 dock 위치 (bd-analyzer 0.5.4)", fontsize=14, weight="bold", loc="left")

    # Main window frame
    ax.add_patch(Rectangle((0.2, 0.3), 8.6, 6.3, fc="white", ec="#333", lw=1.5))
    ax.text(0.35, 6.4, "File  View  Analysis  Tools  Playback  Help", fontsize=10, va="center")
    box(ax, 0.35, 5.75, 8.3, 0.4, "overlay toolbar: overlay 묶음마다 버튼 하나, 앞 8개 = F1-F8", fc="#fff7e0", fs=9)
    # left column
    box(ax, 0.35, 3.95, 1.9, 1.65, "Playlist\n\n[Load Org YUV]", fs=9)
    box(ax, 0.35, 2.35, 1.9, 1.5, "Block Info\n(Ctrl+B)\nME 한 줄 요약", fs=9)
    # picture
    box(ax, 2.4, 2.35, 4.0, 3.25, "그림\n\nMV 화살표 (알고리즘별 색)\n범례: Ctrl+Shift+L\n\nShift+클릭 = 블록 비교",
        fc="#f4f4f4", fs=9)
    # right column
    box(ax, 6.55, 4.3, 2.1, 1.3, "Properties\n[Video] [Overlays]\nME 체크박스는\nOverlays 탭", fs=8.5)
    box(ax, 6.55, 2.35, 2.1, 1.85, "Motion Estimation\n(Ctrl+M)\n\nAlgorithm\nBlock sizes ...", fc="#ffe9d6",
        ec="#d9822b", fs=9, weight="bold")
    # bottom tabs
    box(ax, 0.35, 1.05, 8.3, 1.15, "", fc="#eef7ee", ec="#4f8a4f")
    ax.text(0.5, 1.95, "하단 dock 탭 (RD Plot 과 같은 묶음)", fontsize=9, va="center")
    for i, t in enumerate(["ME block compare", "Global motion", "RD Plot", "..."]):
        box(ax, 0.5 + i * 2.05, 1.15, 1.9, 0.55, t, fc="white", ec="#4f8a4f", fs=9)
    box(ax, 0.35, 0.4, 8.3, 0.5, "Playback controls", fs=9)

    # Menu paths on the right
    x0 = 9.1
    ax.text(x0, 6.85, "여는 방법", fontsize=12, weight="bold", va="center")
    items = [
        ("ME 패널", "View > Dock Panels >\nShow Motion Estimation  (Ctrl+M)", "#ffe9d6"),
        ("한 번에", "View > Perspective > ME / GM  (Alt+5)\nME 패널 + ME block compare + Global motion", "#eef2f7"),
        ("블록 비교", "그림에서 Shift+클릭\n또는 View > Dock Panels > Show ME block compare", "#eef7ee"),
        ("Global motion", "Analysis > Global motion of the\nselected Item (Ctrl+Shift+G), 탭에서 Run", "#eef7ee"),
        ("범례", "View > Show Overlay Legend (Ctrl+Shift+L)", "#fff7e0"),
    ]
    y = 5.6
    for title, text, fc in items:
        box(ax, x0, y, 3.8, 0.95, "", fc=fc)
        ax.text(x0 + 0.12, y + 0.72, title, fontsize=9.5, weight="bold", va="center")
        ax.text(x0 + 0.12, y + 0.33, text, fontsize=8.3, va="center")
        y -= 1.1
    fig.tight_layout()
    path = os.path.join(OUT, "wiki-usage-me-layout.png")
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return path


def controls_figure():
    fig, ax = plt.subplots(figsize=(13, 8.4))
    ax.set_xlim(0, 13)
    ax.set_ylim(-0.4, 8.2)
    ax.axis("off")
    ax.set_title("Motion Estimation 패널: 알고리즘별로 보이는 행과 기본값, overlay 색", fontsize=14, weight="bold",
                 loc="left")

    cols = ["행 (라벨)", "기본값", "SVT", "open-loop", "closed-loop", "open-loop adv"]
    xs = [0.2, 3.0, 6.4, 7.6, 8.9, 10.4]
    rows = [
        ("Compute in the background", "끔 (켜야 계산)", "o", "o", "o", "o"),
        ("Frame interval", "1  (-60..60, 0 거부)", "o", "o", "o", "회색"),
        ("Algorithm", "SVT-AV1 integer ME", "o", "o", "o", "o"),
        ("GF interval", "16  (2/4/8/16)", "", "", "", "o"),
        ("TMV grid", "16x16 blocks", "", "", "", "o"),
        ("Show", "Nearest reference", "", "", "", "o"),
        ("Start MV from", "None (0, 0)", "", "", "o", ""),
        ("Model", "RotZoom", "", "", "Global model 일 때", ""),
        ("Sub-pel", "1/8 pel allowed", "", "", "o", ""),
        ("Block sizes", "64 만 체크", "o", "o", "o", "o"),
        ("SVT static block bypass", "켬 (SVT 용)", "o", "o", "o", "o"),
    ]
    y = 7.45
    for x, c in zip(xs, cols):
        ax.text(x, y, c, fontsize=10, weight="bold", va="center")
    ax.plot([0.15, 12.0], [y - 0.25, y - 0.25], color="#333", lw=1)
    y -= 0.5
    for r in rows:
        for i, (x, c) in enumerate(zip(xs, r)):
            if i >= 2 and c == "o":
                ax.add_patch(Rectangle((x + 0.2, y - 0.13), 0.26, 0.26, fc="#4f8a4f", ec="none"))
            else:
                ax.text(x, y, c, fontsize=9, va="center", color="#666" if i >= 2 else "black")
        y -= 0.42
    ax.text(0.2, y + 0.05, "초록 칸 = 그 알고리즘에서 보이는 행.  Algorithm 콤보 항목: SVT-AV1 integer ME / Odyssey open-loop ME / "
            "Odyssey closed-loop ME / Odyssey open-loop ME adv (GF + TMV)", fontsize=8.5, va="center", color="#444")

    # Overlay colours
    y0 = 1.45
    ax.text(0.2, y0 + 0.35, "overlay 색과 묶음 이름 (Properties > Overlays 탭, toolbar 버튼과 같음)", fontsize=11,
            weight="bold", va="center")
    for i, (combo, base, rgb, native) in enumerate(ME_COLORS):
        x = 0.2 + (i % 2) * 6.4
        yy = y0 - 0.45 - (i // 2) * 0.75
        ax.add_patch(Rectangle((x, yy - 0.2), 0.45, 0.4, fc=tuple(c / 255 for c in rgb), ec="#333"))
        ax.text(x + 0.6, yy + 0.1, f"{base} - MV / - {native} / - common SAD", fontsize=8.8, va="center")
        ax.text(x + 0.6, yy - 0.17, f"RGB {rgb}", fontsize=8, va="center", color="#555")
    fig.tight_layout()
    path = os.path.join(OUT, "wiki-usage-me-controls.png")
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return path


if __name__ == "__main__":
    for p in (layout_figure(), controls_figure()):
        print(p)
