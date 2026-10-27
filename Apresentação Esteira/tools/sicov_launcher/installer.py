"""Monta os comandos que corrigem dependencias faltando (executados pelo ProcessRunner)."""

from __future__ import annotations

import getpass
import shlex
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

from .checks import Fix
from .project import IS_WINDOWS
from .runner import Step

# Pacotes por gerenciador. A chave e o Fix que o pacote resolve.
_SYSTEM_PACKAGES: dict[str, dict[Fix, list[str]]] = {
    "apt-get": {
        Fix.COMPILER: ["build-essential", "pkg-config"],
        Fix.OPENCV: ["libopencv-dev"],
        Fix.ARM_TOOLCHAIN: ["gcc-arm-none-eabi", "libnewlib-arm-none-eabi"],
        Fix.FLASHER: ["stlink-tools"],   # inclui as regras udev do ST-Link
    },
    "dnf": {
        Fix.COMPILER: ["gcc-c++", "make", "pkgconf-pkg-config"],
        Fix.OPENCV: ["opencv-devel"],
        Fix.ARM_TOOLCHAIN: ["arm-none-eabi-gcc-cs", "arm-none-eabi-newlib"],
        Fix.FLASHER: ["stlink"],
    },
    "pacman": {
        Fix.COMPILER: ["base-devel", "pkgconf"],
        Fix.OPENCV: ["opencv"],
        Fix.ARM_TOOLCHAIN: ["arm-none-eabi-gcc", "arm-none-eabi-newlib"],
        Fix.FLASHER: ["stlink"],
    },
}

CMAKE_PIP_SPEC = "cmake>=3.21"
NINJA_PIP_SPEC = "ninja"
_SYSTEM_FIXES = {Fix.COMPILER, Fix.OPENCV, Fix.ARM_TOOLCHAIN, Fix.FLASHER}


@dataclass
class InstallPlan:
    steps: list[Step]
    description: list[str]          # linhas legiveis para o dialogo de confirmacao
    manual: list[str]               # o que nao da para automatizar


def package_manager() -> str | None:
    for manager in _SYSTEM_PACKAGES:
        if shutil.which(manager):
            return manager
    return None


def _add_elevated(steps: list[Step], manual: list[str], title: str, command: str, cwd: Path) -> bool:
    """Agenda o comando como root via pkexec (pedido de senha grafico do sistema).

    Sem pkexec nao ha como pedir a senha de dentro da interface (o sudo exige um
    terminal), entao o comando vai para a lista manual. Retorna True se agendou.
    """
    if shutil.which("pkexec"):
        steps.append(Step(title, "pkexec", ["sh", "-c", command], cwd))
        return True
    manual.append(f"Rode no terminal: sudo sh -c {shlex.quote(command)}")
    return False


def build_plan(fixes: set[Fix], cwd: Path) -> InstallPlan:
    steps: list[Step] = []
    description: list[str] = []
    manual: list[str] = []

    # pip no venv do launcher: versao recente em qualquer distro, sem sudo
    pip_specs = [spec for fix, spec in ((Fix.CMAKE, CMAKE_PIP_SPEC), (Fix.NINJA, NINJA_PIP_SPEC)) if fix in fixes]
    if pip_specs:
        steps.append(Step("Instalar ferramentas (pip)", sys.executable,
                          ["-m", "pip", "install", "--upgrade", *pip_specs], cwd))
        description.append(f"pip install {' '.join(repr(s) for s in pip_specs)} no venv do launcher (sem sudo)")

    system_fixes = fixes & _SYSTEM_FIXES
    if system_fixes:
        manager = package_manager()
        if IS_WINDOWS:
            manual.append("Windows: instale o Visual Studio 2022 Build Tools, o OpenCV e o STM32CubeIDE manualmente "
                          "(ver README).")
        elif manager is None:
            manual.append("Gerenciador de pacotes nao suportado (apt, dnf ou pacman). Instale os pacotes manualmente.")
        else:
            packages = [pkg for fix in sorted(system_fixes, key=lambda f: f.value)
                        for pkg in _SYSTEM_PACKAGES[manager][fix]]
            joined = " ".join(shlex.quote(p) for p in packages)
            if manager == "apt-get":
                command = f"apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y {joined}"
            elif manager == "dnf":
                command = f"dnf install -y {joined}"
            else:
                command = f"pacman -S --needed --noconfirm {joined}"
            if _add_elevated(steps, manual, "Instalar pacotes do sistema", command, cwd):
                description.append(f"{manager}: {' '.join(packages)} (pede a senha de administrador)")

    if Fix.DIALOUT in fixes and not IS_WINDOWS:
        user = getpass.getuser()
        command = f"usermod -aG dialout {shlex.quote(user)}"
        if _add_elevated(steps, manual, "Adicionar usuario ao grupo dialout", command, cwd):
            description.append(f"usermod -aG dialout {user} (vale apos logout/login)")

    return InstallPlan(steps, description, manual)


GALAXY_HINT = ("O Galaxy SDK da Daheng e proprietario e nao tem pacote. Baixe em "
               "https://www.daheng-imaging.com (Downloads > Software > Galaxy), instale e "
               "aponte o campo 'Galaxy SDK' para a pasta que contem inc/GxIAPI.h.")
