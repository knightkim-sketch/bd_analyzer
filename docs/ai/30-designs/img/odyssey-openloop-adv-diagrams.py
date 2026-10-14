# Draws the two diagrams of docs/ai/30-designs/odyssey-openloop-adv-design.md and its Confluence page:
#   odyssey-openloop-adv-gop.png     GF 16 group: layers, bottom-up analysis order, references
#   odyssey-openloop-adv-search.png  one superblock search, step 1 to step 4
# The GOP data is odyssey's own trace (tests/unit/me-odyssey-gf-structure.cpp).
# Run: python3.11 odyssey-openloop-adv-diagrams.py   (writes next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

HERE = os.path.dirname(os.path.abspath(__file__))
plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]

# ---- 1. GF 16 group ----------------------------------------------------------------------------
layer = {0: 0, 16: 1, 8: 2, 4: 3, 12: 3}
for p in (2, 6, 10, 14):
    layer[p] = 4
for p in range(1, 16, 2):
    layer[p] = 5
order = [1, 3, 5, 7, 9, 11, 13, 15, 2, 6, 10, 14, 4, 12, 8, 16]
refs = {  # POC: (list-0 reference, list-1 reference or None)
    1: (0, 2), 2: (0, 4), 3: (2, 4), 4: (0, 8), 5: (4, 6), 6: (4, 8), 7: (6, 8), 8: (0, 16),
    9: (8, 10), 10: (8, 12), 11: (10, 12), 12: (8, 16), 13: (12, 14), 14: (12, 16), 15: (14, 16),
    16: (0, None),
}
colors = {0: "#97a0af", 1: "#bf2600", 2: "#ff991f", 3: "#36b37e", 4: "#4c9aff", 5: "#6554c0"}

fig, ax = plt.subplots(figsize=(15, 6.2))
ypos = lambda l: 5.6 - l  # layer 1 on top, key frame at the bottom row
for poc in range(17):
    l = layer[poc]
    x, y = poc, ypos(l if l else 5.6)
    kind = "I" if poc == 0 else ("P" if l == 1 else "B")
    ax.add_patch(FancyBboxPatch((x - 0.38, y - 0.3), 0.76, 0.6, boxstyle="round,pad=0.02",
                                fc=colors[l], ec="black", lw=0.8))
    ax.text(x, y + 0.06, f"{poc}", ha="center", va="center", color="white", fontsize=11, weight="bold")
    ax.text(x, y - 0.18, kind, ha="center", va="center", color="white", fontsize=7)
    if poc in order:
        ax.text(x, y + 0.45, f"#{order.index(poc) + 1}", ha="center", va="bottom", fontsize=8, color="#172b4d")

for poc, (r0, r1) in refs.items():
    y = ypos(layer[poc])
    for r, c, rad in ((r0, "#0747a6", 0.25), (r1, "#bf2600", -0.25)):
        if r is None:
            continue
        yr = ypos(layer[r]) if layer[r] else ypos(5.6)
        ax.add_patch(FancyArrowPatch((poc, y), (r, yr), arrowstyle="-|>", mutation_scale=8, lw=0.7,
                                     color=c, alpha=0.55, connectionstyle=f"arc3,rad={rad}",
                                     shrinkA=12, shrinkB=12))
for l in range(1, 6):
    ax.text(-1.3, ypos(l), f"L{l}", ha="center", va="center", fontsize=10, weight="bold", color=colors[l])
ax.text(-1.3, ypos(5.6), "KF", ha="center", va="center", fontsize=10, weight="bold", color=colors[0])
ax.set_xlim(-1.9, 16.8)
ax.set_ylim(-0.6, 5.4)
ax.axis("off")
ax.set_title("odyssey open-loop ME — GF 16: layer, 분석 순서 (#), 참조 (파랑 L0 / 빨강 L1)", fontsize=12)
ax.text(8, -0.45,
        "분석은 bottom-up: L5(홀수) → L4 → L3 → L2 → P(16).  B 는 양방향 1개씩, P 는 L0 만 (두 번째 GOP 부터 prev golden 추가).  "
        "남는 tail 은 2 의 거듭제곱 GOP 로 분할 (예: 7 = 4 + 2 + 1)",
        ha="center", fontsize=8.5, color="#172b4d")
fig.tight_layout()
fig.savefig(os.path.join(HERE, "odyssey-openloop-adv-gop.png"), dpi=130)

# ---- 2. One superblock search ---------------------------------------------------------------------
steps = [
    ("Step 1  후보 구성", "21 slot: A B C D | TDMP×4 (off) | TMV×8 | ZERO | MEDIAN(D,B,C) | A/C/B_PASS\n"
     "→ >>3 (half px) → 범위 검사 · 중복 제거 → 앞 6개 → 패딩 클램프\n(+ 옵션: 외부 center MV 를 맨 앞)", "#deebff"),
    ("Step 2  center 결정", "half 영상, 32×32, 후보마다 ±1 (9점)\ncost = SAD + divide_round(mvcost(pmv−mv), 2)", "#e3fcef"),
    ("Step 3a  coarse", "half 영상, step 2, 32×32 창을 2×2 영역으로 (각 8×8 = 64점, 비트 교차 순서)\n"
     "center 사전 조정 + clamp_search_range 로 창 이동, cost = SSE + (divide_round(mvcost, 6))²<<6\n영역 결과 4개를 cost 로 안정 정렬", "#fffae6"),
    ("Step 3b  half", "half 영상, step 1, 각 center ±(−4..+3) 8×8, 같은 cost → 최선 1개", "#fffae6"),
    ("Step 4  VBS", "half→org 업스케일 영상, center = 3b×2, −8..+7 (4 pass × 8×8, 비트 교차)\n"
     "8×8 SSE (행·열 2:1) + (divide_round(mvcost, 8))² → 16/32/64 합산, 크기별 최소", "#ffebe6"),
    ("저장", "64×64 MV → 이웃 line buffer / A 지연 (참조 2개: 2 SB, 1개: 4 SB)\n"
     "TMV 저장 (기본 16×16, 원본 대조 모드 SB 64×64)", "#eae6ff"),
]
fig, ax = plt.subplots(figsize=(11, 9.2))
y = 9.0
boxes = []
for title, body, c in steps:
    h = 0.80 + 0.30 * body.count("\n")
    ax.add_patch(FancyBboxPatch((0.4, y - h), 9.2, h, boxstyle="round,pad=0.05", fc=c, ec="#172b4d", lw=0.9))
    ax.text(0.6, y - 0.15, title, ha="left", va="top", fontsize=11, weight="bold", color="#172b4d")
    ax.text(0.6, y - 0.47, body, ha="left", va="top", fontsize=8.6, color="#172b4d", linespacing=1.45)
    boxes.append((y, y - h))
    y -= h + 0.42
for (t0, b0), (t1, b1) in zip(boxes, boxes[1:]):
    ax.add_patch(FancyArrowPatch((5, b0 - 0.04), (5, t1 + 0.08), arrowstyle="-|>", mutation_scale=14, lw=1.2,
                                 color="#172b4d"))
ax.set_xlim(0, 10)
ax.set_ylim(y, 9.4)
ax.axis("off")
ax.set_title("odyssey open-loop ME — SB 하나의 탐색 (참조마다, 가까운 참조부터)", fontsize=12)
fig.tight_layout()
fig.savefig(os.path.join(HERE, "odyssey-openloop-adv-search.png"), dpi=130)
print("written")
