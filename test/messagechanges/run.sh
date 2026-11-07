#!/bin/sh
# Build and run the MessageChanges.cpp tests on the x86-64 build host, against a Yocto qemux86-64
# imlibpurpleservice workdir (after do_configure: it needs recipe-sysroot, with mojodb, and the
# cross g++ in recipe-sysroot-native). Nothing touches a db: the tests check the merges built for
# one. It is started through the sysroot's loader, so the host's libraries never mix in. The
# mojodb headers build only as the service does, with -fpermissive -DMOJ_LINUX.
#
#   test/messagechanges/run.sh WORKDIR [OUT_DIR]
#   WORKDIR  e.g. tmp/work/corei7-64-webos-linux/imlibpurpleservice/<version>
#
# Environment: CXX (default: the workdir's x86_64-webos-linux-g++), EXTRA_CXXFLAGS (e.g.
# "-fsanitize=address,undefined" with a compiler that has the runtimes), RUNNER (e.g.
# "valgrind --error-exitcode=1").
set -eu

WORKDIR=${1:?usage: run.sh WORKDIR [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/build-messagechanges-test}
SYSROOT=${SYSROOT:-$WORKDIR/recipe-sysroot}
CXX=${CXX:-$WORKDIR/recipe-sysroot-native/usr/bin/x86_64-webos-linux/x86_64-webos-linux-g++}

pc() { PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig pkg-config "$@"; }

mkdir -p "$OUT"
# shellcheck disable=SC2046,SC2086  # pkg-config output and EXTRA_CXXFLAGS are lists of flags
"$CXX" --sysroot="$SYSROOT" -std=c++17 -O1 -g -Wall -Wextra -fpermissive -DMOJ_LINUX ${EXTRA_CXXFLAGS:-} \
	-I"$ROOT/inc" $(pc --cflags mojodb glib-2.0) \
	"$HERE/messagechanges_test.cpp" "$ROOT/src/MessageChanges.cpp" \
	-L"$SYSROOT/usr/lib" -lgtest -lgtest_main -lpthread -lmojocore $(pc --libs glib-2.0) \
	-Wl,--dynamic-linker="$SYSROOT/usr/lib/ld-linux-x86-64.so.2" -Wl,--disable-new-dtags -Wl,-rpath,"$SYSROOT/usr/lib:$SYSROOT/lib" \
	-o "$OUT/messagechanges_test"

cd "$OUT"
${RUNNER:-} ./messagechanges_test
