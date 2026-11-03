#!/bin/sh
# Runs starfox64 for SECONDS with the launcher stopped (bind-mounting
# /bin/true over it keeps the frontend from restarting it), then restores the
# launcher. Output in /tmp/sf64.log; sf64.env applies.
# Usage: run-test.sh SECONDS
. /etc/profile.d/s30.sh
mount --bind /bin/true /usr/bin/s30-launcher
while pidof s30-launcher >/dev/null; do killall s30-launcher 2>/dev/null; sleep 0.5; pidof s30-launcher >/dev/null && killall -9 s30-launcher 2>/dev/null; done
sleep 1
cd /mnt/sd/home/ports/sf64
[ -f sf64.env ] && set -a && . ./sf64.env && set +a
LD_PRELOAD=/usr/lib/s30/libs30lease.so timeout "$1" ./starfox64 > /tmp/sf64.log 2>&1
echo "exit=$?" >> /tmp/sf64.log
umount /usr/bin/s30-launcher
