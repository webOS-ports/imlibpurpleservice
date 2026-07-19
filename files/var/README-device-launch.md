# Transport device launch chain

These files are hand-installed on the TouchPad (they are not produced by `build.sh`, which only
builds `/usr/bin/imlibpurpletransport`). They are vendored here so a reflash can restore them.

## The chain

1. **`/etc/event.d/imtransport`** — upstart job. Starts on `ls-hubd_private-ready`, `respawn`s,
   and execs `/var/imdaemon.sh`. This is the keep-alive: it holds the transport resident so the
   luna hub cannot idle-reap it (see the job's own comment for the full rationale).
2. **`/var/imdaemon.sh`** — launcher. Execs `imwrap.sh` with the same args as the LS2 `.service`.
3. **`/var/imwrap.sh`** — the LD_PRELOAD / LD_LIBRARY_PATH wrapper that loads the transport under
   the wpe-glibc loader (required for purple-signal's in-process JVM). Also self-rotates the log.

The original on-demand LS2 service (`com.palm.imlibpurple.service`, `Exec=/var/imwrap.sh -c … PalmPre
Palm-Pre/1.5`) is left in place untouched. With the upstart daemon owning the bus name, the hub
routes method calls to the resident instance instead of launching (and later reaping) its own.

## Install

    mount -o remount,rw /
    cp var/imwrap.sh var/imdaemon.sh /var/ && chmod 755 /var/imwrap.sh /var/imdaemon.sh
    cp etc/event.d/imtransport /etc/event.d/
    sync   # then tellbootie / reboot

## Verify

    status imtransport            # -> "imtransport (start) running, process <pid>"
    # over an idle period the pid must NOT change and imstdout.log must show no repeated
    # "imlibpurpletransport stopping" (idle-reap) events.
