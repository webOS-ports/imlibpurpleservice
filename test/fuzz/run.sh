#!/bin/bash
# Build and run the host ASan/UBSan harness for the transport's leaf text helpers.
#
# Host-only: nothing here cross-compiles or touches the device. The point is that
# sanitize.cpp and entities.cpp depend on glib and libtidy but NOT on libpurple, db8 or
# luna-service2, so they can be compiled natively and instrumented -- which the ARM
# toolchain cannot do at all (crosstool-NG ships no libasan/libubsan).
#
#   ./run.sh          build + run the fixed corpus
#   ./run.sh fuzz     build with libFuzzer instead and fuzz until interrupted (needs clang)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../..                       # repo root
BUILD=${BUILD:-$HERE/build}           # gitignored
mkdir -p "$BUILD"

# libtidy: prefer a system one, else build the tidy-html5 tree the ARM build already uses.
# CMAKE_POLICY_VERSION_MINIMUM is needed because that tree predates CMake 4's floor.
if pkg-config --exists tidy 2>/dev/null; then
  TIDY_CFLAGS=$(pkg-config --cflags tidy)
  TIDY_LIBS=$(pkg-config --libs tidy)
else
  TIDY_SRC=${TIDY_SRC:-$SRC/../webos-synergy-revival/build-output/tidy-arm/tidy-html5}
  TIDY_PREFIX=$BUILD/tidy-host/install
  if [ ! -f "$TIDY_PREFIX/lib/libtidy.a" ]; then
    [ -d "$TIDY_SRC" ] || { echo "!! no system libtidy and no tidy source at $TIDY_SRC"; \
      echo "   install libtidy-dev, or set TIDY_SRC=/path/to/tidy-html5"; exit 1; }
    echo "== building libtidy for the host (once)"
    cmake -S "$TIDY_SRC" -B "$BUILD/tidy-host" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIB=OFF \
          -DCMAKE_INSTALL_PREFIX="$TIDY_PREFIX" > "$BUILD/tidy.log" 2>&1
    cmake --build "$BUILD/tidy-host" -j"$(nproc)" >> "$BUILD/tidy.log" 2>&1
    cmake --install "$BUILD/tidy-host" >> "$BUILD/tidy.log" 2>&1
  fi
  TIDY_CFLAGS="-I$TIDY_PREFIX/include"
  TIDY_LIBS="$TIDY_PREFIX/lib/libtidy.a"
fi

# -fno-omit-frame-pointer keeps ASan's stack traces readable; -O1 keeps them accurate.
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
INCLUDES="-I$SRC/inc -I$HERE/compat $TIDY_CFLAGS $(pkg-config --cflags glib-2.0)"
LIBS="$TIDY_LIBS $(pkg-config --libs glib-2.0)"
SOURCES="$SRC/src/sanitize.cpp $SRC/src/entities.cpp $HERE/harness.cpp"

if [ "$1" = "fuzz" ]; then
  command -v clang++ >/dev/null || { echo "!! libFuzzer mode needs clang++"; exit 1; }
  echo "== building libFuzzer target"
  # shellcheck disable=SC2086
  clang++ -std=c++17 -g -O1 $SAN -fsanitize=fuzzer -DFUZZ_ENTRY $INCLUDES \
      $SOURCES -o "$BUILD/fuzz" $LIBS
  mkdir -p "$BUILD/corpus"
  shift                              # drop "fuzz"; forward the rest (-max_total_time=60, ...)
  exec "$BUILD/fuzz" "$BUILD/corpus" -max_len=4096 "$@"
fi

echo "== building corpus harness (ASan + UBSan)"
# shellcheck disable=SC2086
g++ -std=c++17 -g -O1 $SAN $INCLUDES $SOURCES -o "$BUILD/harness" $LIBS

echo "== running"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
UBSAN_OPTIONS=print_stacktrace=1 \
exec "$BUILD/harness"
