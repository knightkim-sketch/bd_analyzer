# Figure for the Confluence usage page "디코더 plugin 추가 (사용자)": where a decoder .so goes, the
# search order, and where the decoder is chosen in bd-analyzer 0.5.4.
# Run: python3.11 wiki-usage-decoder.py  (writes wiki-usage-decoder.png next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wiki-usage-decoder.png")

fig, ax = plt.subplots(figsize=(14, 8.6))
ax.set_xlim(0, 14)
ax.set_ylim(0, 8.6)
ax.axis("off")


def box(x, y, w, h, title, lines, fc, size=10.3):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.05,rounding_size=0.12", fc=fc, ec="#555555", lw=1.3))
    ax.text(x + 0.15, y + h - 0.2, title, ha="left", va="top", fontsize=12, fontweight="bold")
    for i, line in enumerate(lines):
        ax.text(x + 0.15, y + h - 0.62 - i * 0.35, line, ha="left", va="top", fontsize=size)


def arrow(x1, y1, x2, y2):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="-|>", mutation_scale=16, lw=1.5, color="#333333"))


ax.text(7, 8.4, "디코더 라이브러리(.so) 추가와 선택 (bd-analyzer 0.5.4)", ha="center", va="top", fontsize=15, fontweight="bold")

# Left: where to put the file
box(0.2, 5.0, 4.95, 2.9, "1. 파일 두기 (재빌드 불필요)",
    ["RPM 설치본 (root):",
     "  sudo cp libHMDecoder.so /opt/bd-analyzer/decoder/",
     "tar.gz 설치본:",
     "  cp libHMDecoder.so bd-analyzer-<ver>/decoder/",
     "root 없음: 아무 폴더 →",
     "  File → Settings... → Decoders",
     "  → Decoder Search Path 에 지정"], "#e8f5e9", size=9.8)

# Middle: search order
box(5.4, 3.6, 4.2, 4.3, "2. 찾는 순서 (먼저 찾은 것 사용)",
    ["0  Settings → Decoders 의 코덱별",
     "    라이브러리 파일 (지정했으면)",
     "1  Decoder Search Path",
     "2  현재 디렉토리, ./decoder/",
     "3  실행 파일 위치, <위치>/decoder/",
     "    RPM: /opt/bd-analyzer/decoder/",
     "4  시스템 경로 (LD_LIBRARY_PATH,",
     "    /usr/lib64 …)",
     "앱을 다시 시작해야 반영됩니다"], "#eef3fb", size=10)
arrow(5.15, 6.4, 5.4, 6.4)

# Right: choosing the decoder
box(10.1, 5.0, 3.6, 2.9, "3. 디코더 고르기",
    ["파일별: Properties dock",
     "  → Video 탭 → Decoder 콤보",
     "기본값: File → Settings...",
     "  → Decoders 탭",
     "  h.265/HEVC Default,",
     "  H.264 Default Decoder (0.5.4)"], "#f3e5f5", size=10)
arrow(9.6, 6.4, 10.1, 6.4)

# Bottom: default engines and fallbacks
box(0.3, 0.35, 6.6, 2.85, "기본 엔진 (번들 decoder/ 폴더) → 실패 시",
    ["AV1    Dav1d          libdav1d-internals.so  → FFmpeg",
     "H.264  JM (analysis)  libbda-jm-analyzer.so  → FFmpeg",
     "HEVC   HM             libHMDecoder.so        → FFmpeg",
     "열 때 로드에 실패하면 FFmpeg 로 넘어갑니다",
     "(블록 통계는 Dav1d·JM·HM 이 제공합니다)"], "#fff8e1", size=10)

box(7.3, 0.35, 6.4, 2.85, "확인과 주의",
    ["오류: Problems dock (Ctrl+Shift+P),",
     "  콤보로 직접 고를 때 \"Error loading library: …\"",
     "주의 (미검증, 코드 독해): Settings 창에서 OK 를 누르면",
     "  HEVC 기본값이 Libde265 로 저장될 수 있음 →",
     "  HEVC 가 안 열리면 h.265/HEVC Default 를 HM 으로",
     "IENT prebuilt libHMDecoder.so 는 끝 2–3 프레임 누락"], "#fdecea", size=10)

fig.savefig(OUT, dpi=110, bbox_inches="tight")
print(OUT)
