# bd_analyzer 설치 안내

Rocky Linux 8 / RHEL 8 (x86_64) 대상. Qt · FFmpeg · dav1d 디코더가 모두 번들되어
있으므로 대상 머신에 Qt 를 설치하지 않아도 된다.

## RPM (권장)

```bash
sudo dnf install ./bd-analyzer-<version>.el8.x86_64.rpm
bd-analyzer                  # 빈 상태로 시작
bd-analyzer stream.ivf       # 파일 열기
```

`dnf` 가 대상 머신에 없는 시스템 라이브러리(libGL, xkb 데이터, 폰트 등)를 함께
설치한다. 폴더 복사 방식에서 자주 났던 "Could not load the Qt platform plugin xcb"
류의 실패가 이 단계에서 사라진다.

애플리케이션 메뉴에도 등록되므로 GUI 환경에서는 아이콘으로 실행할 수 있다.

제거:

```bash
sudo dnf remove bd-analyzer
```

설치 위치는 `/opt/bd-analyzer/` 이고 `/usr/bin/bd-analyzer` 가 그 안의 런처를
가리키는 심볼릭 링크다.

## tar.gz (root 권한이 없는 머신)

```bash
tar xzf bd-analyzer-<version>-linux-x86_64.tar.gz
cd bd-analyzer-<version>
./check-deps.sh              # 부족한 것이 있는지 먼저 확인
./bd-analyzer stream.ivf
```

RPM 과 달리 의존성을 자동으로 채워 주지 못하므로, `check-deps.sh` 가 무언가를
보고하면 관리자에게 아래 패키지를 요청해야 한다.

```bash
sudo dnf install -y libglvnd-glx libglvnd-egl mesa-dri-drivers \
                    xkeyboard-config fontconfig freetype dejavu-sans-fonts
```

## `YUView` 를 직접 실행하면 안 되는 이유

바이너리의 `DT_RUNPATH` 가 빌드 머신의 Qt 경로를 가리키고 있어서, 그 경로가 없는
머신에서는 시작하지 못한다. `bd-analyzer` 런처가 `LD_LIBRARY_PATH` 와
`QT_PLUGIN_PATH` 를 번들 쪽으로 잡아 준다.

## CentOS 7.x 는 지원하지 않는다

번들된 `libQt6Core.so.6.5.3` 이 `GLIBC_2.28` 심볼을 요구한다 (CentOS 7.9 는 glibc
2.17). RPM 의 의존성에도 `glibc >= 2.28` 이 들어가므로 설치 자체가 거부된다.
지원하려면 CentOS 7 에서 Qt 6 을 소스 빌드해야 하고, 이는 별도 작업이다.

## 캐시

디코딩 캐시는 쓰기 가능한 위치를 자동으로 고른다. `/opt` 설치본은 실행 파일 옆에
쓸 수 없으므로 `~/.cache/` 아래로 폴백한다 (`BDCachePaths`).
