#!/usr/bin/env python3.11
"""Figure for the Confluence page "SB / Frame / Sequence BD-rate 사용법" (bd-analyzer 0.5.4).

Where the feature lives: playlist selection -> Analysis menu (Ctrl+R) -> the "SB BD-rate" bottom
dock, with the controls of the dock labelled as in src/integration/BdRatePlotWindow.cpp.
Usage: python3.11 wiki-usage-bdrate.py <out.png>
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]

BLUE, GREEN, ORANGE, GREY, RED = "#2b6cb0", "#2f855a", "#c05621", "#4a5568", "#c53030"


def box(ax, x, y, w, h, title, lines=(), color=BLUE, fill="#ffffff", title_size=11, size=9):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.01,rounding_size=0.08",
                                fc=fill, ec=color, lw=1.8))
    ax.text(x + 0.12, y + h - 0.12, title, ha="left", va="top", fontsize=title_size,
            fontweight="bold", color=color)
    for i, line in enumerate(lines):
        ax.text(x + 0.15, y + h - 0.52 - i * 0.33, line, ha="left", va="top", fontsize=size,
                color="#1a202c")


def arrow(ax, x0, y0, x1, y1, text="", color=GREY, dx=0.0, dy=0.12):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=16,
                                 lw=1.8, color=color))
    if text:
        ax.text((x0 + x1) / 2 + dx, (y0 + y1) / 2 + dy, text, ha="center", va="bottom",
                fontsize=9, color=color)


def main(out):
    fig, ax = plt.subplots(figsize=(15, 9.4))
    ax.set_xlim(0, 15)
    ax.set_ylim(0, 9.4)
    ax.axis("off")
    ax.text(0.2, 9.2, "SB BD-rate 사용 흐름 (bd-analyzer 0.5.4)", fontsize=15,
            fontweight="bold", va="top")

    # 1. selection
    box(ax, 0.2, 5.6, 4.3, 3.1, "① Playlist 에서 선택",
        ["곡선 1개 = 같은 설정, QP/CRF 만 다른",
         "   압축 스트림 2개 이상 (표준 4점)",
         "+ 원본 YUV/Y4M 정확히 1개",
         "또는 각 스트림에 [Load Org YUV]",
         "   (Playlist 패널의 버튼)",
         "AV1 + dav1d analyzer 디코더만"], color=BLUE)

    # 2. menu
    box(ax, 5.0, 6.6, 4.6, 2.1, "② Analysis 메뉴",
        ["Analysis > SB BD-rate from Selection",
         "단축키 Ctrl+R",
         "다시 누를 때마다 group(곡선) 1개 추가"], color=GREEN)
    arrow(ax, 4.5, 7.6, 5.0, 7.6)

    # rejection
    box(ax, 10.1, 6.9, 4.7, 1.8, "조건이 안 맞으면",
        ["메시지 박스 \"SB BD-rate\" 에 이유 표시",
         "(group 이 이미 있으면 dock 은 그대로 열림)"], color=RED, fill="#fff5f5")
    arrow(ax, 9.6, 7.8, 10.1, 7.8, color=RED)

    # re-open
    box(ax, 10.1, 4.9, 4.7, 1.7, "dock 다시 띄우기",
        ["View > Dock Panels > Show SB BD-rate",
         "View > Perspective > RD / BD-rate (Alt+4)"], color=ORANGE, fill="#fffaf0")

    # 3. dock
    dx, dy, dw, dh = 0.2, 0.25, 9.6, 5.0
    ax.add_patch(FancyBboxPatch((dx, dy), dw, dh, boxstyle="round,pad=0.01,rounding_size=0.08",
                                fc="#f7fafc", ec=BLUE, lw=2.2))
    ax.text(dx + 0.15, dy + dh - 0.12,
            "③ 아래쪽 dock 탭 \"SB BD-rate\"  (RD Plot · Problems 와 같은 탭 묶음)",
            fontsize=11, fontweight="bold", color=BLUE, va="top")
    arrow(ax, 7.3, 6.6, 7.3, 5.3, "dock 이 열리고 맨 앞으로", color=GREEN, dx=1.3, dy=-0.25)

    # checkbox row
    ax.add_patch(FancyBboxPatch((0.45, 4.25), 9.1, 0.5, boxstyle="round,pad=0.01", fc="#ffffff",
                                ec=GREY, lw=1))
    ax.text(0.6, 4.5, "[x] SB   [x] Frame   [ ] Sequence   ▮▮▮▯▯ 진행률   [Cancel sweep]",
            fontsize=9.5, va="center")
    ax.text(9.45, 4.5, "Sequence 체크 = sweep 시작 / 해제 = 취소", fontsize=8.5, va="center",
            ha="right", color=ORANGE)

    # three panels
    for i, (name, note) in enumerate([("Superblock", "클릭한 SB, 표시 프레임"),
                                      ("Frame", "표시 프레임 전체"),
                                      ("Sequence", "모든 프레임 누적")]):
        x = 0.45 + i * 3.05
        ax.add_patch(FancyBboxPatch((x, 2.6), 2.9, 1.5, boxstyle="round,pad=0.01", fc="#ffffff",
                                    ec=GREY, lw=1))
        ax.text(x + 0.1, 3.98, name, fontsize=9.5, fontweight="bold", va="top")
        ax.text(x + 0.1, 3.65, note, fontsize=8.5, va="top", color=GREY)
        ax.text(x + 0.1, 3.3, "x = bits + 1 (log)", fontsize=8.5, va="top")
        ax.text(x + 0.1, 3.0, "y = PSNR (dB), 범례에 BD-rate %", fontsize=8.5, va="top")

    ax.text(0.5, 2.42, "상태줄: Frame N, superblock (col, row), anchor \"...\", BD-rate 가 안 나온 곡선 수",
            fontsize=8.8, va="top", color="#1a202c")
    ax.add_patch(FancyBboxPatch((0.45, 1.35), 9.1, 0.65, boxstyle="round,pad=0.01", fc="#ffffff",
                                ec=GREY, lw=1))
    ax.text(0.6, 1.67, "group table:  Anchor (radio)  |  Group (이름 편집)  |  Points  |  Original",
            fontsize=9.2, va="center")
    ax.add_patch(FancyBboxPatch((0.45, 0.4), 9.1, 0.75, boxstyle="round,pad=0.01", fc="#ffffff",
                                ec=GREY, lw=1))
    ax.text(0.6, 0.93, "value table:  Group | Stream | Scope | bits | PSNR (dB)", fontsize=9.2,
            va="center")
    ax.text(0.6, 0.6, "Scope = frame / SB (c,r) / seq A-B / seq SB (c,r)   ← SB 는 (열, 행) 순서",
            fontsize=8.8, va="center", color=RED)

    # main view click
    box(ax, 10.1, 2.4, 4.7, 2.2, "④ 메인 view 에서 SB 클릭",
        ["Superblock 패널이 그 SB 로 갱신",
         "메인 창 view 의 클릭만 반영",
         "(Separate View 창의 클릭은 무시)",
         "프레임 이동 → SB·Frame 패널 갱신"], color=GREEN)
    arrow(ax, 10.1, 3.4, 9.55, 3.4, color=GREEN)

    box(ax, 10.1, 0.25, 4.7, 1.9, "배치 처리 (개발 빌드 전용)",
        ["sb-bdrate-export <org.y4m> <out.csv>",
         "   --a <anchor...> --b <test...>",
         "SB 당 CSV 1행, bin/ 번들에는 없음"], color=GREY, fill="#edf2f7", size=8.6)

    fig.savefig(out, dpi=110, bbox_inches="tight")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "wiki-usage-bdrate.png")
