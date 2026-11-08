# Figure for the Confluence usage page "bd-analyzer 설치와 실행": RPM vs tar.gz install -> launch -> cache.
# Run: python3.11 wiki-usage-install.py  (writes wiki-usage-install.png next to this script)
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

plt.rcParams["font.family"] = ["Noto Sans CJK KR", "Noto Sans CJK TC", "DejaVu Sans"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wiki-usage-install.png")

fig, ax = plt.subplots(figsize=(14, 8.2))
ax.set_xlim(0, 14)
ax.set_ylim(0, 8.2)
ax.axis("off")


def box(x, y, w, h, title, lines, fc, ec="#555555", size=10.5):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.05,rounding_size=0.12", fc=fc, ec=ec, lw=1.3))
    ax.text(x + 0.15, y + h - 0.2, title, ha="left", va="top", fontsize=12, fontweight="bold")
    for i, line in enumerate(lines):
        ax.text(x + 0.15, y + h - 0.62 - i * 0.36, line, ha="left", va="top", fontsize=size, family=None)


def arrow(x1, y1, x2, y2, text=None):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="-|>", mutation_scale=16, lw=1.5, color="#333333"))
    if text:
        ax.text((x1 + x2) / 2, (y1 + y2) / 2 + 0.12, text, ha="center", va="bottom", fontsize=9.5, color="#333333")


ax.text(7, 7.95, "bd-analyzer 설치 → 실행 → 캐시 위치 (0.5.4 예)", ha="center", va="top", fontsize=15, fontweight="bold")

box(0.3, 5.55, 5.0, 1.95, "배포 폴더 (버전별)",
    ["/fs2/install_devel/YUViewer/bd-analyzer-v0.5.4/",
     "  bd-analyzer-0.5.4-1.gitdf2b816.el8.x86_64.rpm",
     "  bd-analyzer-0.5.4-linux-x86_64.tar.gz",
     "  ReleaseNote.md  (버전별 변경 내용)"], "#eef3fb", size=10)

box(6.0, 5.75, 3.7, 1.75, "A. RPM (권장, root)",
    ["sudo dnf install ./bd-analyzer-*.rpm",
     "→ /opt/bd-analyzer/",
     "→ /usr/bin/bd-analyzer (symlink)"], "#e8f5e9")
box(6.0, 3.55, 3.7, 1.85, "B. tar.gz (root 없음)",
    ["tar xzf bd-analyzer-*.tar.gz",
     "cd bd-analyzer-0.5.4",
     "./check-deps.sh  (부족한 패키지 보고)"], "#fff8e1")
arrow(5.3, 6.6, 6.0, 6.6)
arrow(5.3, 6.0, 6.0, 4.6)

box(10.3, 4.55, 3.4, 2.95, "실행 (런처)",
    ["A: bd-analyzer [파일]",
     "    또는 애플리케이션 메뉴",
     "B: ./bd-analyzer [파일]",
     "YUView 를 직접 실행하지",
     "않습니다 (런처가 Qt·FFmpeg",
     "라이브러리 경로를 잡음)"], "#f3e5f5", size=10)
arrow(9.7, 6.6, 10.3, 6.3)
arrow(9.7, 4.5, 10.3, 5.2)

box(0.3, 0.35, 8.6, 2.75, "캐시: 실행한 디렉토리의 .bd_analyzer/",
    ["cd /data/work            # 용량이 있는 볼륨",
     "bd-analyzer a.ivf        # → /data/work/.bd_analyzer/",
     "• 디코딩 YUV·블록 통계, GB 단위로 늘어납니다",
     "• 쓰기 불가 디렉토리 → 캐시 없이 동작 (다른 곳으로 폴백 없음)",
     "• 위치 고정: 설정 키 BDCache/directory",
     "• 종료 시 묻지 않고 삭제"], "#fdecea", size=10.5)
arrow(11.6, 4.55, 8.9, 2.6)
ax.text(10.55, 3.35, "실행 위치(cwd) 기준", ha="left", va="center", fontsize=9.5, color="#333333")

box(9.4, 0.35, 4.3, 2.75, "업그레이드 후 첫 실행",
    ["같은 dnf install 명령으로",
     "새 rpm 을 설치합니다",
     "이전 창 배치가 복원됨 →",
     "Help → Reset Window Layout",
     "버전 확인: Help → About bd-analyzer"], "#e0f2f1", size=10.5)

fig.savefig(OUT, dpi=110, bbox_inches="tight")
print(OUT)
