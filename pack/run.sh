#!/bin/sh
cd "$(dirname "$0")"
./froggysoundtuner.mipsel >log.txt 2>&1
rc=$?
echo "EXIT CODE: $rc" >>log.txt
exit $rc
