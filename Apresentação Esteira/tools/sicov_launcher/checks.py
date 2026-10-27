"""Verificacao do ambiente antes de compilar/executar (somente leitura, sem efeitos)."""

from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from .project import IS_WINDOWS, ProjectLayout

MIN_CMAKE = (3, 21)


class Fix(Enum):
    """Correcao automatica disponivel para um item (ver installer.py)."""

    CMAKE = "cmake"           # CMake >= 3.21 via pip no venv do launcher
    COMPILER = "compiler"     # compilador C++, make e pkg-config
    OPENCV = "opencv"         # pacote de desenvolvimento do OpenCV
    DIALOUT = "dialout"       # adiciona o usuario ao grupo dialout


class Scope(Enum):
    """Para qual etapa o item e obrigatorio."""

    BRIDGE = "bridge"   # compilar a bridge (e o minimo para tudo)
    VISION = "vision"   # compilar a visao
    MODEL = "model"     # rodar a visao (o modelo nao e necessario para compilar)
    SERIAL = "serial"   # falar com o STM32


@dataclass
class LauncherConfig:
    galaxy_sdk_dir: str = ""
    opencv_dir: str = ""
    serial_port: str = ""
    camera_sn: str = "FBR26040067"
    model_path: str = ""


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str
    hint: str = ""
    scopes: set[Scope] = field(default_factory=set)
    fix: Fix | None = None


def _run(cmd: list[str]) -> str | None:
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def check_cmake() -> CheckResult:
    output = _run(["cmake", "--version"])
    if output is None:
        return CheckResult("CMake", False, "nao encontrado no PATH",
                           "Use 'Instalar dependencias' (instala via pip no venv do launcher)",
                           {Scope.BRIDGE, Scope.VISION}, Fix.CMAKE)
    match = re.search(r"(\d+)\.(\d+)", output)
    version = (int(match.group(1)), int(match.group(2))) if match else (0, 0)
    ok = version >= MIN_CMAKE
    return CheckResult("CMake", ok, output.splitlines()[0],
                       "" if ok else f"Necessario >= {MIN_CMAKE[0]}.{MIN_CMAKE[1]}; use 'Instalar dependencias'",
                       {Scope.BRIDGE, Scope.VISION}, None if ok else Fix.CMAKE)


def check_compiler() -> CheckResult:
    if IS_WINDOWS:
        # O gerador Visual Studio localiza o MSVC sozinho; basta o vswhere existir
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        ok = vswhere.exists()
        return CheckResult("Compilador C++", ok, "Visual Studio instalado" if ok else "Visual Studio nao encontrado",
                           "" if ok else "Instale o VS 2022 Build Tools (Desktop C++)",
                           {Scope.BRIDGE, Scope.VISION})
    compiler = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    make = shutil.which("make")
    ok = bool(compiler and make)
    detail = f"{compiler or 'sem compilador'} / {make or 'sem make'}"
    return CheckResult("Compilador C++", ok, detail,
                       "" if ok else "Use 'Instalar dependencias' (build-essential)",
                       {Scope.BRIDGE, Scope.VISION}, None if ok else Fix.COMPILER)


def check_opencv(cfg: LauncherConfig) -> CheckResult:
    candidates: list[str] = []
    if cfg.opencv_dir:
        candidates.append(cfg.opencv_dir)
    if os.environ.get("OpenCV_DIR"):
        candidates.append(os.environ["OpenCV_DIR"])
    for directory in candidates:
        if (Path(directory) / "OpenCVConfig.cmake").exists():
            return CheckResult("OpenCV", True, f"OpenCVConfig.cmake em {directory}", scopes={Scope.VISION})

    if not IS_WINDOWS:
        version = _run(["pkg-config", "--modversion", "opencv4"])
        configs = (glob.glob("/usr/lib/*/cmake/opencv4/OpenCVConfig.cmake")
                   + glob.glob("/usr/lib/cmake/opencv4/OpenCVConfig.cmake")
                   + glob.glob("/usr/local/lib/cmake/opencv4/OpenCVConfig.cmake"))
        if configs:
            return CheckResult("OpenCV", True, f"{version or 'versao ?'} ({Path(configs[0]).parent})",
                               scopes={Scope.VISION})
        return CheckResult("OpenCV", False, "OpenCVConfig.cmake nao encontrado",
                           "Use 'Instalar dependencias' (libopencv-dev) ou informe o OpenCV_DIR",
                           {Scope.VISION}, Fix.OPENCV)

    return CheckResult("OpenCV", False, "OpenCV_DIR nao configurado",
                       "Informe a pasta com OpenCVConfig.cmake (ex: C:/opencv/build)", {Scope.VISION})


