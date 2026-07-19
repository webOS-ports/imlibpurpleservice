#!/bin/sh
# Keep the IM transport resident (respawned by upstart) so it isn't hub-idle-reaped and can
# receive pushed messages. Mirrors the exact invocation the LS2 .service uses (see
# ../sysbus/com.palm.imlibpurple.service.in Exec line): -c <log config> <device args>.
exec /var/imwrap.sh -c '{"log":{"appender":{"type":"stdout"},"levels":{"imlibpurple":"debug"}}}' PalmPre Palm-Pre/1.5
