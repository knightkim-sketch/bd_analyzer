# Figure for the Confluence usage page "Y4M Sequence Generator (Generate Y4M Sequence) 사용법":
# menu -> bottom dock tab -> controls -> outputs, bd-analyzer 0.5.4.
# Run: python3.11 wiki-usage-y4m.py  (writes wiki-usage-y4m.png next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, Rectangle

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wiki-usage-y4m.png")

fig, ax = plt.subplots(figsize=(14, 9.4))
ax.set_xlim(0, 14)
ax.set_ylim(0, 9.4)
ax.axis("off")


def box(x, y, w, h, title, lines, fc, size=10, ec="#555555"):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.04,rounding_size=0.1", fc=fc, ec=ec, lw=1.3))
    ax.text(x + 0.12, y + h - 0.15, title, ha="left", va="top", fontsize=11, fontweight="bold")
    for i, line in enumerate(lines):
        ax.text(x + 0.12, y + h - 0.52 - i * 0.31, line, ha="left", va="top", fontsize=size)


def arrow(x1, y1, x2, y2):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="-|>", mutation_scale=16, lw=1.5, color="#333333"))


ax.text(7, 9.3, "Y4M sequence generator: 여는 곳과 화면 구성 (bd-analyzer 0.5.4)", ha="center", va="top",
        fontsize=15, fontweight="bold")

# Step 1: menu
box(0.2, 7.55, 3.9, 1.25, "1. 메뉴", ["Tools → Generate Y4M Sequence...", "(0.5.2 이전: File 메뉴)"], "#e8f5e9", size=10.5)
# Step 2: dock tab
box(4.6, 7.55, 9.2, 1.25, "2. 아래쪽 dock 탭 \"Y4M sequence generator\" 로 열림 (0.5.4)",
    ["같은 탭 묶음: RD Plot · SB BD-rate · Find diff · Global motion · ME block compare · Problems",
     "다시 보이기: View → Dock Panels → Show Y4M sequence generator   |   제목줄을 끌면 떼어낼 수 있음 (미검증)"],
    "#e3f2fd", size=9.8)
arrow(4.1, 8.17, 4.6, 8.17)

# Dock contents frame
ax.add_patch(Rectangle((0.2, 1.75), 13.6, 5.5, fc="#fafafa", ec="#333333", lw=1.5))
ax.text(0.35, 7.1, "3. dock 안의 화면", ha="left", va="center", fontsize=11, fontweight="bold")

box(0.4, 3.0, 5.6, 3.8, "캔버스 (현재 프레임 미리보기)",
    ["빈 곳 드래그 = 선택한 도형 그리기",
     "도형 드래그 = 이동, 노란 모서리 = 크기",
     "선택한 물체: 점선 상자 + MV 화살표",
     "",
     "",
     "",
     "아래 상태 줄: 안내 / \"Wrote … and opened",
     "it in the playlist.\""], "#ffffff", size=9.8)
box(0.4, 1.95, 5.6, 0.9, "Play | 프레임 슬라이더 | frame N / M | Grid □ [8–128]",
    ["Grid 는 미리보기 전용 (y4m 에 안 씀), 기본 꺼짐·64"], "#ffffff", size=9.5)

box(6.2, 4.5, 3.7, 2.55, "Sequence",
    ["Size (even) 16–7680 x 16–4320",
     "Frames · fps · Background",
     "Flip: horizontal / vertical",
     "Camera shake: Off / Random walk / Sine",
     "Amplitude x, y px, deg",
     "period · seed"], "#fff8e1", size=9.4)
box(6.2, 1.95, 3.7, 2.4, "Layers (top of the list is drawn last)",
    ["[Rectangle|Circle|Triangle|Star]",
     "Add · Image... · Paste",
     "레이어 목록 (체크 = 보이기)",
     "Up · Down · Delete"], "#fff8e1", size=9.6)
box(10.1, 5.05, 3.55, 2.0, "Object (frame 0)",
    ["Position x, y · Size w, h",
     "Rotation (deg)",
     "Colour · Alpha 0–255"], "#f3e5f5", size=9.6)
box(10.1, 2.9, 3.55, 2.05, "Motion",
    ["MV(x, y) px/frame (0.25 단위)",
     "Frames a to b (b = last)",
     "At the edge",
     "Rotation/frame · Scale/frame"], "#f3e5f5", size=9.6)
box(10.1, 1.95, 3.55, 0.82, "버튼", ["Save scene... · Load scene... · Generate Y4M..."], "#e0f2f1", size=8.8)

# Outputs
box(0.2, 0.05, 13.6, 1.5, "4. Generate Y4M... 결과",
    ["<이름>.y4m  (기본 이름 generated_<W>x<H>_<N>f.y4m, YUV4MPEG2 8-bit 4:2:0 C420jpeg, BT.709 limited) → playlist 에 추가·선택",
     "Camera shake 가 Off 가 아니면 <이름>.y4m.camera.csv 도 씀 (frame,dx,dy,angle: flip 반영 출력 좌표)   |   장면은 Save scene... → JSON"],
    "#e0f2f1", size=9.8)

fig.savefig(OUT, dpi=110, bbox_inches="tight")
print(OUT)
