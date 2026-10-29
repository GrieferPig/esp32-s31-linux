#!/bin/sh
# One supervised getty, only while USB serial login is selected.

port="${1:-}"
case "$port" in ttyGS[0-9]*) ;; *) exit 2 ;; esac
case "${port#ttyGS}" in ''|*[!0-9]*) exit 2 ;; esac
child=''
stop_console()
{
    trap - TERM INT HUP
    if [ -n "$child" ]; then
        kill -HUP "$child" 2>/dev/null || true
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
    exit 0
}
trap stop_console TERM INT HUP
while [ -c "/dev/$port" ]; do
    /sbin/getty -L "$port" 115200 vt100 &
    child=$!
    wait "$child" || true
    child=''
    # Avoid spinning when the cable is absent or the host closes the port.
    sleep 1 &
    child=$!
    wait "$child" || true
    child=''
done
