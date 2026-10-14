#!/usr/bin/env bash
# OpenCV static install for the bd-analyzer C++ app (optional dependency)
#
#   - Static (.a, PIC) on purpose: the libraries are absorbed into the YUView executable, so the
#     AppImage/RPM bundle (make-bin-bundle.sh) gains no new .so. The only extra runtime dependency
#     is the system libz, which YUView already loads.
#   - Modules: core, imgproc, features2d, video, flann, calib3d. calib3d is what gives RANSAC
#     global motion (estimateAffinePartial2D, estimateAffine2D, findHomography); it needs flann.
#     calib3d uses OpenCV's own solver here (LAPACK/Eigen off), so it adds no external library.
#   - highgui/videoio/Qt/GTK/FFmpeg/OpenCL/IPP are off: the app has its own Qt 6.5.3 and loads
#     FFmpeg 7.1 with dlopen, and a second copy of either inside OpenCV would collide.
#   - Built with the app's toolchain (gcc-toolset-13) so the libstdc++ ABI matches.
#   - No sudo needed: dnf only has opencv-devel 3.4.6 (powertools), which also needs root.
#   - scripts/build.sh passes the prefix to qmake as BDA_OPENCV_DIR. When it is missing, the build
#     still works and code guarded by BDA_HAVE_OPENCV is compiled out.
set -euo pipefail

OPENCV_VERSION="${OPENCV_VERSION:-4.14.0}"
OPENCV_SHA256="ee8fb9b30eb60850431b4656447080e3737b56e45719c92b67f245950609f86e"
OPENCV_MODULES="${OPENCV_MODULES:-core,imgproc,features2d,video,flann,calib3d}"

PREFIX="${PREFIX:-$HOME/opt/opencv-${OPENCV_VERSION%.*}}"
WORK="${WORK:-${TMPDIR:-/tmp}/bd-opencv-build}"
TOOLSET="${TOOLSET:-gcc-toolset-13}"
JOBS="${JOBS:-$(nproc)}"

# An older install with fewer modules is rebuilt; YUViewApp.pro links every module listed above.
missing=0
for m in ${OPENCV_MODULES//,/ }; do
    [[ -e "$PREFIX/lib64/libopencv_$m.a" ]] || missing=1
done
if [[ $missing == 0 ]]; then
    echo "already installed: $PREFIX"
    exit 0
fi

mkdir -p "$WORK"
cd "$WORK"
tarball="opencv-$OPENCV_VERSION.tar.gz"
[[ -f $tarball ]] || \
    curl -sSLo "$tarball" "https://github.com/opencv/opencv/archive/refs/tags/$OPENCV_VERSION.tar.gz"
if [[ $OPENCV_VERSION == 4.14.0 ]]; then
    echo "$OPENCV_SHA256  $tarball" | sha256sum -c -
fi
rm -rf "opencv-$OPENCV_VERSION" build && tar xf "$tarball"

scl enable "$TOOLSET" -- bash -c "
    set -euo pipefail
    cmake -S 'opencv-$OPENCV_VERSION' -B build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX='$PREFIX' \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_LIST='$OPENCV_MODULES' \
        -DWITH_QT=OFF -DWITH_GTK=OFF -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF \
        -DWITH_OPENCL=OFF -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_1394=OFF -DWITH_V4L=OFF \
        -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF -DWITH_OPENEXR=OFF \
        -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
        -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF \
        -DOPENCV_GENERATE_PKGCONFIG=ON >/dev/null
    cmake --build build -j'$JOBS' >/dev/null
    cmake --install build >/dev/null
"

cat <<EOF

done.

  installed : $PREFIX
  modules   : $OPENCV_MODULES
  next      : ./scripts/build.sh   (qmake prints "OpenCV: ..." when it picks this up)
EOF
