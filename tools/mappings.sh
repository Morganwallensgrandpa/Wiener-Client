#!/usr/bin/env bash
#
# Look up Minecraft member ids for the pinned game version.
#
# The client talks to the game through Fabric's *intermediary* names, which are
# stable for a pinned version but are not guessable by hand -- and a stale id
# resolves to nothing rather than failing loudly, so a module just silently does
# nothing. This fetches Yarn's named mappings for the pinned version and hands the
# lookup to tools/yarn_map.py, which is the only place the tiny v2 format is
# parsed.
#
#   tools/mappings.sh class net/minecraft/entity/Entity   # every member of a class
#   tools/mappings.sh net/minecraft/entity/Entity#getYaw  # one member
#   tools/mappings.sh inter:net/minecraft/class_1297#method_73189  # id -> name
#   tools/mappings.sh --check                             # verify mappings.h
#
# The mappings file is downloaded from Fabric's Maven on first use and cached in
# build/mappings/. Override the version with MC_VERSION=...
set -euo pipefail

MC_VERSION="${MC_VERSION:-1.21.11}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/mappings"
TINY="$OUT/mappings.tiny"

PY="${PYTHON:-python}"
command -v "$PY" >/dev/null 2>&1 || PY=python3

fetch() {
    if [ -f "$TINY" ]; then return; fi
    mkdir -p "$OUT"
    meta="$(curl -fsSL https://maven.fabricmc.net/net/fabricmc/yarn/maven-metadata.xml)"
    build="$(printf '%s' "$meta" | grep -o "${MC_VERSION}+build\.[0-9]*" \
             | sort -t. -k2 -n | tail -1)"
    if [ -z "$build" ]; then
        echo "no Yarn build found for $MC_VERSION" >&2
        exit 1
    fi
    echo "fetching Yarn $build ..." >&2
    curl -fsSL -o "$OUT/yarn.jar" \
        "https://maven.fabricmc.net/net/fabricmc/yarn/$build/yarn-$build-mergedv2.jar"
    unzip -o -q "$OUT/yarn.jar" "mappings/mappings.tiny" -d "$OUT"
    # The archive nests the file under mappings/; normalise the path.
    [ -f "$OUT/mappings/mappings.tiny" ] && mv -f "$OUT/mappings/mappings.tiny" "$TINY"
}

fetch

# --check verifies every constant in src/jni/mappings.h against the real remapped
# client jar with javap. That is the check worth running after touching the header.
if [ "${1:-}" = "--check" ]; then
    # APPDATA is a Windows path in this shell (C:\Users\...); cygpath turns it
    # into the /c/... form find understands.
    appdata="$(cygpath -u "${APPDATA:-}" 2>/dev/null || printf '%s' "$HOME/AppData/Roaming")"
    JAR="$(find "$appdata/ModrinthApp/profiles" -path '*remappedJars*' \
            -name 'client-intermediary.jar' 2>/dev/null | head -1)"
    AUTH="$(find "$appdata/ModrinthApp/meta/libraries/com/mojang/authlib" \
            -name 'authlib-*.jar' 2>/dev/null | head -1)"
    if [ -z "$JAR" ]; then
        echo "no remapped client jar found; run the game once through Fabric" >&2
        exit 1
    fi
    # The .tiny file goes along so every entry is also checked by name against
    # the same mappings the ids came from.
    exec "$PY" "$ROOT/tools/check_mappings.py" "$TINY" "$JAR" ${AUTH:+"$AUTH"}
fi

if [ "${1:-}" = "class" ]; then
    [ $# -ge 2 ] || { echo "usage: $0 class <named-class>" >&2; exit 2; }
    exec "$PY" "$ROOT/tools/yarn_map.py" "$TINY" --class "$2"
fi

if [ $# -eq 0 ]; then
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

exec "$PY" "$ROOT/tools/yarn_map.py" "$TINY" "$@"
