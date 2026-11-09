#!/bin/sh
# provision-im-activities.sh
#
# (Re)create the com.palm.imlibpurple ACTIVITY-MANAGER activities from the on-disk Configurator
# configs in /etc/palm/activities/com.palm.imlibpurple/. Without these, the IM transport is never
# woken to drain its outbound queue, so NOTHING SENDS: com.palm.immessage.libpurple rows sit
# folder=outbox / status=pending forever and com.palm.imcommand.libpurple (reactions etc.) sit
# status=pending -- the transport's sendIM / sendCommand callbacks never fire. Incoming still works
# (that path doesn't use these activities), which makes it look like a Teams/plugin bug.
#
# WHY they go missing: a device "Erase Apps & Data" reset wipes the activity-manager DB, and the
# stock Configurator does NOT re-provision these (unchanged, 2011-dated) configs on the next boot.
# They also stop being armed when the transport is restarted while one fires (the activity stays
# "expired" with no one to complete it).
#
# WHO creates them: the transport itself. Since "Transport: Arm the DB watches again when it
# starts", imlibpurpleservice sends these same files to activitymanager/create with
# "replace": true each time it starts. Created that way the creator is
# {"serviceId":"com.palm.imlibpurple"}, the one the Configurator gives them, so the transport
# replaces the Configurator's activity instead of adding a second one.
#
# This script used to create them itself with `luna-send -a com.palm.imlibpurple`. -a makes the
# caller an APP, so those activities got creator {"appId":"com.palm.imlibpurple"}: a separate set
# next to the Configurator's, never replaced by it or by the transport, the login-state one
# firing loginStateChanged a second time. luna-send cannot create as the service. So now this
# script:
#   1. cancels what earlier runs of it left behind (creator appId:com.palm.imlibpurple; a cancel
#      by name looks the activity up under the caller's own id, so -a reaches exactly those and
#      never the transport's or the Configurator's),
#   2. restarts the transport, which re-creates the three with the right creator.
#
# ORDER MATTERS: the watch trigger query is `where status=pending AND folder=outbox` on
# com.palm.immessage.libpurple:1. If that kind is missing its indexes (also reverted by a reset ->
# db8 -3965 "no index for query"), the activity is torn down the instant it tries to arm. So run
# provision-im-db.sh (re-registers kinds + indexes + permissions) BEFORE this script.
#
# Needs a transport that re-arms its watches at start (see above); an older one creates nothing.
# NEVER kill -9 the transport (corrupts the PmLog sem); use stop/start.

ACTDIR=/etc/palm/activities/com.palm.imlibpurple
FAIL=0

for f in "$ACTDIR"/com.palm.immessage.libpurple \
         "$ACTDIR"/com.palm.imcommand.libpurple \
         "$ACTDIR"/com.palm.imloginstate.libpurple; do
    [ -f "$f" ] || { echo "  MISSING $(basename "$f")"; FAIL=1; continue; }
    # The activity's name, from the config file (one "name" key, inside "activity").
    name=$(tr -d '\n\t' < "$f" | sed -n 's/.*"name"[ ]*:[ ]*"\([^"]*\)".*/\1/p')
    [ -n "$name" ] || { echo "  NO NAME in $(basename "$f")"; FAIL=1; continue; }
    # Only an earlier run's copy (creator appId:com.palm.imlibpurple) matches; none left is fine.
    echo "cancel leftover copy (appId creator), if any: $name"
    luna-send -n 1 -a com.palm.imlibpurple -f luna://com.palm.activitymanager/cancel \
        "{\"activityName\":\"$name\"}" </dev/null
done

# The transport re-creates them as it starts.
if command -v systemctl >/dev/null 2>&1 && systemctl cat imlibpurpleservice.service >/dev/null 2>&1; then
    echo "restarting imlibpurpleservice (it re-creates the activities)"
    systemctl restart imlibpurpleservice.service || FAIL=1
else
    echo "Now restart the transport (stop imtransport ; start imtransport); it re-creates the activities."
fi

echo "provision-im-activities: done (FAIL=$FAIL)"
exit $FAIL
