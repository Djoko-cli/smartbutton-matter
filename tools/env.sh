# Charge ESP-IDF + esp-matter.  À sourcer :  source tools/env.sh
#
# PlatformIO (utilisé par d'autres projets) place son propre python3 en tête du
# PATH. ESP-IDF chercherait alors un environnement Python qui n'existe pas
# (« idf5.5_py3.11_env not found ») et ne se chargerait pas, sans bloquer le
# shell. On lui désigne explicitement celui créé à l'installation.
_sb_venv="$(ls -d "$HOME"/.espressif/python_env/idf5.5_py*_env 2>/dev/null | tail -n 1)"
[ -n "$_sb_venv" ] && export IDF_PYTHON_ENV_PATH="$_sb_venv"
unset _sb_venv
# Les export.sh lisent des variables non définies : on suspend `set -u` si un
# script appelant l'a activé, puis on le rétablit.
case $- in *u*) _sb_u=1; set +u ;; *) _sb_u= ;; esac
source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
source "$HOME/esp/esp-matter/export.sh" >/dev/null 2>&1
[ -n "$_sb_u" ] && set -u
unset _sb_u
if [ -z "${IDF_PATH:-}" ] || [ -z "${ESP_MATTER_PATH:-}" ]; then
    echo "tools/env.sh : échec du chargement d'ESP-IDF / esp-matter." >&2
    echo "  Diagnostic : source \$HOME/esp/esp-idf/export.sh (sans redirection)" >&2
    return 1 2>/dev/null || exit 1
fi
