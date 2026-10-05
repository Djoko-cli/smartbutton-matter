#!/usr/bin/env bash
#
# Vérifie que chaque symbole des sdkconfig.defaults* existe réellement dans les
# Kconfig d'ESP-IDF / esp-matter / connectedhomeip.
#
# Raison d'être : un symbole inconnu dans un sdkconfig.defaults est ignoré
# SILENCIEUSEMENT. Sur un projet dont toute la valeur tient au réglage fin de
# l'ICD et du power management, une faute de frappe ne se voit pas au build —
# elle se voit six mois plus tard sur la pile. Voir docs/02-energie.md.
#
# Usage :  ./tools/check_sdkconfig.sh
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -n "${ESP_MATTER_PATH:-}" ] || source "$HERE/tools/env.sh"
KNOWN="$(mktemp)"
trap 'rm -f "$KNOWN"' EXIT

echo "Indexation des Kconfig…"
{ find "$IDF_PATH" -maxdepth 1 -name 'Kconfig*' -type f -print0 2>/dev/null
  find "$IDF_PATH/components" \
       "$ESP_MATTER_PATH/components" \
       "$ESP_MATTER_PATH/device_hal" \
       "$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/config/esp32" \
       -name 'Kconfig*' -type f -print0 2>/dev/null; } \
  | xargs -0 grep -hoE '^[[:space:]]*(menu)?config[[:space:]]+[A-Z0-9_]+' 2>/dev/null \
  | awk '{print $NF}' | sort -u > "$KNOWN"

echo "  $(wc -l < "$KNOWN" | tr -d ' ') symboles connus"
echo

rc=0
for f in "$HERE"/firmware/sdkconfig.defaults* "$HERE"/firmware/profiles/*.defaults; do
    [ -f "$f" ] || continue
    echo "── $(basename "$f")"
    bad=0
    while IFS= read -r sym; do
        # IDF_TARGET est posé par `idf.py set-target`, pas déclaré en Kconfig.
        [ "$sym" = "IDF_TARGET" ] && continue
        if ! grep -qxF "$sym" "$KNOWN"; then
            echo "   ✗ CONFIG_$sym  — INCONNU (sera ignoré en silence)"
            bad=$((bad + 1)); rc=1
        fi
    done < <(grep -oE '^CONFIG_[A-Z0-9_]+' "$f" | sed 's/^CONFIG_//' | sort -u)
    [ "$bad" -eq 0 ] && echo "   ✓ tous les symboles existent"
    echo
done

exit $rc
