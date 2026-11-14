#!/bin/sh
# Regenerate symbols.json from the real runtime's own introspection.
# Usage: scripts/regen.sh   (or LUNA_BIN=/path/to/luna scripts/regen.sh)
#
# The binary is looked up as $LUNA_BIN, then ../../build/luna (an
# in-tree build), then `luna` on PATH.
set -e
cd "$(dirname "$0")"

# repo root is three levels up from scripts/
LUNA="${LUNA_BIN:-../../../build/luna}"
# the default is a FILE path, not a PATH lookup: test -x on it, then
# fall back to `luna` on PATH (an installed luna dumps ITS staged
# modules, which may be older than the repo's — prefer the in-tree build)
if [ ! -x "$LUNA" ]; then
    LUNA="$(command -v luna)" || {
        echo "regen.sh: no luna binary (build one or pass LUNA_BIN=...)" >&2
        exit 1
    }
fi

"$LUNA" dump_symbols.lua ../symbols.json
