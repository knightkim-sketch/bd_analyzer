---
title: TASK-0008 설치형 패키지 (RPM + tar.gz)
status: implemented
created: 2026-08-20
updated: 2026-08-20
author: claude-opus-5
verified: "RPM/tar.gz 생성, 페이로드 대조, 의존성 해결, 정리된 환경 실행, bm32 실패 재현·해소까지 실측"
upstream: IENT/YUView @ a72eb3488097313511e60ed70db4af6071cbe9fe
---

## 배경

사용자 신고: *"YUView 가 내 머신에서는 잘 도는데, 다른 머신에서는 문제가 많아.
설치형 package 로 만들어 줄 수 있어?"*

TASK-0007 에서 커밋한 `bin/` 번들(폴더째 복사 + `YUView.sh` 런처)은 자기충족적이지만,
**대상 머신에 없는 시스템 패키지는 스스로 채울 수 없다.** `bin/README.md` 가 남은 위험
지점으로 적어 둔 것이 정확히 그것이다 — `libglvnd-glx`/`libglvnd-egl`, `xkeyboard-config`,
`fontconfig`/`freetype`. 폰트 데이터(`dejavu-sans-fonts`)와 GL 드라이버(`mesa-dri-drivers`)도
같은 범주다. 이 목록을 사람이 README 를 읽고 손으로 맞추는 구조가 "다른 머신에서 문제가
많다" 의 실체였다.

## 결정

**RPM 을 1차 산출물로 한다.** ADR-0002 는 AppImage 를 1차로 뒀지만, 지금 실제로 깨지는
원인이 "대상 머신의 시스템 패키지 부재"이고 그것을 자동으로 해결하는 것은 dnf 뿐이다.
AppImage 는 파일 하나로 줄여 주지만 `libGL`/xkb 데이터 문제는 그대로 남는다.
`rpmbuild` 가 빌드 머신에 이미 있어 추가 다운로드도 필요 없다.

보조로 **tar.gz** — root 권한이 없는 머신용. AppImage 는 필요해지면 그때.

### 대상: Rocky 8 만. CentOS 7.9 는 불가

사용자 요청에 CentOS 7.9 가 포함됐으나 실측으로 배제했다. 번들된
`libQt6Core.so.6.5.3` 이 `GLIBC_2.28` 심볼을 요구하고 CentOS 7.9 는 glibc 2.17 이다
(`libavformat`/`libavutil` 도 2.28, `libQt6Gui`/`libavcodec` 은 2.27).
지원하려면 CentOS 7 에서 Qt 6 을 소스 빌드해야 한다 — 별도 작업.

RPM 의 자동 의존성에 `libc.so.6(GLIBC_2.28)` 이 들어가므로 **CentOS 7 에서는 설치가
자동으로 거부된다.** 조용히 깨지는 대신 설치 시점에 거부되는 쪽이 낫다.

## 구현

| 파일 | 역할 |
|---|---|
| `packaging/rpm/bd-analyzer.spec` | spec. 페이로드는 `bin/` 번들 그대로 |
| `packaging/common/bd-analyzer` | 런처. rpm/tar 두 레이아웃을 치환 없이 겸용 |
| `packaging/common/bd-analyzer.desktop` | 메뉴 등록 |
| `scripts/package.sh` | `rpm` / `tar` / `all` (+ `--refresh`) |
| `docs/deploy/package-README.md` | 패키지에 동봉되는 설치 안내 |

설치 레이아웃: `/opt/bd-analyzer/` + `/usr/bin/bd-analyzer` (심볼릭 링크) +
`/usr/share/{applications,icons/hicolor/*/apps,doc,licenses}`.

### 런처를 치환 없이 겸용하는 방법

`readlink -f "${BASH_SOURCE[0]}"` 로 자기 실체 경로를 구해 그 디렉토리를 `BDA_HOME` 으로
쓴다. tar.gz 는 런처가 `YUView` 옆에 있고, RPM 은 `/usr/bin/bd-analyzer` 가
`/opt/bd-analyzer/bd-analyzer` 를 가리키는 심볼릭 링크라 `readlink -f` 가 같은 곳으로
수렴한다. 빌드 시점 경로 치환(`@BDA_HOME@`)이 필요 없다.

### spec 에서 실제로 어려웠던 부분 — 자동 의존성 생성기 길들이기

번들 페이로드를 그냥 넣으면 rpm 이 **번들된 Qt 를 시스템 Qt 인 것처럼 Provides 로 내보내고,
동시에 그것을 Requires 로도 잡아** 설치 불가 패키지가 된다. 그렇다고 자동 생성을 통째로
끄면(`AutoReqProv: no`) `glibc`/`libstdc++`/`libGL` 같은 **진짜** 의존성까지 사라진다 —
그런데 그 목록이 이 작업의 목적 전부다.

그래서 세 갈래로 나눴다.

1. `__provides_exclude_from ^/opt/bd-analyzer/.*$` — 번들 라이브러리를 시스템에 광고하지 않는다.
2. `__requires_exclude` 의 `_bundled` 그룹 — 번들이 스스로 제공하는 soname
   (`libQt6*`, `libicu*`, `libav*`, `libswresample`, `libdav1d-internals`, X11/xcb 계열)만 제거.
   나머지 자동 탐지 결과는 그대로 둔다.
