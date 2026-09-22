#!/usr/bin/env bash
# Build the libraries the libretro core links into a prefix, as static,
# position independent archives, so that the core carries them inside it.
#
#   ui/libretro/build-deps.sh android <prefix>   # needs ANDROID_NDK_ROOT
#   ui/libretro/build-deps.sh linux   <prefix>
#
# A frontend loads the core into its own process, where nothing can be assumed
# about which shared libraries are installed or what they are called: libpcap
# is libpcap.so.0.8 on Debian and libpcap.so.1 everywhere else, and libslirp
# comes with QEMU or not at all. On Android there is no system copy of any of
# them. glib, pixman, libsamplerate, libslirp, libpcap and, for Linux, libepoxy
# are therefore built from source here; what is left dynamic is the platform
# (libc, libGL, libvulkan) and whatever SDL and epoxy dlopen at run time.
#
# The prefix has no shared libraries in it, and pkg-config has to be asked for
# --static when it is read (see pkg-config-static below): the private
# dependencies of a static glib (pcre2, libffi) are otherwise left off the link
# line, and a shared module links with undefined symbols without complaint --
# the failure would come when a frontend loads the core.
#
# Needs meson >= 1.4, ninja, cmake, pkg-config and curl on PATH.
set -euo pipefail

host="${1:?usage: build-deps.sh android|linux <prefix>}"
prefix="$(realpath -m "${2:?usage: build-deps.sh android|linux <prefix>}")"
work="${DEPS_WORK:-${prefix}.build}"
mkdir -p "$prefix/lib/pkgconfig" "$work"
jobs="$(nproc 2>/dev/null || echo 4)"

case "$host" in
android)
    : "${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT is not set}"
    api="${ANDROID_API:-29}"
    tc="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
    cat > "$work/cross.txt" <<EOF
[binaries]
c = '$tc/aarch64-linux-android$api-clang'
cpp = '$tc/aarch64-linux-android$api-clang++'
ar = '$tc/llvm-ar'
strip = '$tc/llvm-strip'
ranlib = '$tc/llvm-ranlib'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']

[properties]
pkg_config_libdir = '$prefix/lib/pkgconfig'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
    meson_host=(--cross-file "$work/cross.txt")
    cmake_host=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
                -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$api")
    ;;
linux)
    meson_host=(-Dc_args=-fPIC -Dcpp_args=-fPIC)
    cmake_host=()
    ;;
*)
    echo "unknown host $host" >&2
    exit 1
    ;;
esac

export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH=""

meson_common=(--prefix "$prefix" --libdir lib --buildtype release
              --default-library static -Db_staticpic=true)
cmake_common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
              -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON
              -DBUILD_SHARED_LIBS=OFF)

fetch() { # url dir
    if [ ! -d "$work/$2" ]; then
        curl -fsSL "$1" -o "$work/$2.tar"
        mkdir "$work/$2"
        tar -xf "$work/$2.tar" -C "$work/$2" --strip-components=1
        rm "$work/$2.tar"
    fi
    cd "$work/$2"
    rm -rf _build
}

have() { test -f "$prefix/lib/pkgconfig/$1.pc"; }

# glib pulls libffi, pcre2 and (on Android) proxy-libintl in as subprojects
if ! have glib-2.0; then
    fetch https://download.gnome.org/sources/glib/2.82/glib-2.82.5.tar.xz glib
    meson setup _build "${meson_host[@]}" "${meson_common[@]}" \
        --wrap-mode=forcefallback -Dtests=false -Dglib_debug=disabled \
        -Dintrospection=disabled -Dnls=disabled -Dselinux=disabled \
        -Dxattr=false -Dlibmount=disabled -Dman-pages=disabled \
        -Ddocumentation=false -Dlibelf=disabled -Dsysprof=disabled
    ninja -C _build -j"$jobs" install
fi

if ! have pixman-1; then
    fetch https://cairographics.org/releases/pixman-0.44.2.tar.gz pixman
    meson setup _build "${meson_host[@]}" "${meson_common[@]}" \
        -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
        -Dopenmp=disabled -Da64-neon=disabled
    ninja -C _build -j"$jobs" install
fi

if ! have samplerate; then
    fetch https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz samplerate
    cmake -S . -B _build "${cmake_host[@]}" "${cmake_common[@]}" \
        -DBUILD_TESTING=OFF -DLIBSAMPLERATE_EXAMPLES=OFF -DLIBSAMPLERATE_INSTALL=ON
    ninja -C _build -j"$jobs" install
fi

if ! have slirp; then
    fetch https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.0/libslirp-v4.9.0.tar.gz slirp
    meson setup _build "${meson_host[@]}" "${meson_common[@]}"
    ninja -C _build -j"$jobs" install
fi

# The pcap netdev is not optional in xemu's QAPI schema
if ! have libpcap; then
    fetch https://www.tcpdump.org/release/libpcap-1.10.5.tar.xz pcap
    cmake -S . -B _build "${cmake_host[@]}" "${cmake_common[@]}" \
        -DDISABLE_DBUS=ON -DDISABLE_RDMA=ON -DDISABLE_BLUETOOTH=ON \
        -DDISABLE_NETMAP=ON -DDISABLE_DPDK=ON -DBUILD_WITH_LIBNL=OFF \
        -DDISABLE_LINUX_USBMON=ON -DENABLE_REMOTE=OFF
    ninja -C _build -j"$jobs" install
    # A static-only install still ships libpcap.so on some versions
    rm -f "$prefix"/lib/libpcap.so*
fi

if [ "$host" = android ]; then
    # Only its headers and platform independent helpers get used: there is
    # no SDL video on Android
    if ! have sdl3; then
        fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz sdl3
        cmake -S . -B _build "${cmake_host[@]}" "${cmake_common[@]}" \
            -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
            -DSDL_CAMERA=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF -DSDL_HAPTIC=OFF \
            -DSDL_SENSOR=OFF -DSDL_POWER=OFF -DSDL_TRAY=OFF
        ninja -C _build -j"$jobs" install
    fi
else
    # epoxy resolves GL with dlopen, so a static copy adds no dependency. On
    # Linux, SDL3 is left to xemu's own subproject.
    if ! have epoxy; then
        fetch https://github.com/anholt/libepoxy/archive/refs/tags/1.5.10.tar.gz epoxy
        meson setup _build "${meson_host[@]}" "${meson_common[@]}" \
            -Dtests=false -Ddocs=false -Dglx=yes -Degl=yes -Dx11=true
        ninja -C _build -j"$jobs" install
    fi
fi

# meson reads PKG_CONFIG as a single program, so --static goes in a wrapper
cat > "$prefix/pkg-config-static" <<'EOF'
#!/bin/sh
exec pkg-config --static "$@"
EOF
chmod +x "$prefix/pkg-config-static"

rm -f "$prefix"/lib/*.so "$prefix"/lib/*.so.*
ls "$prefix/lib/pkgconfig"
