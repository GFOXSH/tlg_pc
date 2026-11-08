#!/usr/bin/env bash
# Stops packaging when the staged folder holds loose game/firmware files, official art, or
# player-created data (saves, caches, settings, logs). NGS2 V5 is linked into the probe binary.
set -euo pipefail
dest=$1
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname '*.psarc' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.pkg' -o -iname '*.sfo' \
    -o -iname icon0.png -o -iname 'pic0.png' -o -iname 'pic1.png' -o -iname 'playgo-chunk*' \
    -o -iname boot-linked.bin -o -iname content.bin -o -iname tlg.ini -o -iname settings.json \
    -o -iname '*.log' -o -iname '*.spv' -o -path '*/user/*' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
echo "Package check: no loose game/firmware assets or player files."
