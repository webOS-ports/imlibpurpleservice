#!/bin/sh
# Transport launch wrapper. The transport binary's ELF interpreter is patched to the
# wpe-glibc loader (/media/internal/wpe-glibc/lib/ld-linux.so.3) so purple-signal's
# in-process JVM works (the wpe-252 glibc build that ld-teams.so.3 uses SIGSEGVs libjvm;
# both are glibc 2.23 but different builds). Put wpe-glibc/lib FIRST so its matching
# libc/pthread/dl/rt load (loader<->libc are build-coupled). libstdc++ preloaded first so
# its operator new wins over libmojocore's weak _Znwj. Self-rotating at 30MB.
LOG=/media/internal/imstdout.log
SZ=$(wc -c < "$LOG" 2>/dev/null || echo 0)
if [ "$SZ" -gt 31457280 ] 2>/dev/null; then mv -f "$LOG" "$LOG.1" 2>/dev/null; fi
B=/media/cryptofs/apps/usr/palm/applications/com.palm.app.teams/backend/lib
W=/media/cryptofs/apps/usr/palm/applications/org.webosports.app.atlas/deviceroot/wpe-252/lib
G=/media/internal/wpe-glibc
exec env \
  LD_PRELOAD="$B/libstdc++.so.6 $G/lib/librt.so.1" \
  LD_LIBRARY_PATH="$G/lib:$B:$W:/usr/lib:/lib" \
  /usr/bin/imlibpurpletransport "$@" >> "$LOG" 2>&1
