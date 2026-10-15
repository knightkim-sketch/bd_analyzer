# Draws docs/ai/30-designs/img/opencv-filter-overview.png for the Confluence page on OpenCV in
# bd-analyzer: left, how the optional static OpenCV is wired today (commit 47ef91e, patch 0065);
# right, the approved filter-chain plan, which is NOT implemented (labelled as such in the image).
# Source: the bd-analyzer-2f session's review (2026-10-15).
# Run: python3.11 opencv-filter-diagrams.py   (writes next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

HERE = os.path.dirname(os.path.abspath(__file__))
plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]

fig, ax = plt.subplots(figsize=(15, 8.6))
ax.set_xlim(0, 15)
ax.set_ylim(0, 8.6)
ax.axis("off")


def box(x, y, w, h, title, body, fc, dashed=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.04", fc=fc, ec="#172b4d", lw=1.0,
                                ls="--" if dashed else "-"))
    ax.text(x + 0.15, y + h - 0.12, title, ha="left", va="top", fontsize=10.5, weight="bold", color="#172b4d")
    ax.text(x + 0.15, y + h - 0.48, body, ha="left", va="top", fontsize=8.4, color="#172b4d", linespacing=1.45)


def arrow(x0, y0, x1, y1, dashed=False):
    ax.add_patch(FancyArrowPatch((x0, y0), (x1, y1), arrowstyle="-|>", mutation_scale=13, lw=1.1,
                                 color="#172b4d", ls="--" if dashed else "-"))


# ---- left: today -----------------------------------------------------------------------------
ax.text(3.5, 8.35, "현재 (커밋 47ef91e, 패치 0065) — 확인됨", ha="center", fontsize=12, weight="bold", color="#006644")
box(0.4, 6.75, 6.2, 1.25, "1. scripts/setup-opencv.sh",
    "OpenCV 4.14.0 소스 빌드 → ~/opt/opencv-4.14 (static .a, PIC, gcc-toolset-13)\n"
    "core · imgproc · features2d · video · flann · calib3d", "#e3fcef")
box(0.4, 5.05, 6.2, 1.25, "2. scripts/build.sh",
    "BDA_OPENCV_DIR (기본 ~/opt/opencv-4.14, OPENCV_DIR=... 로 변경)\n→ qmake 에 전달", "#e3fcef")
box(0.4, 2.75, 3.0, 1.85, "3a. YUViewLib.pro",
    "include/opencv4 추가\nDEFINES += BDA_HAVE_OPENCV\n\"OpenCV: <dir>\" 출력", "#e3fcef")
box(3.6, 2.75, 3.0, 1.85, "3b. YUViewApp.pro",
    "-lopencv_video ... -lopencv_core\n-lz -ldl (opencv4.pc 순서)\n앱에서 링크 (staticlib 은\nLIBS 를 넘기지 않음)", "#e3fcef")
box(0.4, 0.55, 6.2, 1.75, "OpenCV 가 없으면",
    "qmake: \"OpenCV: not found ..., building without it\"\n"
    "define·라이브러리 없이 빌드 — #ifdef BDA_HAVE_OPENCV 코드는 빠짐\n"
    "실행 시 새 .so 없음 (libz 는 이미 로드됨), 사용자 기능은 아직 없음", "#f4f5f7")
arrow(3.5, 6.75, 3.5, 6.3)
arrow(1.9, 5.05, 1.9, 4.6)
arrow(5.1, 5.05, 5.1, 4.6)

# ---- right: plan -----------------------------------------------------------------------------
ax.text(11.0, 8.35, "계획 — 승인됨, 미구현 (패치 0066 예정)", ha="center", fontsize=12, weight="bold", color="#bf2600")
box(7.6, 6.75, 6.8, 1.25, "YUV 속성 패널 · Convolution filter (luma)",
    "Enable · 필터 선택 + Add · 단계 목록 (켜기/제거/위/아래)\n선택 단계의 파라미터 · \"Use as ME input\"", "#fffae6", True)
stages = "Gaussian · Box · Median · Bilateral · Sobel/Scharr · Laplacian · Canny · Unsharp\n" \
         "Morphology · HistEq · CLAHE · Threshold · Adaptive · Pyramid · Keypoint map\n" \
         "GM compensation (ORB+RANSAC, 체인당 1개) · Custom 3x3 (OpenCV 없이도)"
box(7.6, 4.35, 6.8, 1.95, "단계 cascade (luma 만, 8~16 bit)",
    "프레임 → 단계 1 → 단계 2 → … (앞 단계 출력이 다음 입력)\n" + stages, "#fffae6", True)
box(7.6, 1.6, 2.1, 2.3, "화면 표시", "필터 결과를\n뷰에 표시", "#fffae6", True)
box(9.95, 1.6, 2.1, 2.3, "ME 입력", "\"Use as ME input\"\n켜면 ME 가\n필터 결과를 읽음\n필터 변경 시\nME 재시작", "#fffae6", True)
box(12.3, 1.6, 2.1, 2.3, "Y4M 내보내기", "File > Export\nfiltered Y4M\n(4:2:0 8/10bit)\n→ GM 창·CLI 입력", "#fffae6", True)
ax.text(11.0, 0.85, "위험 (미검증): GM compensation 은 k−1 프레임이 필요 → 압축 스트림에서 디코더 재탐색 가능\n"
        "OpenCV 스레드와 캐시 스레드가 겹쳐 CPU 과점유 가능 · 필터 상태는 playlist 에 저장되지 않음",
        ha="center", fontsize=8.4, color="#bf2600")
arrow(11.0, 6.75, 11.0, 6.3, True)
for x in (8.65, 11.0, 13.35):
    arrow(11.0, 4.35, x, 3.9, True)

fig.tight_layout()
fig.savefig(os.path.join(HERE, "opencv-filter-overview.png"), dpi=130)
print("written")
