#!/bin/sh
# Keep the IM transport resident (the imtransport upstart job runs this) so it isn't idled out and
# can receive pushed messages.
#
# This does NOT launch the transport: it asks ls-hubd to, by calling com.palm.imlibpurple, and the
# hub starts it through the LS2 .service (com.palm.imlibpurple.service -> imwrap.sh). With ls-hubd
# as the only launcher there is no race between this job and the hub's own on-demand activation:
# before, both launched it, and whichever lost at boot (anything calling com.palm.imlibpurple before
# this job is up starts an on-demand copy, e.g. the loginstate activities) either exited and burned
# upstart's respawns, or waited behind the other copy with a heuristic watchdog for one stuck in
# startup. Now there is one copy, and a stuck startup is plain to see: it never answers on the bus.
#
# RESIDENT_FLAG tells the transport it is kept resident, so it does NOT self-terminate on idle
# (IMServiceHandler::OkToShutdown). Without it the transport's idle self-shutdown and this job
# would fight in a ~30s churn loop that reloads every prpl (incl. purple-signal's JVM) each cycle and
# never lets slow accounts connect. A hub launch can't carry an environment variable, hence the file;
# the on-demand path without this job still idles out as before.
#
# Logging stays at "info" (the level is in the .service Exec line): debug-level logging streams
# every libpurple prpl_debug_misc line to imstdout.log at tens of MB/hour. To debug a connector, edit
# the .service line temporarily; for libpurple's prpl debug also add: export IM_PURPLE_DEBUG=1
LOG=/media/cryptofs/imstdout.log
RESIDENT_FLAG=/var/run/imlibpurple.resident
ARGS='{"log":{"appender":{"type":"stdout"},"levels":{"imlibpurple":"info"}}}'
log() { echo "$(date -Iseconds 2>/dev/null) imdaemon.sh: $*" >> "$LOG"; }

# The transport process proper: libpurple forks helpers (DNS lookups) that share its name.
transport_pid() {
    all=" $(pidof imlibpurpletransport) "
    for p in $all; do
        case "$all" in *" $(cut -d' ' -f4 /proc/$p/stat 2>/dev/null) "*) ;; *) echo $p; return;; esac
    done
}

# Calls the transport, which has ls-hubd start it if it isn't running; waits up to $1 seconds.
# Prints "up" when it answered (an "Unknown method" reply is an answer: it is on the bus),
# "denied" when its role doesn't accept this call, and "silent" when there was no answer.
ask() {
    R=/tmp/imdaemon.ask.$$
    luna-send -n 1 -m com.palm.lunasend palm://com.palm.imlibpurple/keepalive '{}' </dev/null >$R 2>&1 &
    lp=$!; i=0
    while [ $i -lt $1 ] && kill -0 $lp 2>/dev/null; do sleep 1; i=$((i+1)); done
    kill $lp 2>/dev/null
    if grep -q 'Not permitted' $R; then echo denied
    elif grep -q '"returnValue"' $R; then echo up
    else echo silent; fi
    rm -f $R
}

# SELF-HEAL the PmLog init semaphore. libPmLogLib opens /dev/shm/sem.PmLogLib and takes a one-time
# init lock on it; if a transport is killed mid-init (a kill -9 on a running system, OR the process
# being reaped during the boot ordering) the sem stays LOCKED, and every subsequent transport then
# blocks forever on its first PmLog call -- 1 thread, no log output, no LS2 hub connect, no accounts,
# "no messages come in", and upstart eventually hits its respawn limit. Already-running services that
# inited PmLog earlier are unaffected, so ONLY the (re)started transport hangs. A tmpfs /dev/shm is
# cleared on boot, but the corruption also happens DURING boot, so clear it on every launch: unlinking
# the name is harmless (sem_open recreates a fresh, unlocked one; other holders keep their handle).
# See the imtransport-pmlog-sem-hang note. imwrap.sh does the same before every launch.
rm -f /dev/shm/sem.PmLogLib

touch "$RESIDENT_FLAG"
quick=0
while :; do
    case "$(ask 60)" in
    up)
        p=$(transport_pid)
        started=$(cut -d. -f1 /proc/uptime)
        while [ -n "$p" ] && kill -0 "$p" 2>/dev/null; do sleep 5; done
        lived=$(( $(cut -d. -f1 /proc/uptime) - started ))
        log "transport ${p:-?} exited after ${lived}s; starting it again"
        # a transport that keeps dying at once: back off rather than spin
        if [ "$lived" -lt 30 ]; then quick=$((quick+1)); else quick=0; fi
        if [ $quick -ge 5 ]; then log "transport keeps exiting at once; waiting 60s"; sleep 60; quick=0; fi
        sleep 2
        ;;
    silent)
        # Started but never got onto the bus: stuck in startup (see the semaphore note above).
        p=$(transport_pid)
        log "transport ${p:-(none running)} did not answer on the bus within 60s; restarting it"
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
        rm -f /dev/shm/sem.PmLogLib
        sleep 2
        ;;
    denied)
        # The transport's role doesn't accept com.palm.lunasend (an older role file): launch it
        # directly as before, once any on-demand copy has gone (imwrap.sh's lock would refuse us).
        log "com.palm.imlibpurple does not accept com.palm.lunasend; launching the transport directly"
        export IM_RESIDENT=1
        p=$(transport_pid)
        while [ -n "$p" ]; do
            while kill -0 "$p" 2>/dev/null; do sleep 5; done
            p=$(transport_pid)
        done
        exec /var/imwrap.sh -c "$ARGS" PalmPre Palm-Pre/1.5
        ;;
    esac
done
