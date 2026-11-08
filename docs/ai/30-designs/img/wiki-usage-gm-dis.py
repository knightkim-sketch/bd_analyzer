#!/usr/bin/env python3
# Figure for the wiki page "Global motion / DIS 사용법" (bd-analyzer 0.5.4).
# Top: where the tool lives (menu -> bottom dock tab) and the dock layout.
# Bottom: data flow SVT integer ME 16x16 -> RANSAC model -> overlays / closed-loop seed / DIS.
# Usage: python3.11 wiki-usage-gm-dis.py <out.png>
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]

INK = "#1f2a44"
MENU = "#e8f0fe"
DOCK = "#f4f6f8"
CTRL = "#ffffff"
STAB = "#eaf7ee"
FLOW = "#fff7e0"
OUT = "#efe9fb"


def box(ax, x, y, w, h, title, body="", fc=CTRL, ls="-", tsize=11, bsize=9.5):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.004,rounding_size=0.006",
                                fc=fc, ec=INK, lw=1.1, ls=ls))
    ax.text(x + 0.008, y + h - 0.012, title, ha="left", va="top", fontsize=tsize,
            fontweight="bold", color=INK)
    if body:
        ax.text(x + 0.008, y + h - 0.012 - 0.026 * (tsize / 11), body, ha="left", va="top",
                fontsize=bsize, color=INK, linespacing=1.35)


def arrow(ax, x0, y0, x1, y1, text="", dashed=False):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=14,
                                 lw=1.3, color=INK, ls="--" if dashed else "-"))
    if text:
        ax.text((x0 + x1) / 2, (y0 + y1) / 2 + 0.008, text, ha="center", va="bottom",
                fontsize=8.5, color=INK)


