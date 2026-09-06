%global bda_home /opt/bd-analyzer

# The payload is a prebuilt bundle. Ship it exactly as staged: no stripping, no
# debuginfo extraction (the bundled Qt/FFmpeg libraries are not ours to strip).
%global debug_package %{nil}
%global __os_install_post %{nil}
# Build-id symlinks under /usr/lib/.build-id are generated at packaging time, independently
# of __os_install_post. The bundled Qt/FFmpeg libraries carry the build-ids of their upstream
# builds, so emitting those links invites file conflicts with any other package shipping the
# same binaries. We ship no debuginfo, so the links have no consumer either.
%global _build_id_links none

# The bundled Qt / FFmpeg / ICU / X11 libraries are private to this prefix. Do not
# advertise them to the rest of the system as if they were a system Qt install.
%global __provides_exclude_from ^%{bda_home}/.*$

# ...and do not let the auto-dependency generator turn them back into Requires.
# Everything else it finds from the payload ELF files (glibc, libstdc++, libGL,
# glib2, fontconfig, openssl-libs, ...) is a genuine target dependency and is kept -
# that auto-generated list is the whole point of shipping an RPM instead of a folder.
# [.] instead of \. avoids a second round of backslash escaping through rpm macros.
%global __requires_exclude_bundled libQt6[A-Za-z]+[.]so.*|libicu(data|i18n|uc)[.]so.*|libav(codec|format|util)[.]so.*|libswresample[.]so.*|libdav1d-internals[.]so.*|libX11(-xcb)?[.]so.*|libxcb[A-Za-z0-9_-]*[.]so.*|libXau[.]so.*|libXdmcp[.]so.*|libXext[.]so.*|libXrender[.]so.*|libxkbcommon(-x11)?[.]so.*

# The optional Qt plugins we ship must not become hard dependencies. Qt probes them at
# startup and silently skips any whose libraries are missing, so requiring these would
# drag the GTK3 and Wayland stacks onto every target for no functional gain:
#   platformthemes/libqgtk3.so        -> gtk3, gdk-pixbuf, pango, cairo, atk
#   platforms/libqwayland-*.so        -> libwayland-* (we run on xcb)
#   platforms/libqlinuxfb.so          -> libdrm
# gtk3 is listed as Recommends instead, so desktops that have it get the native theme.
%global __requires_exclude_optional libgtk-3[.]so.*|libgdk-3[.]so.*|libgdk_pixbuf-2[.]0[.]so.*|libatk-1[.]0[.]so.*|libpango(cairo)?-1[.]0[.]so.*|libcairo(-gobject)?[.]so.*|libwayland-(client|cursor|egl)[.]so.*|libdrm[.]so.*

%global __requires_exclude ^(%{__requires_exclude_bundled}|%{__requires_exclude_optional})$

Name:           bd-analyzer
Version:        %{?bda_version}%{!?bda_version:0.2.0}
Release:        %{?bda_release}%{!?bda_release:1}%{?dist}
Summary:        Video bitstream and YUV analysis toolset
Summary(ko):    비디오 비트스트림·YUV 분석 도구

License:        GPL-3.0-or-later
URL:            https://github.com/IENT/YUView
Source0:        %{name}-%{version}-linux-x86_64.tar.gz

ExclusiveArch:  x86_64

# Runtime needs the auto-dependency generator cannot see, because they are either
# data rather than libraries, or loaded through a driver indirection.
# xkeyboard-config  - /usr/share/X11/xkb data. Without it libxkbcommon cannot
#                     initialise the keyboard and the xcb plugin aborts.
# mesa-dri-drivers  - the GL driver behind libGL.so.1, including the swrast
#                     fallback used over X forwarding.
# dejavu-sans-fonts - font data. fontconfig with no fonts installed renders nothing.
Requires:       xkeyboard-config
Requires:       mesa-dri-drivers
Requires:       dejavu-sans-fonts
Requires:       hicolor-icon-theme
Requires:       bash

