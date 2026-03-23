#!/usr/bin/env bash
set -o pipefail
trap 'exit 0' TERM INT HUP

"${HOME}/.platformio/penv/bin/pio" device monitor -b 115200 -f direct |
stdbuf -oL awk '
{
    gsub(/[^[:print:]\t]/, "", $0)
    if (length($0) == 0) next
    if ($0 ~ /\[API\] GET \/api\/logs$/) next
    if ($0 ~ /\[WEB\] Static fallback served:/) next
    if ($0 ~ /\[KEYPAD\] key=/) next
    if ($0 ~ /AsyncTCP\.cpp:986/ || $0 ~ /_poll[(][)]: pcb is NULL/) next

    if ($0 == last) {
        next
    }

    print
    last = $0
}
'
