#!/bin/sh
# Build and run the IrcSupport.cpp tests on the x86-64 build host, against a Yocto qemux86-64
# imlibpurpleservice workdir (after do_configure: it needs recipe-sysroot, with libpurple and
# its protocol plug-ins, and the cross g++ in recipe-sysroot-native). The test loads the
# sysroot's own prpl-irc (purple-2/libirc.so) and signs it in to an IRC server it runs itself on
# 127.0.0.1, so nothing goes out to a real network. It is started through the sysroot's loader,
# so the host's libraries never mix in.
#
#   test/irc/run.sh WORKDIR [OUT_DIR]
#   WORKDIR  e.g. tmp/work/corei7-64-webos-linux/imlibpurpleservice/<version>
#
# Environment: CXX (default: the workdir's x86_64-webos-linux-g++), EXTRA_CXXFLAGS (e.g.
# "-fsanitize=address,undefined" with a compiler that has the runtimes), RUNNER (e.g.
# "valgrind --error-exitcode=1"), IRC_TEST_DEBUG=1 for libpurple's own debug output.
set -eu

WORKDIR=${1:?usage: run.sh WORKDIR [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/build-irc-test}
SYSROOT=${SYSROOT:-$WORKDIR/recipe-sysroot}
CXX=${CXX:-$WORKDIR/recipe-sysroot-native/usr/bin/x86_64-webos-linux/x86_64-webos-linux-g++}

pc() { PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig pkg-config "$@"; }

mkdir -p "$OUT"
"$CXX" --sysroot="$SYSROOT" -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_CXXFLAGS:-} \
	-DPURPLE_PLUGIN_DIR="\"$SYSROOT/usr/lib/purple-2\"" \
	-I"$ROOT/inc" $(pc --cflags purple glib-2.0) \
	"$HERE/irc_test.cpp" "$ROOT/src/IrcSupport.cpp" \
	-L"$SYSROOT/usr/lib" -lgtest -lpthread $(pc --libs purple glib-2.0) \
	-Wl,--dynamic-linker="$SYSROOT/usr/lib/ld-linux-x86-64.so.2" -Wl,--disable-new-dtags -Wl,-rpath,"$SYSROOT/usr/lib:$SYSROOT/lib" \
	-o "$OUT/irc_test"

cd "$OUT"
${RUNNER:-} ./irc_test
