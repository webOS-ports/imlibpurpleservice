#!/bin/sh
# Build and run the VoiceNote.cpp tests (M4A voice recordings -> Ogg/Opus) on the x86-64 build host,
# against a Yocto qemux86-64 imlibpurpleservice workdir (after do_configure, with gstreamer1.0 and
# gstreamer1.0-plugins-base in DEPENDS). The test makes its M4A and decodes the result through
# GStreamer, so it also needs the plugins: coreelements, audiotestsrc, audioconvert, audioresample,
# app, ogg, opus (base), isomp4, audioparsers (good) and avdec_aac/avenc_aac (libav). Point
# GST_PLUGINS at a folder holding them; by default the workdir's sysroot is looked in.
#
#   test/voicenote/run.sh WORKDIR [OUT_DIR]
#   WORKDIR  e.g. tmp/work/corei7-64-webos-linux/imlibpurpleservice/<version>
#
# Environment: CXX (default: the workdir's x86_64-webos-linux-g++), EXTRA_CXXFLAGS (e.g.
# "-fsanitize=address,undefined" with a compiler that has the runtimes), SYSROOT (default:
# WORKDIR/recipe-sysroot), GST_PLUGINS (default: SYSROOT/usr/lib/gstreamer-1.0), RUNNER (e.g.
# "valgrind --error-exitcode=1").
set -eu

WORKDIR=${1:?usage: run.sh WORKDIR [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-$ROOT/build-voicenote-test}
SYSROOT=${SYSROOT:-$WORKDIR/recipe-sysroot}
CXX=${CXX:-$WORKDIR/recipe-sysroot-native/usr/bin/x86_64-webos-linux/x86_64-webos-linux-g++}
GST_PLUGINS=${GST_PLUGINS:-$SYSROOT/usr/lib/gstreamer-1.0}

pc() { PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig pkg-config "$@"; }

mkdir -p "$OUT"
"$CXX" --sysroot="$SYSROOT" -std=c++17 -O1 -g -Wall -Wextra -DHAVE_GSTREAMER ${EXTRA_CXXFLAGS:-} \
	-I"$ROOT/inc" $(pc --cflags gstreamer-1.0 gstreamer-app-1.0 opus ogg) \
	"$HERE/voicenote_test.cpp" "$ROOT/src/VoiceNote.cpp" "$ROOT/src/OpusEncoder.cpp" \
	-L"$SYSROOT/usr/lib" -lgtest_main -lgtest $(pc --libs gstreamer-1.0 gstreamer-app-1.0) -lopus -logg -lpthread \
	-Wl,--dynamic-linker="$SYSROOT/usr/lib/ld-linux-x86-64.so.2" -Wl,--disable-new-dtags -Wl,-rpath,"$SYSROOT/usr/lib:$SYSROOT/lib" \
	-o "$OUT/voicenote_test"

cd "$OUT"
# Only the sysroot's plugins, with a registry of their own, so the host's GStreamer never mixes in.
# Plugins load in this process: the sysroot has no gst-plugin-scanner.
GST_PLUGIN_SYSTEM_PATH_1_0=$GST_PLUGINS GST_PLUGIN_PATH_1_0= GST_REGISTRY_1_0=$OUT/gst-registry.bin \
GST_REGISTRY_FORK=no \
	${RUNNER:-} ./voicenote_test
