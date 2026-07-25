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
# ENTROPY: this 2.6.35 kernel has no getrandom(2) syscall, so crypto that seeds from the OS
# (SQLCipher/OpenSSL/ring inside purple-presage) falls back to BLOCKING /dev/random. With the tiny
# entropy pool (~130 bits) that read stalls during dlopen, so the presage plugin load HANGS and the
# whole transport looks dead -- intermittently, depending on how much entropy exists at respawn time
# (this is why presage "loaded at boot but crashed on a later respawn"). Point /dev/random at the
# non-blocking /dev/urandom (cryptographically fine post-boot). /dev is a tmpfs, so re-apply each launch.
if [ ! -L /dev/random ]; then
  rm -f /dev/random && ln -s /dev/urandom /dev/random 2>/dev/null
fi

# SELF-HEAL the PmLog init semaphore before EVERY transport launch. libPmLogLib takes a one-time init
# lock on /dev/shm/sem.PmLogLib; if a transport was killed mid-init (kill -9, OR reaped during the boot
# ordering) the sem stays LOCKED and the next transport hangs forever on its first PmLog call -- 1
# thread, no log, no accounts ("no messages come in"). This lives in imwrap.sh (not just imdaemon.sh)
# because BOTH launch paths exec here: the upstart resident daemon (imdaemon.sh) AND the on-demand LS2
# .service. Unlinking is harmless -- sem_open recreates a fresh unlocked one; other holders keep theirs.
# See the imtransport-pmlog-sem-hang note.
rm -f /dev/shm/sem.PmLogLib

exec env \
  LD_PRELOAD="$B/libstdc++.so.6 $G/lib/librt.so.1" \
  LD_LIBRARY_PATH="$S:$G/lib:$B:$W:/usr/lib:/lib" \
  /usr/bin/imlibpurpletransport "$@" >> "$LOG" 2>&1