# Native GTK3 theme when the target is a full desktop; skipped cleanly otherwise.
Recommends:     gtk3

%description
bd_analyzer is a fork of IENT/YUView extended for codec development and
verification: AV1/H.264/HEVC bitstream inspection, YUV analysis with PSNR/SSIM
metrics and scopes, YUV test pattern generation and image filters.

This package bundles Qt 6.5.3, FFmpeg 7.1 and the dav1d analyzer decoder under
%{bda_home}, so no Qt installation is required on the target machine.

%description -l ko
bd_analyzer 는 코덱 개발·검증용으로 확장한 IENT/YUView fork 입니다.
AV1/H.264/HEVC 비트스트림 분석, PSNR/SSIM 및 스코프를 포함한 YUV 분석,
YUV 테스트 패턴 생성, 이미지 필터를 제공합니다.

Qt 6.5.3, FFmpeg 7.1, dav1d analyzer 디코더가 %{bda_home} 아래에 번들되어
있으므로 대상 머신에 Qt 를 설치하지 않아도 됩니다.

%prep
%setup -q -n %{name}-%{version}

%build
# Nothing to build: the payload is produced by scripts/build.sh + package.sh on the
# build machine, where gcc-toolset-13 and Qt 6.5.3 are available.

%install
install -d %{buildroot}%{bda_home}
cp -a YUView bd-analyzer qt.conf check-deps.sh \
      lib syslib ffmpeg plugins decoder %{buildroot}%{bda_home}/

# The binary is reached through the launcher, never directly - see packaging/common/bd-analyzer.
install -d %{buildroot}%{_bindir}
ln -s %{bda_home}/bd-analyzer %{buildroot}%{_bindir}/%{name}

install -Dm644 share/applications/%{name}.desktop \
               %{buildroot}%{_datadir}/applications/%{name}.desktop
for size in 32 64 128 256 512; do
    install -Dm644 share/icons/${size}/%{name}.png \
                   %{buildroot}%{_datadir}/icons/hicolor/${size}x${size}/apps/%{name}.png
done

%post
[ -x %{_bindir}/update-desktop-database ] && \
    %{_bindir}/update-desktop-database %{_datadir}/applications &>/dev/null || :
[ -x %{_bindir}/gtk-update-icon-cache ] && \
    %{_bindir}/gtk-update-icon-cache -q -t -f %{_datadir}/icons/hicolor &>/dev/null || :

%postun
if [ $1 -eq 0 ]; then
    [ -x %{_bindir}/update-desktop-database ] && \
        %{_bindir}/update-desktop-database %{_datadir}/applications &>/dev/null || :
    [ -x %{_bindir}/gtk-update-icon-cache ] && \
        %{_bindir}/gtk-update-icon-cache -q -t -f %{_datadir}/icons/hicolor &>/dev/null || :
fi

%files
%license LICENSE.GPL3
%doc README.md
%dir %{bda_home}
%{bda_home}/YUView
%{bda_home}/bd-analyzer
%{bda_home}/check-deps.sh
%{bda_home}/qt.conf
%{bda_home}/lib
%{bda_home}/syslib
%{bda_home}/ffmpeg
%{bda_home}/plugins
%{bda_home}/decoder
%{_bindir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png

%changelog
* Sun Sep 06 2026 knight.kim <knight.kim@blue-dot.io> - 0.2.0-1
- Motion estimation analysis: SVT-AV1 integer ME and odyssey open-loop ME reproduced,
  drawn through the statistics overlay and readable per block in the Block Info pane.
  Works on raw YUV items and on compressed streams with an original YUV attached.
- The playlist is kept across sessions, with a File menu switch, and no longer collects
  duplicates of the same file.
- Fixes: abort on quit, abort when switching items with the overlay on, a raw YUV whose
  name carries no resolution, and Y4M originals being read at the wrong offsets.

* Wed Aug 20 2026 knight.kim <knight.kim@blue-dot.io> - 0.1.0-1
- First packaged release: RPM and tar.gz built from the bin/ deployment bundle.
