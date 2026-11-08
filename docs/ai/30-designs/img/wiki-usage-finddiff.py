#!/usr/bin/env python3.11
"""Figure for the Confluence page "Find diff 사용법" (bd-analyzer 0.5.4).

The comparison pipeline of src/integration/StreamDiffWindow.cpp (steps 1-2, 3, 4, D), where each
result appears (the "Find diff" dock tree, the Problems dock) and how to jump to a difference.
Usage: python3.11 wiki-usage-finddiff.py <out.png>
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]

BLUE, GREEN, ORANGE, GREY, RED, PURPLE = ("#2b6cb0", "#2f855a", "#c05621", "#4a5568", "#c53030",
                                          "#6b46c1")


def box(ax, x, y, w, h, title, lines=(), color=BLUE, fill="#ffffff", size=8.8, dash=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.01,rounding_size=0.08",
                                fc=fill, ec=color, lw=1.8, ls="--" if dash else "-"))
    ax.text(x + 0.12, y + h - 0.12, title, ha="left", va="top", fontsize=10.5,
            fontweight="bold", color=color)
    for i, line in enumerate(lines):
        ax.text(x + 0.15, y + h - 0.5 - i * 0.31, line, ha="left", va="top", fontsize=size,
                color="#1a202c")


def arrow(ax, x0, y0, x1, y1, text="", color=GREY, dx=0.0, dy=0.1, ls="-"):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=15,
                                 lw=1.8, color=color, linestyle=ls))
    if text:
        ax.text((x0 + x1) / 2 + dx, (y0 + y1) / 2 + dy, text, ha="center", va="bottom",
                fontsize=8.8, color=color)


def main(out):
    fig, ax = plt.subplots(figsize=(16, 10))
    ax.set_xlim(0, 16)
    ax.set_ylim(0, 10)
    ax.axis("off")
    ax.text(0.2, 9.85, "Find diff 비교 흐름과 결과 위치 (bd-analyzer 0.5.4)", fontsize=15,
            fontweight="bold", va="top")

    box(ax, 0.2, 7.75, 4.4, 1.75, "① Playlist 에서 압축 스트림 2개 선택",
        ["순서: playlist 위쪽 = A, 아래쪽 = B (클릭 순서 무관)",
         "2개가 아니거나 같은 파일이면",
         "메시지 박스 \"Find diff\" 로 이유 표시"], color=BLUE)
    box(ax, 5.1, 7.75, 4.6, 1.75, "② Analysis 메뉴",
        ["Analysis > Find diff between two Streams",
         "단축키 Ctrl+J  (우클릭 메뉴에는 없음)",
         "→ 아래쪽 dock 탭 \"Find diff\" 가 열림"], color=GREEN)
    arrow(ax, 4.6, 8.6, 5.1, 8.6)

    box(ax, 10.2, 7.75, 5.6, 1.75, "dock 다시 띄우기",
        ["View > Dock Panels > Show Find diff",
         "View > Perspective > YUV compare (Alt+3)",
         "   = Find diff + Problems 를 함께 표시"], color=ORANGE, fill="#fffaf0")

    # pipeline
    ax.text(0.2, 7.35, "③ 백그라운드에서 차례로 실행 (헤더가 달라도 묻지 않고 계속)", fontsize=11,
            fontweight="bold", color=PURPLE, va="top")
    steps = [
        ("steps 1-2  Header syntax", ["sequence header,", "OBU header + frame header", "값 다름 / only in A·B"]),
        ("step 3  OBU payload", ["OBU payload 를 바이트로 비교", "첫 차이 OBU 와 byte 위치", ".av1 / .ivf"]),
        ("step 4  Blocks, frame N", ["step 3 이 지목한 프레임만 디코딩", "SB(row, col) · MI(row, col)", "첫 차이 SB 에서 멈춤"]),
        ("D  Reconstructed pictures", ["체크박스를 켤 때만 (기본 꺼짐)", "두 스트림 전체 디코딩", "프레임별 PSNR, 첫 차이 픽셀"]),
    ]
    for i, (title, lines) in enumerate(steps):
        x = 0.2 + i * 3.95
        box(ax, x, 5.0, 3.6, 2.0, title, lines, color=PURPLE, dash=(i == 3))
        if i < 3:
            arrow(ax, x + 3.6, 6.0, x + 3.95, 6.0, color=PURPLE)

    # dock tree
    box(ax, 0.2, 0.3, 7.7, 4.3, "④ \"Find diff\" dock (위에서 아래로)",
        ["요약 label: A / B 파일 이름, 단계별 한 줄 결론",
         "Block info: (•)A ( )B    Syntax info: (•)A ( )B    [Go to first difference]",
         "트리 (Where | Difference | A | B), 최상위 4개:",
         "   Header syntax (steps 1-2)",
         "   OBU payload (step 3)",
         "   Blocks, frame N (step 4)  ← 자동으로 펼쳐짐",
         "   Reconstructed pictures (D)",
         "[ ] Compare CDF (entropy coder state)  — 비활성, 이유는 tooltip",
         "[ ] Compare reconstructed pictures (decodes both whole streams)",
         "[Close]  — dock 을 close 해도 진행 중 비교가 취소됨"], color=BLUE, size=8.6)
    arrow(ax, 4.0, 5.0, 4.0, 4.6, color=PURPLE)

    box(ax, 8.3, 2.75, 3.7, 1.85, "Problems dock (Ctrl+Shift+P)",
        ["차이가 있으면 자동으로 열림",
         "SB 마다 1행: SB(row, col)",
         "recon 프레임마다 1행"], color=RED, fill="#fff5f5")
    arrow(ax, 12.3, 5.0, 11.6, 4.6, color=RED)
    ax.text(11.45, 4.68, "비교가 끝나면", ha="right", va="bottom", fontsize=8.8, color=RED)

    # jump
    box(ax, 12.4, 0.3, 3.4, 4.3, "⑤ 그 지점으로 이동",
        ["메인 창이 바뀌는 것:",
         "· playlist: Block info 쪽이",
         "  첫 선택(왼쪽 view)",
         "· 그 프레임으로 이동",
         "· 블록 선택 (클릭과 동일)",
         "· Block Info dock 열림",
         "· Bitstream Analysis 는",
         "  Syntax info 쪽 스트림",
         "자동 이동은 하지 않음"], color=GREEN, size=8.4)
    box(ax, 8.3, 0.3, 3.7, 2.15, "이동하는 법",
        ["dock 트리 행 더블클릭",
         "[Go to first difference]",
         "Problems 행 더블클릭",
         "A/B 토글 변경 = 같은 지점 재표시"], color=GREEN, fill="#f0fff4")
    arrow(ax, 7.9, 1.4, 8.3, 1.4, color=GREEN)
    arrow(ax, 12.0, 1.4, 12.4, 1.4, color=GREEN)
    arrow(ax, 10.15, 2.75, 10.15, 2.45, color=GREEN)

    fig.savefig(out, dpi=110, bbox_inches="tight")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "wiki-usage-finddiff.png")