3. `__requires_exclude` 의 `_optional` 그룹 — **선택적 Qt 플러그인이 끌어오는 것.**
   `platformthemes/libqgtk3.so` 가 GTK3·pango·cairo·atk 전체를,
   `platforms/libqwayland-*.so` 가 `libwayland-*`, `libqlinuxfb.so` 가 `libdrm` 을 요구한다.
   Qt 는 이 플러그인들을 시작 시 훑어보고 라이브러리가 없으면 조용히 건너뛰므로,
   하드 의존성이 되면 얻는 것 없이 대상 머신에 GTK3/Wayland 스택을 끌어온다.
   페이로드는 검증된 번들과 동일하게 유지하고 의존성만 제거한 뒤, `gtk3` 는
   `Recommends:` 로 뒀다 — 데스크톱이면 네이티브 테마를 얻고, 아니면 그냥 넘어간다.

정규식은 `\.` 대신 `[.]` 를 쓴다. rpm 매크로를 한 번 더 통과하면서 백슬래시가 소비된다.

자동 생성기가 볼 수 없는 것은 명시적 `Requires:` 로 넣었다 — `xkeyboard-config`(라이브러리가
아니라 `/usr/share/X11/xkb` 데이터), `mesa-dri-drivers`(`libGL.so.1` 뒤의 드라이버),
`dejavu-sans-fonts`(폰트 데이터), `hicolor-icon-theme`, `bash`.

### 그 밖의 spec 설정

* `debug_package %{nil}` + `__os_install_post %{nil}` — 남의 바이너리를 strip 하지 않는다.
* `_build_id_links none` — 이게 없으면 `/usr/lib/.build-id/` 하위에 심볼릭 링크 115개가
  생긴다. `__os_install_post` 와 무관하게 패키징 시점에 만들어진다. 번들 Qt/FFmpeg 는
  upstream 빌드의 build-id 를 갖고 있어 같은 바이너리를 담은 다른 패키지와 **파일 충돌**을
  일으킬 수 있고, debuginfo 를 배포하지 않으므로 링크의 소비자도 없다.

### 페이로드 출처

기본은 커밋된 `bin/` 번들이다 — 실제로 실행해서 검증한 산출물이 그것이고, Qt 가 없는
머신에서 clone 만으로 패키징이 가능하다. `--refresh` 를 주면 `make-bin-bundle.sh` 로
`build/YUViewApp` 에서 다시 만든다.

`bin/` 은 건드리지 않았다. 패키지용 조정(런처 추가, `check-deps.sh` 의 안내 문구를
`./YUView.sh` → `./bd-analyzer` 로)은 스테이징 디렉토리에서만 한다.

## 검증 (실측)

| 항목 | 방법 | 결과 |
|---|---|---|
| CentOS 7.9 배제 근거 | `objdump -T` 로 번들 전체의 최대 GLIBC 심볼 | `libQt6Core` → `GLIBC_2.28` |
| 페이로드 완전성 | `bin/` 파일 목록 vs `rpm -qpl` 대조 | 누락 0. 추가는 런처 1개뿐 |
| Provides 누출 | `rpm -qp --provides` | `application()`, `bd-analyzer` 만. 번들 soname 없음 |
| 선택적 의존성 제거 | `rpm -qp --requires` | gtk3/wayland/drm/pango/cairo/atk 전부 사라짐 |
| build-id 링크 | `rpm -qpl \| grep build-id` | 0건 (총 126 파일) |
| 의존성 해결 가능성 | 각 Requires 를 `rpm -q --whatprovides` | 미해결 0건, 18개 패키지로 해결 |
| 파일 레이아웃/퍼미션 | `rpm2cpio \| cpio -idm` 후 `stat` | 실행 비트, SONAME 심볼릭 링크 보존 |
| desktop 파일 | `desktop-file-validate` | 경고 없음 (`Development` 를 빼서 main category 중복 해소) |
| 정리된 환경 실행 | `env -i` + `/proc/PID/maps` 검사 | RPM·tar 양쪽 실행, 빌드 머신 경로 유출 0, 번들 .so 180개 |
| **bm32 실패 재현·해소** | `unshare -rm` 으로 `libxcb-cursor.so.0` 을 빈 파일로 마스킹 | 직접 실행은 신고와 동일한 오류로 실패, 런처 경유는 정상 실행 |
| `check-deps.sh` | 최종 페이로드에서 실행 | 미해결 라이브러리 0건 |

`dnf install --setopt=tsflags=test` 는 root 권한이 필요해 실행하지 못했다. 대신 각
Requires 를 로컬 rpmdb 로 해결해 봤다 (타겟이 동일 Rocky 8 이므로 유효한 대체 검증이지만,
**실제 dnf 트랜잭션은 미검증**이다).

## 남은 것

* 실제 다른 머신(bm32 등)에서의 `dnf install` — 사용자 환경에서 확인 필요
* MIME 등록 없음. `.yuv`/`.ivf` 를 파일 관리자에서 "연결 프로그램"으로 열려면
  `shared-mime-info` XML 이 추가로 필요하다
* 서명 없음. 사내 배포라 `--nogpgcheck` 없이 설치하려면 GPG 키가 필요해진다
* AppImage / Flatpak 은 만들지 않았다 (ADR-0002 의 보조 산출물로 남아 있음)
