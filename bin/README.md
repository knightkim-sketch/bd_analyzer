# YUView 배포 번들 (bin/)

같은 Rocky 8 머신에 이 폴더째 복사해서 실행하기 위한 파일 모음이다.
`scripts/make-bin-bundle.sh` 로 생성한다.

## 실행

```bash
./YUView.sh                # 빈 상태로 시작
./YUView.sh stream.ivf     # 파일 열기
```

`YUView` 를 직접 실행하면 **안 된다.** 바이너리의 `DT_RUNPATH` 가 빌드 머신의
Qt 경로(`/home/knight2/Qt/6.5.3/gcc_64/lib`)를 가리키고 있어서, 그 경로가 없는
머신에서는 시작하지 못한다. `YUView.sh` 가 `LD_LIBRARY_PATH` 를 번들 쪽으로
잡아준다 (`LD_LIBRARY_PATH` 가 `DT_RUNPATH` 보다 먼저 검색되므로 번들이 이긴다).

## 구성

| 경로 | 내용 | 왜 필요한가 |
|---|---|---|
| `YUView` | 실행 파일 | |
| `YUView.sh` | 런처 | `LD_LIBRARY_PATH`, `QT_PLUGIN_PATH` 설정 |
| `qt.conf` | Qt 경로 설정 | Qt 가 플러그인을 빌드 시점 prefix 대신 실행 파일 옆에서 찾게 함 |
| `lib/` | Qt 6.5.3 + ICU (13개) | **Qt 는 동적 링크다.** 아래 "static 이 아닌 이유" 참조 |
| `plugins/platforms/` | `libqxcb.so` | **필수.** 없으면 "could not load the Qt platform plugin xcb" 로 죽는다 |
| `plugins/imageformats/`, `iconengines/`, `tls/` | 이미지/아이콘/TLS | 아이콘 렌더링, 업데이트 확인 |
| `plugins/platforminputcontexts/`, `platformthemes/` | 입력기/테마 | 선택. 대상에 ibus/GTK3 이 없으면 Qt 가 조용히 건너뛴다 |
| `syslib/` | X11 / xcb / xkb 헬퍼 20개 | xcb 플랫폼 플러그인이 요구. `xcb-util-*` 는 GUI 없는 설치에 대개 없어서 번들에 넣었다 |
| `ffmpeg/` | libavcodec/avformat/avutil/swresample | YUView 가 `applicationDirPath()/ffmpeg/` 에서 dlopen. **libdav1d 가 avcodec 안에 static 내장** |
| `decoder/libdav1d-internals.so` | dav1d analyzer fork | 블록 통계(Pred Mode 등). 없으면 FFmpeg 디코더로 폴백되고 통계가 안 나온다 |
| `check-deps.sh` | 진단 | 대상 머신에서 부족한 라이브러리를 한 번에 보고 |

## 새 머신에서 먼저 할 것

```bash
./check-deps.sh
```

부족한 라이브러리가 있으면 이름과 함께 보고한다. 아무것도 없으면 바로 `./YUView.sh`.

## static 이 아닌 이유 — 확인된 사실

이 빌드는 static 이 **아니다**. 확인 결과:

* `YUView` 는 Qt 6.5.3 을 동적 링크하고 `DT_RUNPATH` 에 빌드 머신의 Qt 경로가 박혀 있다.
* `libstdc++.so.6` 도 시스템(`/lib64`) 것을 쓴다. gcc-toolset-13 으로 빌드했지만
  새 표준 라이브러리 심볼은 정적으로 끌어와 시스템 libstdc++ 8.5 와 함께 동작한다.
* FFmpeg 과 dav1d 는 애초에 링크 대상이 아니라 **런타임 dlopen** 이다 (YUView 는
  FFmpeg 구조체 레이아웃을 버전별로 직접 재선언해서 쓴다 → static 링크는 불가).

따라서 "추가 lib 없이 동작"은 static 링크가 아니라 **필요한 라이브러리를 번들에
넣어서** 달성한다. `build/YUViewApp` 을 그대로 복사하면 안 되는 이유도 이것이다:
그 폴더의 `ffmpeg/*.so.NN` 은 `~/opt/ffmpeg-7.1` 을 가리키는 심볼릭 링크라 대상
머신에서 깨진다.

## 대상 머신에 필요한 것