def check_galaxy(cfg: LauncherConfig) -> CheckResult:
    if not cfg.galaxy_sdk_dir:
        return CheckResult("Galaxy SDK", False, "pasta nao informada",
                           "Informe a pasta do SDK que contem inc/GxIAPI.h", {Scope.VISION})
    root = Path(cfg.galaxy_sdk_dir)
    header = next((p for p in (root / "inc" / "GxIAPI.h", root / "include" / "GxIAPI.h") if p.exists()), None)
    ok = header is not None
    return CheckResult("Galaxy SDK", ok, str(header) if ok else f"GxIAPI.h ausente em {root}",
                       "" if ok else "Aponte para a pasta Galaxy_camera (Linux) ou 'VC SDK' (Windows)",
                       {Scope.VISION})


def check_model(cfg: LauncherConfig, layout: ProjectLayout) -> CheckResult:
    path = Path(cfg.model_path) if cfg.model_path else layout.default_model
    ok = path.is_file() and path.stat().st_size > 1_000_000
    return CheckResult("Modelo ONNX", ok, str(path) if ok else f"nao encontrado: {path}",
                       "" if ok else "Use 'Extrair modelo do zip' ou selecione o arquivo .onnx",
                       {Scope.MODEL})


def check_serial(cfg: LauncherConfig) -> list[CheckResult]:
    results: list[CheckResult] = []
    port = cfg.serial_port

    if not port:
        results.append(CheckResult("Porta serial", False, "nenhuma porta selecionada",
                                   "Conecte o ST-Link e clique em 'Atualizar portas'", {Scope.SERIAL}))
    elif IS_WINDOWS:
        results.append(CheckResult("Porta serial", True, port, scopes={Scope.SERIAL}))
    elif not os.path.exists(port):
        results.append(CheckResult("Porta serial", False, f"{port} nao existe",
                                   "Confira o cabo USB e o ST-Link", {Scope.SERIAL}))
    else:
        ok = os.access(port, os.R_OK | os.W_OK)
        results.append(CheckResult("Porta serial", ok, f"{port} {'acessivel' if ok else 'sem permissao'}",
                                   "" if ok else "Veja o item 'Grupo dialout'", {Scope.SERIAL}))

    if not IS_WINDOWS:
        import grp  # disponivel apenas em POSIX

        try:
            dialout_gid = grp.getgrnam("dialout").gr_gid
        except KeyError:
            dialout_gid = None
        # os.getgroups() reflete a sessao atual: apos o usermod e preciso relogar
        in_group = dialout_gid is not None and dialout_gid in os.getgroups()
        results.append(CheckResult("Grupo dialout", in_group,
                                   "usuario no grupo" if in_group else "usuario fora do grupo nesta sessao",
                                   "" if in_group else "Use 'Instalar dependencias' e depois faca logout/login",
                                   {Scope.SERIAL}, None if in_group else Fix.DIALOUT))
    return results


def run_all(cfg: LauncherConfig, layout: ProjectLayout) -> list[CheckResult]:
    return [
        check_cmake(),
        check_compiler(),
        check_opencv(cfg),
        check_galaxy(cfg),
        check_model(cfg, layout),
        *check_serial(cfg),
    ]


def blocking_failures(results: list[CheckResult], scopes: set[Scope]) -> list[CheckResult]:
    """Falhas que impedem as etapas pedidas."""
    return [r for r in results if not r.ok and r.scopes & scopes]