def main(out):
    fig = plt.figure(figsize=(17, 12.5), dpi=100)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

    ax.text(0.5, 0.985, "Global motion / DIS — 어디서 열고 무엇이 어떻게 이어지는가 (bd-analyzer 0.5.4)",
            ha="center", va="top", fontsize=15, fontweight="bold", color=INK)

    # ---- Opening paths
    ax.text(0.02, 0.945, "① 여는 법", fontsize=12.5, fontweight="bold", color=INK, va="top")
    box(ax, 0.02, 0.795, 0.215, 0.13, "Analysis 메뉴",
        "Global motion of the selected Item\n단축키 Ctrl+Shift+G\n(playlist 에서 raw YUV/Y4M, 또는\n 원본 YUV 를 붙인 스트림 선택)", fc=MENU)
    box(ax, 0.02, 0.655, 0.215, 0.12, "View 메뉴",
        "Perspective → ME / GM  (Alt+5)\nDock Panels → Show Global motion\n(Motion Estimation 패널: Ctrl+M)", fc=MENU)
    box(ax, 0.02, 0.515, 0.215, 0.12, "Tools 메뉴",
        "Generate Y4M Sequence...\n  (Camera shake 로 시험 영상)\nExport Stabilized Y4M...", fc=MENU)
    arrow(ax, 0.235, 0.86, 0.265, 0.86, "")

    # ---- Dock mock-up
    ax.text(0.265, 0.945, "② 아래쪽 dock 탭 \"Global motion\"", fontsize=12.5, fontweight="bold",
            color=INK, va="top")
    ax.add_patch(FancyBboxPatch((0.265, 0.475), 0.715, 0.44, boxstyle="round,pad=0.004",
                                fc=DOCK, ec=INK, lw=1.4))
    # tab strip
    tabs = ["RD Plot", "SB BD-rate", "Find diff", "Global motion", "ME block compare", "Y4M sequence generator", "Problems"]
    tx = 0.272
    for t in tabs:
        w = 0.0062 * len(t) + 0.012
        sel = t == "Global motion"
        ax.add_patch(FancyBboxPatch((tx, 0.89), w, 0.02, boxstyle="square,pad=0",
                                    fc="#ffffff" if sel else "#dde3ea", ec=INK, lw=1.4 if sel else 0.6))
        ax.text(tx + w / 2, 0.90, t, ha="center", va="center", fontsize=8.5,
                fontweight="bold" if sel else "normal", color=INK)
        tx += w + 0.004
    box(ax, 0.275, 0.84, 0.695, 0.045, "요약 줄", "", tsize=9)
    ax.text(0.33, 0.863, "대상 파일, 해상도, 실행 결과 요약", fontsize=9, va="center", color=INK)
    box(ax, 0.275, 0.79, 0.695, 0.055, "파라미터 줄",
        "Frames [  ] to [  ]   R (px) 256   Refine ± 16   Smoothing 5-tap   Brightness DC + gain   "
        "Model rotzoom (off / translation / rotzoom / affine)", tsize=9, bsize=9)
    box(ax, 0.275, 0.745, 0.695, 0.04, "", "", tsize=9)
    ax.text(0.283, 0.765, "[Run]  [Stop]  [Export CSV...] → <이름>.gm.csv   [Export model CSV...] → <이름>.gm-model.csv   진행 상태",
            fontsize=9, va="center", color=INK)
    box(ax, 0.275, 0.665, 0.34, 0.075, "프레임 표 (행 클릭 → 그 프레임으로 이동, overlay 갱신)",
        "poc · ref · n · accepted x/y · median dx/dy (px)\nrefine moved · model · model MV at centre (px)\nscale / angle (deg) · inliers", tsize=9, bsize=8.5)
    box(ax, 0.625, 0.665, 0.345, 0.075, "window 표 (선택한 프레임의 4x4 window 16개)",
        "window (i, j) · dx/dy (px) · coarse · accept x/y\ncost best/zero · a_q · sad2d …", tsize=9, bsize=8.5)
    box(ax, 0.275, 0.485, 0.695, 0.175, "Stabilization (DIS)", "", fc=STAB, tsize=10)
    ax.text(0.285, 0.625,
            "Method   Moving average / Gaussian (기본) / Kalman causal / Kalman RTS\n"
            "  Radius (frames) 15   ← Moving average 일 때\n"
            "  Sigma (frames) 6.0   ← Gaussian 일 때\n"
            "  Kalman Q 0.002 · Kalman R 4.0   ← Kalman 일 때\n"
            "[✓] Apply to view   → 메인 화면에 안정화된 그림\n"
            "[Export stabilized...] → <이름>_stabilized.y4m\n"
            "상태: Frames a-b, <Method>. Largest correction N px",
            fontsize=8.8, va="top", color=INK, linespacing=1.4)
    box(ax, 0.72, 0.495, 0.24, 0.15, "궤적 그래프",
        "dx (px) · dy (px) · angle (deg) 띠\n회색 = 측정 궤적\n색 = 평활 궤적\n세로선 = 화면의 현재 프레임", fc=CTRL, tsize=9, bsize=8.5)

    # ---- Data flow
    ax.text(0.02, 0.44, "③ 데이터 흐름 — 모델은 SVT integer ME 16x16 벡터에만 맞춥니다", fontsize=12.5,
            fontweight="bold", color=INK, va="top")
    box(ax, 0.02, 0.27, 0.16, 0.13, "파일 (raw YUV / Y4M)",
        "Global motion 은 파일을 직접\n읽습니다. 필터 체인·\nUse as ME input 영향 없음", fc=FLOW, bsize=8.8)
    box(ax, 0.21, 0.27, 0.16, 0.13, "SVT integer ME",
        "프레임 쌍마다\n16x16 블록 벡터\n(블록 중심 → 중심 + MV)", fc=FLOW, bsize=8.8)
    box(ax, 0.40, 0.27, 0.17, 0.13, "RANSAC 전역 모델",
        "translation / rotzoom / affine\n시도 20회, inlier 1.25 px\nseed 고정 → 결과 결정적", fc=FLOW, bsize=8.8)
    arrow(ax, 0.18, 0.335, 0.21, 0.335)
    arrow(ax, 0.37, 0.335, 0.40, 0.335)
    box(ax, 0.02, 0.09, 0.16, 0.15, "window 추정 (별도)",
        "4x4 window 16개 평행이동\noverlay: GM accepted /\nGM rejected (raw) /\nGM window (accepted axes)", fc=FLOW, bsize=8.5)
    arrow(ax, 0.10, 0.27, 0.10, 0.24)

    box(ax, 0.62, 0.335, 0.36, 0.085, "overlay (Properties → Overlays 탭, 또는 overlay toolbar)",
        "GM model inlier: 초록 = 모델을 따름(1), 빨강 = 안 따름(0)\nGM model vector: 64x64 마다 모델의 MV", fc=OUT, tsize=9.5, bsize=8.8)
    box(ax, 0.62, 0.225, 0.36, 0.095, "closed-loop 시작 MV (Motion Estimation 패널)",
        "Algorithm = Odyssey closed-loop ME 일 때\nStart MV from = Global model (SVT ME)\nModel = Translation / RotZoom (기본) / Affine", fc=OUT, tsize=9.5, bsize=8.8)
    box(ax, 0.62, 0.03, 0.36, 0.18, "DIS (안정화)",
        "모델 → 누적 궤적 → 평활화 → 보정 warp\n크기 그대로, 가장자리 replicate, crop 없음\n"
        "• dock: Apply to view / Export stabilized...\n"
        "• Tools → Export Stabilized Y4M...\n   Motion model: Translation / Rotation + zoom (기본)\n   Smoothing 4가지, 2-pass 진행 창",
        fc=OUT, tsize=9.5, bsize=8.8)
    arrow(ax, 0.57, 0.36, 0.62, 0.375)
    arrow(ax, 0.57, 0.32, 0.62, 0.27)
    arrow(ax, 0.57, 0.29, 0.62, 0.15)

    box(ax, 0.21, 0.03, 0.36, 0.2, "시험 영상 (정답 궤적 있음)",
        "Tools → Generate Y4M Sequence...\nCamera shake: Off / Random walk / Sine\nAmplitude x, y px, deg · period · seed\n"
        "Generate 시 <y4m>.camera.csv 에 정답 궤적 저장\n→ 이 파일을 열고 Global motion 실행,\n   궤적 그래프·CSV 와 비교", fc=FLOW, bsize=8.8)

    fig.savefig(out, dpi=100)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "wiki-usage-gm-dis.png")
