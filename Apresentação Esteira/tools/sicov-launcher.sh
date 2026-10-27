#!/usr/bin/env sh
# Abre o SICOV Launcher no Linux. Na primeira execucao instala o que faltar:
#   - python3 (pelo gerenciador de pacotes, pedindo confirmacao)
#   - PySide6 em tools/.venv (feito pelo proprio launcher, ver bootstrap.py)
set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if ! command -v python3 >/dev/null 2>&1; then
    echo "[sicov-launcher] python3 nao encontrado."
    if command -v apt-get >/dev/null 2>&1; then
        INSTALL="sudo apt-get update && sudo apt-get install -y python3 python3-venv"
    elif command -v dnf >/dev/null 2>&1; then
        INSTALL="sudo dnf install -y python3"
    elif command -v pacman >/dev/null 2>&1; then
        INSTALL="sudo pacman -S --needed --noconfirm python"
    else
        echo "[sicov-launcher] Instale o Python 3.9+ manualmente e rode de novo."
        exit 1
    fi
    printf '[sicov-launcher] Executar "%s"? [s/N] ' "$INSTALL"
    read -r REPLY
    case "$REPLY" in
        s|S|sim|y|Y|yes) sh -c "$INSTALL" ;;
        *) echo "[sicov-launcher] Cancelado."; exit 1 ;;
    esac
fi

exec python3 "$TOOLS_DIR/sicov_launcher" "$@"
