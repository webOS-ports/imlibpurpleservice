#!/bin/sh
# Keep the IM transport resident (respawned by upstart) so it isn't hub-idle-reaped and can
# receive pushed messages. Mirrors the exact invocation the LS2 .service uses (see
# ../sysbus/com.palm.imlibpurple.service.in Exec line): -c <log config> <device args>.
#
# IM_RESIDENT tells the transport it is the resident daemon so it does NOT self-terminate on idle
# (IMServiceHandler::OkToShutdown). Without this the transport's idle self-shutdown + this respawn
# fight each other in a ~30s churn loop that reloads every prpl (incl. purple-signal's JVM) each
# cycle and never lets slow accounts connect. The on-demand .service path leaves it unset.
export IM_RESIDENT=1
exec /var/imwrap.sh -c '{"log":{"appender":{"type":"stdout"},"levels":{"imlibpurple":"debug"}}}' PalmPre Palm-Pre/1.5