번들에 없는 것은 OS 기본 공유 라이브러리 76개뿐이다. 제공 패키지:

기본 설치에 보통 있는 것: `glibc`, `libstdc++`, `libgcc`, `zlib`, `bzip2-libs`,
`xz-libs`, `lz4-libs`, `expat`, `pcre`, `pcre2`, `glib2`, `libffi`, `libselinux`,
`systemd-libs`, `dbus-libs`, `libuuid`, `libblkid`, `libmount`, `libcap`,
`openssl-libs`, `gnutls`, `nettle`, `gmp`, `libtasn1`, `p11-kit`, `libunistring`,
`libidn2`, `krb5-libs`, `libcom_err`, `keyutils-libs`, `libgcrypt`, `libgpg-error`,
`libtirpc`, `libnsl2`, `nss_nis`, `freetype`, `fontconfig`, `libpng`

X11/xcb 계열 20개는 **번들의 `syslib/` 에 들어 있으므로 설치하지 않아도 된다.**
실제로 bm32 에서 났던 오류가 이것이었다:

```
qt.qpa.plugin: From 6.5.0, xcb-cursor0 or libxcb-cursor0 is needed to load the Qt xcb platform plugin.
qt.qpa.plugin: Could not load the Qt platform plugin "xcb" in "" even though it was found.
Aborted (core dumped)
```

`libxcb-cursor.so.0` (`xcb-util-cursor` 패키지) 가 없어서 실패한 것이다. Qt 6.5 부터
xcb 플러그인의 필수 의존성인데 GUI 를 쓰지 않는 Rocky 설치에는 대개 없다. 지금은
`syslib/` 에 번들되어 있고, `LD_LIBRARY_PATH` 가 `/lib64` 보다 먼저 검색되므로
대상 머신에 있든 없든 번들 쪽이 쓰인다.

**아직 대상 머신에 필요한 것 — 남은 위험 지점:**

```bash
sudo dnf install -y libglvnd-glx libglvnd-egl fontconfig freetype xkeyboard-config
```

* `libglvnd-*` (libGL/libEGL): 그래픽 드라이버와 결합되어 있어 번들에 넣지 않았다.
  빌드 머신 것을 복사하면 대상의 드라이버와 어긋날 수 있다.
* `fontconfig`, `freetype`: 번들해도 폰트 데이터가 없으면 의미가 없어 제외했다.
* `xkeyboard-config`: 라이브러리가 아니라 `/usr/share/X11/xkb` 데이터다.
  없으면 libxkbcommon 이 키보드를 초기화하지 못한다.

GUI 이므로 X11 디스플레이(또는 X 포워딩)가 필요하다.

세션 D-Bus 가 없으면 `qt.qpa.theme.dbus: Session DBus not running` 경고가 나오지만
실행에는 영향이 없다. `qt.tlsbackend.ossl: Incompatible version of OpenSSL` 도
업데이트 확인에만 쓰이므로 무해하다.

## 검증 방법

### 라이브러리가 없는 머신 재현

사용자 네임스페이스로 시스템 라이브러리를 가려서 대상 머신 상황을 그대로 만들 수 있다.
`syslib/` 번들이 실제로 문제를 해결하는지 이 방법으로 확인했다:

```bash
: > /tmp/empty.so
unshare -rm bash -c '
  mount --bind /tmp/empty.so /lib64/libxcb-cursor.so.0
  # A) syslib 없이 → bm32 와 동일한 오류로 abort
  LD_LIBRARY_PATH=bin/lib:bin/ffmpeg QT_PLUGIN_PATH=bin/plugins ./bin/YUView test.ivf
  # B) 런처로 → 정상 실행
  ./bin/YUView.sh test.ivf
'
```

### 번들 자기충족성

프로젝트 밖으로 복사해 정리된 환경에서 실행하고, 빌드 머신 경로에서 로드되는 것이
없는지 본다:

```bash
cp -r bin /tmp/sim/yuview && cd /tmp/sim
env -i HOME=$HOME DISPLAY=$DISPLAY XAUTHORITY=$HOME/.Xauthority PATH=/usr/bin:/bin \
    ./yuview/YUView.sh test.ivf &
sleep 8
awk '{print $NF}' /proc/$(pgrep -x YUView)/maps | grep '\.so' | sort -u \
    | grep -E "$HOME/Qt|$HOME/opt"      # 출력이 없어야 정상
```
