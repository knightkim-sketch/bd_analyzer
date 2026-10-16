# Draws the two images of the Confluence page "RANSAC - global motion / DIS 적용 검토" (child of the
# OpenCV filter page): ransac-loop.png (the RANSAC iteration) and ransac-usage.png (where it would sit
# in bd-analyzer: today's OpenCV GM compensation stage, and the proposed RANSAC on block ME vectors
# feeding the GM window, the closed-loop start MV and a DIS export).
# Source: review of src/filter/FilterChain.cpp, src/me/GlobalMotion.cpp and the ME estimators (2026-10-16).
# Run: python3.11 ransac-diagrams.py   (writes next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

HERE = os.path.dirname(os.path.abspath(__file__))
plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]


def make(w, h):
    fig, ax = plt.subplots(figsize=(w, h))
    ax.set_xlim(0, w)
    ax.set_ylim(0, h)
    ax.axis("off")
    return fig, ax


def box(ax, x, y, w, h, title, body, fc, dashed=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.04", fc=fc, ec="#172b4d", lw=1.0,
                                ls="--" if dashed else "-"))
    ax.text(x + 0.15, y + h - 0.12, title, ha="left", va="top", fontsize=10.5, weight="bold", color="#172b4d")
    if body:
        ax.text(x + 0.15, y + h - 0.5, body, ha="left", va="top", fontsize=8.6, color="#172b4d", linespacing=1.45)


def arrow(ax, x0, y0, x1, y1, dashed=False, label=None):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=13, lw=1.1,
                                 color="#172b4d", ls="--" if dashed else "-"))
    if label:
        ax.text((x0 + x1) / 2 + 0.08, (y0 + y1) / 2, label, fontsize=8.2, color="#5e6c84", va="center")


# ---- 1. RANSAC loop ----------------------------------------------------------------------------
fig, ax = make(12, 6.2)
ax.text(6, 5.95, "RANSAC — 이상치가 섞인 대응점에서 모델 하나 맞추기", ha="center", fontsize=12.5, weight="bold",
        color="#172b4d")
box(ax, 0.3, 4.0, 3.3, 1.45, "입력: 대응점 쌍",
    "(블록 중심, 중심 + MV)\n또는 (특징점, 매칭된 특징점)\n이상치 포함", "#deebff")
box(ax, 4.3, 4.0, 3.4, 1.45, "① 최소 표본 무작위 추출",
    "평행이동 1점 · rotzoom 2점\naffine 3점 · homography 4점", "#fffae6")
box(ax, 8.4, 4.0, 3.3, 1.45, "② 표본으로 모델 계산", "최소 표본이므로 해가 바로 정해짐", "#fffae6")
box(ax, 8.4, 1.75, 3.3, 1.6, "③ inlier 세기",
    "모든 쌍에 모델 적용 → 오차가\n임계값(예: 1~2 px) 이하인 쌍 수", "#fffae6")
box(ax, 4.3, 1.75, 3.4, 1.6, "④ 최대 inlier 모델 기억",
    "N 회 반복 (①~③)\n고정 seed 면 결과가 결정적", "#fffae6")
box(ax, 0.3, 1.75, 3.3, 1.6, "⑤ inlier 전체로 재추정",
    "최소제곱 → 최종 모델\n+ inlier 마스크 (전역 움직임을\n따르는 블록 / 아닌 블록)", "#e3fcef")
arrow(ax, 3.6, 4.72, 4.3, 4.72)
arrow(ax, 7.7, 4.72, 8.4, 4.72)
arrow(ax, 10.05, 4.0, 10.05, 3.35)
arrow(ax, 8.4, 2.55, 7.7, 2.55)
arrow(ax, 6.0, 3.35, 6.0, 4.0, dashed=True, label="반복")
arrow(ax, 4.3, 2.55, 3.6, 2.55, label="")
ax.text(6, 0.85, "표본이 적으면(예: GM window 16개) 신뢰도가 크게 떨어집니다 — 블록 ME 벡터(수백~수천 개)가 입력으로 적합합니다.",
        ha="center", fontsize=9.2, color="#5e6c84")
fig.savefig(os.path.join(HERE, "ransac-loop.png"), dpi=130, bbox_inches="tight")
plt.close(fig)

# ---- 2. usage in bd-analyzer ---------------------------------------------------------------------
fig, ax = make(15, 8.2)
ax.set_ylim(1.3, 8.2)
ax.text(3.4, 7.95, "현재 — 구현되어 있음", ha="center", fontsize=12, weight="bold", color="#006644")
box(ax, 0.3, 5.75, 6.2, 1.8, "Convolution filter: GM compensation (RANSAC)",
    "ORB 특징점 → Hamming 매칭 → OpenCV RANSAC\n(estimateAffinePartial2D / estimateAffine2D /\n findHomography)\n"
    "→ 현재 프레임을 직전 프레임에 warp", "#e3fcef")
box(ax, 0.3, 3.4, 6.2, 1.9, "한계",
    "· 직전 프레임에만 정렬 — 누적 궤적 없음 (DIS 아님)\n· OpenCV 내부 RNG — 결정적 재현을 보장하기 어려움\n"
    "· GlobalMotion: window 16개 MV 그대로, 모델 피팅 없음\n  (가장자리 window 실패, 회전·zoom 표현 불가)", "#ffebe6")
arrow(ax, 3.4, 5.75, 3.4, 5.3)

ax.text(10.8, 7.95, "제안 — 미구현", ha="center", fontsize=12, weight="bold", color="#bf2600")
box(ax, 7.3, 6.0, 3.0, 1.55, "블록 ME 벡터", "SVT / open-loop / adv /\nclosed-loop, 16x16 등", "#deebff", dashed=True)
box(ax, 11.2, 6.0, 3.5, 1.55, "① RANSAC 모델 피팅",
    "Qt-free 코어, 고정 seed\n평행이동 / rotzoom / affine", "#fffae6", dashed=True)
arrow(ax, 10.3, 6.77, 11.2, 6.77)
box(ax, 7.3, 3.4, 2.3, 1.9, "GM window", "전역 모델 표시\ninlier 마스크 overlay\n(로컬 움직임 영역)",
    "#eae6ff", dashed=True)
box(ax, 9.95, 3.4, 2.3, 1.9, "② closed-loop\n시작 MV", "\n블록 중심에서\n모델이 예측한 MV", "#eae6ff", dashed=True)
box(ax, 12.6, 3.4, 2.1, 1.9, "③ DIS", "궤적 누적 → 평활화\n→ 보정 warp → crop\nExport Stabilized Y4M",
    "#eae6ff", dashed=True)
for x in (8.45, 11.1, 13.65):
    arrow(ax, 12.95, 6.0, x, 5.3, dashed=True)
box(ax, 7.3, 1.5, 7.4, 1.25, "검증용 영상: Y4M sequence generator",
    "화면 전체에 알려진 흔들림(camera shake)을 주는 옵션\n→ 정답 궤적이 있는 영상으로 RANSAC 모델·DIS 정량 평가",
    "#f4f5f7", dashed=True)
arrow(ax, 13.65, 2.75, 13.65, 3.4, dashed=True)
fig.savefig(os.path.join(HERE, "ransac-usage.png"), dpi=130, bbox_inches="tight")
plt.close(fig)
print("written")
