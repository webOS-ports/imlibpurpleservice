#!/bin/sh
# Build and run the ChatMembers.cpp test (Group Info: the getChatMembers reply) on the x86-64
# build host, against a Yocto qemux86-64 imlibpurpleservice workdir (after do_configure: it needs
# recipe-sysroot and the cross g++ in recipe-sysroot-native). Linked against the sysroot's db8 and
# gtest and started through the sysroot's own loader, as test/sendextras is.
#
#   test/chatmembers/run.sh WORKDIR [OUT_DIR]
#   WORKDIR  e.g. tmp/work/corei7-64-webos-linux/imlibpurpleservice/<version>
#
# Environment: CXX, EXTRA_CXXFLAGS (e.g. "-fsanitize=address,undefined"), RUNNER (e.g.
# "valgrind --error-exitcode=1"), as for test/sendextras/run.sh.
set -eu

WORKDIR=${1:?usage: run.sh WORKDIR [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/build-chatmembers-test}
SYSROOT=$WORKDIR/recipe-sysroot
CXX=${CXX:-$WORKDIR/recipe-sysroot-native/usr/bin/x86_64-webos-linux/x86_64-webos-linux-g++}

mkdir -p "$OUT"
# The db8 headers need what the service itself is built with (CMakeLists.txt): -fpermissive,
# MOJ_LINUX, no RTTI.
"$CXX" --sysroot="$SYSROOT" -std=c++17 -O1 -g -Wall -Wextra -fpermissive -fno-rtti \
	-DMOJ_LINUX -DUSE_PMLOG_DECLARATION ${EXTRA_CXXFLAGS:-} \
	-I"$ROOT/inc" -I"$SYSROOT/usr/include/mojodb" \
	$(PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig pkg-config --cflags glib-2.0) \
	"$HERE/chatmembers_test.cpp" "$ROOT/src/ChatMembers.cpp" \
	-L"$SYSROOT/usr/lib" -lgtest_main -lgtest -lmojocore -lglib-2.0 -lpthread \
	-Wl,--dynamic-linker="$SYSROOT/usr/lib/ld-linux-x86-64.so.2" -Wl,-rpath,"$SYSROOT/usr/lib:$SYSROOT/lib" \
	-o "$OUT/chatmembers_test"
LD_LIBRARY_PATH="$SYSROOT/usr/lib:$SYSROOT/lib" ${RUNNER:-} "$OUT/chatmembers_test"
