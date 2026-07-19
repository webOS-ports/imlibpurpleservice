# Transport device launch chain

These files are hand-installed on the TouchPad (they are not produced by `build.sh`, which only
builds `/usr/bin/imlibpurpletransport`). They are vendored here so a reflash can restore them.

## The chain

1. **`/etc/event.d/imtransport`** — upstart job. Starts on `ls-hubd_private-ready`, `respawn`s,
   and execs `/var/imdaemon.sh`. This is the keep-alive: it holds the transport resident so the
   luna hub cannot idle-reap it (see the job's own comment for the full rationale).
2. **`/var/imdaemon.sh`** — launcher. Execs `imwrap.sh` with the same args as the LS2 `.service`.
3. **`/var/imwrap.sh`** — the LD_PRELOAD / LD_LIBRARY_PATH wrapper that loads the transport under
   the wpe-glibc loader (required for purple-signal's in-process JVM). Also self-rotates the log,
   and self-provisions the OpenSSL override (see below).

## OpenSSL override (SSLFIX) — required for Facebook

purple-facebook is the only prpl that uses libpurple's *native* `purple_ssl_*` layer (the others
bring their own TLS). That layer needs an ssl provider plugin (`ssl-openssl.so`) to load at startup.
wpe-glibc/lib ships an OLD `libcrypto.so.3` (OpenSSL 3.0.16) and, because wpe-glibc must be first on
`LD_LIBRARY_PATH`, it shadows the newer Atlas OpenSSL in `wpe-252/lib` — so `libssl.so.3` (needs
`OPENSSL_3.3.0`) fails to load, no ssl provider registers, and Facebook logins hang then "Cancelled".

`imwrap.sh` fixes this by prepending `/media/internal/sslfix` (holding ONLY the matched wpe-252
`libcrypto.so.3` + `libssl.so.3`) to `LD_LIBRARY_PATH`, so OpenSSL resolves from there while
libc/pthread/dl/rt still come from wpe-glibc. It **self-provisions** that dir from `wpe-252/lib` on
every launch (copies if missing or size-changed), so a reflash restores it automatically — the only
dependency is that the Atlas app (`org.webosports.app.atlas/.../wpe-252/lib`) with OpenSSL >= 3.3.0
is installed. If Atlas is absent, the copy is skipped and Facebook falls back to broken (all other
services unaffected).

The original on-demand LS2 service (`com.palm.imlibpurple.service`, `Exec=/var/imwrap.sh -c … PalmPre
Palm-Pre/1.5`) is left in place untouched. With the upstart daemon owning the bus name, the hub
routes method calls to the resident instance instead of launching (and later reaping) its own.

## Contacts search-by-service (two parts)

Lets the native Contacts search box find contacts by IM **service** — typing
"telegram", "whatsapp", "facebook", "signal", … surfaces those contacts — not just
by name/handle/email.

The Contacts search field runs one db8 full-text query (`?`) against the
`searchProperty` multi-index on `com.palm.person:1`. Stock, that index tokenizes
names, `organization.name`, `nickname`, `searchTerms`, `ims.value` and
`emails.value` — but **not** `ims.type`, where the service token lives
(`type_telegram`, `type_whatsapp`, …). Two changes, both required (verified on a
topaz device):

1. **Index patch** — `etc/palm/db/kinds/com.palm.person` here is the stock kind
   copied verbatim with `{"name": "ims.type", "tokenize": "all"}` added to the
   searchProperty include list, and the index **renamed**
   (`favorite_searchProperty_sortKey` → `favorite_searchPropertySvc_sortKey`)
   because db8 only rebuilds an index whose name changed. `var/provision-person-search.sh`
   registers it as the owning service and forces the reindex (no migration — the
   `ims.type` values already exist on every person, so all existing contacts are
   covered immediately).
2. **App patch** — `com.palm.app.contacts/app/patches.js` rewrites a typed service
   name (`telegram`) to the stored token (`type_telegram`) before the query. This is
   necessary because db8's tokenizer keeps the `type_` prefix as one token, so a
   bare service word never matches the raw `ims.type` value on its own.

Two dead ends ruled out on device: the `"all"` tokenizer does **not** split the
`type_` prefix (so the index patch alone can't match a plain word), and the contacts
linker builds `person.searchTerms` from names only — it ignores a contact's own
`searchTerms` field (so seeding searchTerms on the buddy contact does nothing).

Reflash reverts both to stock — re-run the Install below.

## Install

    mount -o remount,rw /
    cp var/imwrap.sh var/imdaemon.sh /var/ && chmod 755 /var/imwrap.sh /var/imdaemon.sh
    cp etc/event.d/imtransport /etc/event.d/
    # search-by-service, part 1 (index):
    cp etc/palm/db/kinds/com.palm.person /etc/palm/db/kinds/com.palm.person
    cp var/provision-person-search.sh /var/ && chmod 755 /var/provision-person-search.sh
    /var/provision-person-search.sh
    # search-by-service, part 2 (app): from the com.palm.app.contacts checkout
    cp app/patches.js /media/cryptofs/apps/usr/palm/applications/com.palm.app.contacts/app/patches.js
    stop LunaSysMgr; start LunaSysMgr
    sync   # then tellbootie / reboot

## Verify

    status imtransport            # -> "imtransport (start) running, process <pid>"
    # over an idle period the pid must NOT change and imstdout.log must show no repeated
    # "imlibpurpletransport stopping" (idle-reap) events.
