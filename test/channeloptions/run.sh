#!/bin/sh
# Build and run the ChannelOptions.cpp tests on the x86-64 build host, against a Yocto qemux86-64
# imlibpurpleservice workdir (after do_configure: it needs recipe-sysroot, with libpurple, and the
# cross g++ in recipe-sysroot-native). No prpl is loaded and nothing goes out to a network. It is
# started through the sysroot's loader, so the host's libraries never mix in.
#
#   test/channeloptions/run.sh WORKDIR [OUT_DIR]
#   WORKDIR  e.g. tmp/work/corei7-64-webos-linux/imlibpurpleservice/<version>
#
# Environment: CXX (default: the workdir's x86_64-webos-linux-g++), EXTRA_CXXFLAGS (e.g.
# "-fsanitize=address,undefined" with a compiler that has the runtimes), RUNNER (e.g.
# "valgrind --error-exitcode=1"), CHANNELOPTIONS_TEST_DEBUG=1 for libpurple's own debug output.
set -eu

WORKDIR=${1:?usage: run.sh WORKDIR [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/build-channeloptions-test}
SYSROOT=${SYSROOT:-$WORKDIR/recipe-sysroot}
CXX=${CXX:-$WORKDIR/recipe-sysroot-native/usr/bin/x86_64-webos-linux/x86_64-webos-linux-g++}

pc() { PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig pkg-config "$@"; }

mkdir -p "$OUT"
"$CXX" --sysroot="$SYSROOT" -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_CXXFLAGS:-} \
	-I"$ROOT/inc" $(pc --cflags purple glib-2.0) \
	"$HERE/channeloptions_test.cpp" "$ROOT/src/ChannelOptions.cpp" \
	-L"$SYSROOT/usr/lib" -lgtest -lpthread $(pc --libs purple glib-2.0) \
	-Wl,--dynamic-linker="$SYSROOT/usr/lib/ld-linux-x86-64.so.2" -Wl,--disable-new-dtags -Wl,-rpath,"$SYSROOT/usr/lib:$SYSROOT/lib" \
	-o "$OUT/channeloptions_test"

cd "$OUT"
${RUNNER:-} ./channeloptions_test
