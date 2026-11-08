# Figure for the Confluence usage page "Info 패널 Frame 탭 (coding_order / order_hint) 사용법":
# where the Info dock is in bd-analyzer 0.5.4 and which rows the Frame tab shows per codec.
# Run: python3.11 wiki-usage-frame-tab.py  (writes wiki-usage-frame-tab.png next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, Rectangle

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wiki-usage-frame-tab.png")

fig, ax = plt.subplots(figsize=(14, 8.4))
ax.set_xlim(0, 14)
ax.set_ylim(0, 8.4)
ax.axis("off")


def rect(x, y, w, h, label, fc, size=10.5, bold=False, ec="#555555"):
    ax.add_patch(Rectangle((x, y), w, h, fc=fc, ec=ec, lw=1.2))
    ax.text(x + w / 2, y + h / 2, label, ha="center", va="center", fontsize=size,
            fontweight="bold" if bold else "normal")


def box(x, y, w, h, title, lines, fc, size=10.2):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.05,rounding_size=0.12", fc=fc, ec="#555555", lw=1.3))
    ax.text(x + 0.15, y + h - 0.2, title, ha="left", va="top", fontsize=12, fontweight="bold")
    for i, line in enumerate(lines):
        ax.text(x + 0.15, y + h - 0.62 - i * 0.34, line, ha="left", va="top", fontsize=size)


def arrow(x1, y1, x2, y2):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="-|>", mutation_scale=16, lw=1.5, color="#333333"))


ax.text(7, 8.25, "Info dock 의 Frame 탭 위치 (bd-analyzer 0.5.4, Default 배치)", ha="center", va="top",
        fontsize=15, fontweight="bold")

# Main window sketch
ax.add_patch(Rectangle((0.2, 0.3), 7.6, 7.3, fc="#fafafa", ec="#333333", lw=1.5))
ax.text(0.35, 7.45, "bd-analyzer - <파일명>", ha="left", va="center", fontsize=10.5, fontweight="bold")
rect(0.2, 6.75, 7.6, 0.5, "Frame Timeline (위쪽, Ctrl+Shift+T) — 막대 클릭 = 그 프레임으로 이동", "#e3f2fd", size=9.5)
rect(0.35, 5.45, 1.9, 1.15, "Playlist\n(Ctrl+L)", "#eeeeee", size=9.5)
rect(0.35, 2.75, 1.9, 2.6, "Info\n(Ctrl+I)\n\nFile | Frame | OBU", "#fff3cd", size=10, bold=True, ec="#c77800")
rect(0.35, 0.45, 1.9, 2.2, "Block Info\n(Ctrl+B)", "#eeeeee", size=9.5)
rect(2.4, 2.0, 3.3, 4.6, "그림\n(overlay toolbar F1–F8)", "#ffffff", size=10)
rect(2.4, 0.45, 3.3, 1.45, "hex dump\n재생 컨트롤 (Ctrl+D)", "#eeeeee", size=9.5)
rect(5.85, 4.15, 1.8, 2.45, "Properties\n(Ctrl+P)", "#eeeeee", size=9.5)
rect(5.85, 0.45, 1.8, 3.6, "Frame Info\n(Ctrl+Shift+F)\n\n이 탭과는\n다른 패널", "#f5f5f5", size=9.5)

# Ways to open
box(8.2, 5.15, 5.6, 2.45, "Info (왼쪽 노란 칸) 를 여는 법",
    ["View → Dock Panels → Show Info  (Ctrl+I)",
     "View → Perspective:",
     "  Default (Alt+1), Bitstream debug (Alt+2) → 보임",
     "  YUV compare (Alt+3), RD / BD-rate (Alt+4),",
     "  ME / GM (Alt+5) → 숨김"], "#e8f5e9")

# Rows per codec
box(8.2, 0.3, 2.75, 4.55, "Frame 탭 행: AV1",
    ["YUView frame index",
     "coding_order",
     "order_hint",
     "frame_type",
     "show_existing_frame",
     "Hidden frames in this TU",
     "AVPacket",
     "+ 그 패킷의 OBU 트리"], "#fff3cd", size=9.8)
box(11.1, 0.3, 2.7, 4.55, "Frame 탭 행: H.264",
    ["YUView frame index",
     "coding_order",
     "POC",
     "frame_num",
     "slice_type",
     "nal_ref_idc",
     "IDR",
     "AVPacket",
     "+ NAL 트리 (0.5.1+)"], "#fff3cd", size=9.8)

fig.savefig(OUT, dpi=110, bbox_inches="tight")
print(OUT)
