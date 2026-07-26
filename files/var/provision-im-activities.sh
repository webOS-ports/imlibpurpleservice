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
# stock Configurator does NOT re-provision these (unchanged, 2011-dated) configs on the next boot
# ("configured" counts them but leaves the imlibpurple outbound watches uncreated). The connection
# and login-state watches survive because the transport creates those itself (ConnectionStateHandler);
# only the OUTBOUND message/command watches rely on the Configurator -- that is the deployment gap.
# (The stale creator=com.palm.imyahoo "pending messages/commands" activities that DO survive watch
# com.palm.immessage.yahoo:1 and call a dead service, so they never help our rows.)
#
# ORDER MATTERS: the watch trigger query is `where status=pending AND folder=outbox` on
# com.palm.immessage.libpurple:1. If that kind is missing its indexes (also reverted by a reset ->
# db8 -3965 "no index for query"), the activity is torn down the instant it tries to arm. So run
# provision-im-db.sh (re-registers kinds + indexes + permissions) BEFORE this script.
#
# Idempotent (replace:true). NEVER kill -9 the transport (corrupts the PmLog sem); use stop/start.
# After running:  stop imtransport ; start imtransport

ACTDIR=/etc/palm/activities/com.palm.imlibpurple
FAIL=0

for f in "$ACTDIR"/com.palm.immessage.libpurple \
         "$ACTDIR"/com.palm.imcommand.libpurple \
         "$ACTDIR"/com.palm.imloginstate.libpurple; do
    [ -f "$f" ] || { echo "  MISSING $(basename "$f")"; FAIL=1; continue; }
    # The config file is {"start":true,"activity":{...}} -- exactly the activitymanager/create
    # envelope. Flatten (strip newlines/tabs; luna-send mishandles multi-line JSON) and inject
    # "replace":true so a re-run replaces the existing activity rather than erroring. Create AS
    # com.palm.imlibpurple so the transport (same id) owns + can restart the watch after each drain.
    payload=$(tr -d '\n\t' < "$f" | sed 's/  */ /g; s/"start": *true,/"start":true,"replace":true,/')
    echo "create activity: $(basename "$f")"
    luna-send -i -n 1 -a com.palm.imlibpurple -f luna://com.palm.activitymanager/create "$payload" </dev/null || FAIL=1
done

echo "provision-im-activities: done (FAIL=$FAIL). Now: stop imtransport ; start imtransport"
exit $FAIL
