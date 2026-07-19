#!/bin/sh
# Transport launch wrapper. The transport binary's ELF interpreter is patched to the
# wpe-glibc loader (/media/internal/wpe-glibc/lib/ld-linux.so.3) so purple-signal's
# in-process JVM works (the wpe-252 glibc build that ld-teams.so.3 uses SIGSEGVs libjvm;
# both are glibc 2.23 but different builds). Put wpe-glibc/lib FIRST so its matching
# libc/pthread/dl/rt load (loader<->libc are build-coupled). libstdc++ preloaded first so
# its operator new wins over libmojocore's weak _Znwj. Self-rotating at 30MB.
#
# SSLFIX: wpe-glibc/lib ships an OLD libcrypto.so.3 (OpenSSL 3.0.16). Because wpe-glibc must be
# FIRST on LD_LIBRARY_PATH (above), it shadows the newer Atlas OpenSSL in wpe-252/lib, so
# wpe-252's libssl.so.3 (built against 3.3.0+) fails to load ("version OPENSSL_3.3.0 not found").
# libpurple then registers NO ssl provider (purple_ssl_is_supported()==FALSE) and purple-facebook
# -- the ONLY prpl using libpurple's native purple_ssl_* (others bring their own TLS) -- can't do
# HTTPS ("Unable to connect to graph.facebook.com: Cancelled" after a ~30s hang). Fix: prepend a
# dir holding ONLY the matched wpe-252 libcrypto+libssl pair, so OpenSSL loads from there while
# libc/pthread/dl/rt still fall through to wpe-glibc (coupling preserved). The dir is
# self-provisioned from wpe-252 below so a re-flash restores it automatically; vfat /media/internal
# cannot hold symlinks, so real copies (refreshed when the Atlas build changes size).
LOG=/media/internal/imstdout.log
SZ=$(wc -c < "$LOG" 2>/dev/null || echo 0)
if [ "$SZ" -gt 31457280 ] 2>/dev/null; then mv -f "$LOG" "$LOG.1" 2>/dev/null; fi
B=/media/cryptofs/apps/usr/palm/applications/com.palm.app.teams/backend/lib
W=/media/cryptofs/apps/usr/palm/applications/org.webosports.app.atlas/deviceroot/wpe-252/lib
G=/media/internal/wpe-glibc
S=/media/internal/sslfix
# Provision/refresh the ssl override dir from the Atlas OpenSSL build (copy if missing or size-changed).
mkdir -p "$S"
for L in libcrypto.so.3 libssl.so.3; do
  if [ "$(wc -c < "$S/$L" 2>/dev/null || echo 0)" != "$(wc -c < "$W/$L" 2>/dev/null || echo x)" ]; then
    cp -f "$W/$L" "$S/$L" 2>/dev/null
  fi
done
exec env \
  LD_PRELOAD="$B/libstdc++.so.6 $G/lib/librt.so.1" \
  LD_LIBRARY_PATH="$S:$G/lib:$B:$W:/usr/lib:/lib" \
  /usr/bin/imlibpurpletransport "$@" >> "$LOG" 2>&1
