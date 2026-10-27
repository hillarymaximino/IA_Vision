"""Prepara o ambiente Python do launcher antes de importar o PySide6.

Usa apenas a biblioteca padrao. Fluxo:
  1. Se o PySide6 ja importa (ex: rodando dentro do venv), nao faz nada.
  2. Cria tools/.venv (o Debian/Ubuntu bloqueia pip no Python do sistema, PEP 668).
  3. Instala tools/requirements.txt no venv.
  4. No Linux, garante a lib de sistema que o Qt 6.5+ exige (libxcb-cursor0).
  5. Reinicia o launcher com o Python do venv.
"""

from __future__ import annotations

import ctypes.util
import importlib.util
import os
import shutil
import subprocess
import sys
import venv
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
VENV_DIR = TOOLS_DIR / ".venv"
REQUIREMENTS = TOOLS_DIR / "requirements.txt"
_REEXEC_FLAG = "SICOV_LAUNCHER_BOOTSTRAPPED"

# Pacote que fornece python3 -m venv / ensurepip em cada gerenciador
_VENV_PACKAGES = {"apt-get": ["python3-venv"], "dnf": ["python3"], "pacman": ["python"]}
# Dependencia de sistema do plugin xcb do Qt 6.5+
_XCB_CURSOR_PACKAGES = {"apt-get": ["libxcb-cursor0"], "dnf": ["xcb-util-cursor"], "pacman": ["xcb-util-cursor"]}


def _log(message: str) -> None:
    print(f"[sicov-launcher] {message}", flush=True)


def _venv_python() -> Path:
    return VENV_DIR / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def _package_manager() -> str | None:
    for manager in ("apt-get", "dnf", "pacman"):
        if shutil.which(manager):
            return manager
    return None


def _confirm(question: str) -> bool:
    if not sys.stdin or not sys.stdin.isatty():
        return False
    try:
        return input(f"[sicov-launcher] {question} [s/N] ").strip().lower() in ("s", "sim", "y", "yes")
    except EOFError:
        return False


def _system_install(packages_by_manager: dict[str, list[str]], reason: str) -> bool:
    """Instala pacotes do sistema com sudo, sempre pedindo confirmacao antes."""
    manager = _package_manager()
    if manager is None:
        _log(f"Gerenciador de pacotes nao suportado. Instale manualmente: {reason}")
        return False
    packages = packages_by_manager[manager]
    if manager == "apt-get":
        command = ["sudo", "sh", "-c", f"apt-get update && apt-get install -y {' '.join(packages)}"]
    elif manager == "dnf":
        command = ["sudo", "dnf", "install", "-y", *packages]
    else:
        command = ["sudo", "pacman", "-S", "--needed", "--noconfirm", *packages]

    _log(f"Necessario para {reason}: {' '.join(packages)}")
    if not _confirm(f"Executar '{' '.join(command)}'?"):
        _log(f"Instalacao recusada. Rode manualmente: {' '.join(command)}")
        return False
    return subprocess.run(command, check=False).returncode == 0


def _create_venv() -> None:
    _log(f"Criando ambiente virtual em {VENV_DIR} ...")
    try:
        venv.EnvBuilder(with_pip=True, clear=VENV_DIR.exists()).create(VENV_DIR)
    except (subprocess.CalledProcessError, OSError) as error:
        # Debian/Ubuntu: falta o pacote python3-venv (ensurepip)
        _log(f"Falha ao criar o venv ({error}).")
        shutil.rmtree(VENV_DIR, ignore_errors=True)
        if not _system_install(_VENV_PACKAGES, "criar o ambiente virtual Python"):
            raise SystemExit(1)
        venv.EnvBuilder(with_pip=True).create(VENV_DIR)


def _install_requirements() -> None:
    python = str(_venv_python())
    _log(f"Instalando dependencias Python de {REQUIREMENTS.name} ...")
    subprocess.run([python, "-m", "pip", "install", "--upgrade", "pip"], check=True)
    subprocess.run([python, "-m", "pip", "install", "-r", str(REQUIREMENTS)], check=True)


def _ensure_qt_system_libs() -> None:
    if not sys.platform.startswith("linux"):
        return
    if os.environ.get("QT_QPA_PLATFORM", "").startswith(("offscreen", "wayland")):
        return
    if ctypes.util.find_library("xcb-cursor") is None:
        _system_install(_XCB_CURSOR_PACKAGES, "abrir janelas Qt no X11 (plugin xcb)")


def ensure_environment(entry_point: Path) -> None:
    """Garante PySide6 disponivel; pode reiniciar o processo dentro do venv."""
    if importlib.util.find_spec("PySide6") is not None:
        return

    if os.environ.get(_REEXEC_FLAG):
        _log("PySide6 continua indisponivel apos a instalacao. Verifique a saida do pip acima.")
        raise SystemExit(1)

    if not _venv_python().exists():
        _create_venv()
    try:
        _install_requirements()
    except subprocess.CalledProcessError:
        _log("pip falhou. Verifique a conexao com a internet e tente novamente.")
        raise SystemExit(1)
    _ensure_qt_system_libs()

    _log("Reiniciando o launcher no ambiente virtual ...")
    os.environ[_REEXEC_FLAG] = "1"
    python = str(_venv_python())
    args = [python, str(entry_point), *sys.argv[1:]]
    if os.name == "nt":
        # os.execv no Windows nao substitui o processo de verdade; delega e repassa o codigo
        raise SystemExit(subprocess.run(args, check=False).returncode)
    os.execv(python, args)
