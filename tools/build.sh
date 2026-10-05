#!/usr/bin/env bash
#
# Compile / flashe le firmware selon un profil. Chaque profil a son propre
# dossier de build et son propre sdkconfig : passer de l'un à l'autre ne force
# jamais de recompilation complète.
#
#   ./tools/build.sh dev                       # compile
#   ./tools/build.sh dev flash monitor         # compile, flashe, ouvre la console
#   ./tools/build.sh sleepy -p /dev/cu.usbmodem1101 flash
#   TARGET=esp32c6 ./tools/build.sh dev        # autre cible
#
# Profils :
#   dev     SuperMini, pas de veille, console USB, ICD rapide   (étape 1)
#   sleepy  SuperMini, vrai profil ICD LIT + light sleep        (étapes 2-3)
#   pcb     config de base seule (quartz 32 kHz externe)        (PCB final)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="${1:-}"
[ $# -gt 0 ] && shift
TARGET="${TARGET:-esp32h2}"

case "$PROFILE" in
    dev)    DEFAULTS="sdkconfig.defaults;profiles/supermini.defaults;profiles/dev.defaults" ;;
    sleepy) DEFAULTS="sdkconfig.defaults;profiles/supermini.defaults" ;;
    pcb)    DEFAULTS="sdkconfig.defaults" ;;
    *)      sed -n '3,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
# ESP-IDF ajoute de lui-même sdkconfig.defaults.<cible> après sdkconfig.defaults.

# Les export.sh changent le répertoire courant : on se replace après.
[ -n "${ESP_MATTER_PATH:-}" ] || source "$HERE/tools/env.sh"
cd "$HERE/firmware"

BUILD_DIR="build-$PROFILE-$TARGET"
export IDF_TARGET="$TARGET"

echo "▶ profil=$PROFILE cible=$TARGET dossier=$BUILD_DIR"
exec idf.py -B "$BUILD_DIR" \
    -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="$DEFAULTS" \
    -D IDF_TARGET="$TARGET" \
    "${@:-build}"
