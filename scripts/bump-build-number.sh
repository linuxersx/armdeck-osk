#!/bin/sh
# Increment the ArmDeck OSK build number and keep pkgrel in sync.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FILE="$ROOT/BUILD_NUMBER"
[ -f "$FILE" ] || echo 1 > "$FILE"
N=$(cat "$FILE" 2>/dev/null || echo 0)
case "$N" in (*[!0-9]*|'') N=0 ;; esac
N=$((N + 1))
printf '%s\n' "$N" > "$FILE"
PKG="$ROOT/packaging/arch-bin/PKGBUILD"
if [ -f "$PKG" ]; then
    sed -i.bak "s/^pkgrel=.*/pkgrel=$N/" "$PKG" && rm -f "$PKG.bak"
fi
printf '%s\n' "$N"
