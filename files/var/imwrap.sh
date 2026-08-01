#!/bin/sh
# Transport launch wrapper. The transport binary's ELF interpreter is patched to the
# wpe-glibc loader (/media/cryptofs/wpe-glibc/lib/ld-linux.so.3) so purple-signal's
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
# self-provisioned from wpe-252 below so a re-flash restores it automatically; real copies (not
# symlinks) so it's self-contained and refreshed when the Atlas build changes size.
# LOG + the transport's private glibc (G) and ssl-override (S) live on /media/cryptofs, NOT
# /media/internal. /media/internal is the vfat partition exported in USB "drive" mode; the transport
# mmaps its whole glibc + interpreter from G and mmaps S's libcrypto/libssl, and those mappings pin
# the partition so storaged can't unmount it ("USB drive Connection failed"). cryptofs is only
# SUSPENDED (not unmounted) for MSM, so the transport (frozen during the suspend) stops blocking.
# The binary's ELF interpreter is likewise patched to /media/cryptofs/wpe-glibc/lib/ld-linux.so.3
# (build.sh --dynamic-linker). See the usb-drive-mode-media-internal-blockers note.
LOG=/media/cryptofs/imstdout.log
SZ=$(wc -c < "$LOG" 2>/dev/null || echo 0)
if [ "$SZ" -gt 31457280 ] 2>/dev/null; then mv -f "$LOG" "$LOG.1" 2>/dev/null; fi
# libpurple.so + purple-2/ plugins now live at the real /usr/lib(+/purple-2) -- see
# packaging/README.md "why /usr/lib now". RUNTIME holds only the private, non-stock-colliding
# runtime deps (libstdc++/libgcrypt/libpng16/libwebp/libopus/... -- third-party link deps unique
# to specific prpls, kept OUT of /usr/lib so they can't silently replace a system-wide lib version
# other apps rely on) that used to sit alongside the engine under com.palm.app.teams/backend/lib
# for no good reason (that app dir never had anything to do with the shared backend).
RUNTIME=/usr/lib/synergy-runtime
W=/media/cryptofs/apps/usr/palm/applications/org.webosports.app.atlas/deviceroot/wpe-252/lib
G=/media/cryptofs/wpe-glibc
S=/media/cryptofs/sslfix
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

# Also preload the SYSTEM libasound (/usr/lib/libasound.so.2): the in-plugin call bridges
# (WhatsApp glue/call.c, Telegram tdlib-purple) open the "voip"/"voipsource" ALSA PCMs, but the
# Atlas wpe-252 libasound on LD_LIBRARY_PATH can't find its pulse plugin module ("No such device").
# The system libasound has /usr/lib/alsa-lib's pulse module + /etc/asound.conf's voip PCMs — the
# same route the standalone wacallm/Signal engines used. Without this: "audio playback/capture-FAIL".
exec env \
  LD_PRELOAD="$B/libstdc++.so.6 $G/lib/librt.so.1 /usr/lib/libasound.so.2" \
  LD_LIBRARY_PATH="$S:$G/lib:$B:$W:/usr/lib:/lib" \
  /usr/bin/imlibpurpletransport "$@" >> "$LOG" 2>&1
