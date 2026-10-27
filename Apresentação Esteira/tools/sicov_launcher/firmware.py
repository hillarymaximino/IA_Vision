"""Descoberta do toolchain ARM e do gravador, e comandos de build/gravacao do STM32."""

from __future__ import annotations

import glob
import os
import shutil
from dataclasses import dataclass
from pathlib import Path

from .project import IS_WINDOWS

FIRMWARE_NAME = "comEdge"
FLASH_BASE = "0x08000000"   # inicio da flash interna do STM32F407

_EXE = ".exe" if IS_WINDOWS else ""


def _cubeide_plugin_dirs() -> list[Path]:
    """Pastas plugins/ das instalacoes do STM32CubeIDE (mais nova primeiro)."""
    if IS_WINDOWS:
        patterns = ["C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins",
                    "C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeIDE*/STM32CubeIDE/plugins"]
    else:
        home = str(Path.home())
        patterns = ["/opt/st/stm32cubeide_*/plugins", f"{home}/st/stm32cubeide_*/plugins",
                    f"{home}/STM32CubeIDE*/plugins"]
    found = {Path(p) for pattern in patterns for p in glob.glob(pattern)}
    return sorted(found, reverse=True)


def _from_cubeide(plugin_pattern: str, executable: str) -> Path | None:
    for plugins in _cubeide_plugin_dirs():
        for candidate in sorted(plugins.glob(f"{plugin_pattern}/tools/bin/{executable}{_EXE}"), reverse=True):
            return candidate
    return None


def _cubeprogrammer_standalone() -> Path | None:
    if IS_WINDOWS:
        roots = ["C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin"]
    else:
        roots = [f"{Path.home()}/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin",
                 "/opt/st/STM32CubeProgrammer/bin"]
    for root in roots:
        candidate = Path(root) / f"STM32_Programmer_CLI{_EXE}"
        if candidate.exists():
            return candidate
    return None


def find_arm_gcc() -> Path | None:
    on_path = shutil.which("arm-none-eabi-gcc")
    return Path(on_path) if on_path else _from_cubeide("*gnu-tools-for-stm32*", "arm-none-eabi-gcc")


def find_cmake() -> Path | None:
    on_path = shutil.which("cmake")
    return Path(on_path) if on_path else _from_cubeide("*externaltools.cmake*", "cmake")


def find_ninja() -> Path | None:
    on_path = shutil.which("ninja")
    return Path(on_path) if on_path else _from_cubeide("*externaltools.ninja*", "ninja")


@dataclass(frozen=True)
class Flasher:
    name: str           # STM32CubeProgrammer, st-flash ou OpenOCD
    program: Path

    def command(self, elf: Path, binary: Path) -> list[str]:
        """Argumentos para gravar, verificar e resetar o STM32 via ST-Link (SWD)."""
        if self.name == "STM32CubeProgrammer":
            return ["-c", "port=SWD", "-w", str(elf), "-v", "-rst"]
        if self.name == "st-flash":
            return ["--reset", "write", str(binary), FLASH_BASE]
        # OpenOCD: chaves protegem caminhos com espacos no Tcl
        return ["-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg",
                "-c", f"program {{{elf.as_posix()}}} verify reset exit"]


def find_flasher() -> Flasher | None:
    """Ordem: ferramenta oficial da ST, depois as alternativas open source."""
    cube = shutil.which("STM32_Programmer_CLI")
    cube_path = Path(cube) if cube else (_cubeprogrammer_standalone()
                                         or _from_cubeide("*externaltools.cubeprogrammer*", "STM32_Programmer_CLI"))
    if cube_path:
        return Flasher("STM32CubeProgrammer", cube_path)
    if shutil.which("st-flash"):
        return Flasher("st-flash", Path(shutil.which("st-flash")))
    if shutil.which("openocd"):
        return Flasher("OpenOCD", Path(shutil.which("openocd")))
    return None


def extra_tool_dirs() -> list[str]:
    """Pastas do CubeIDE a acrescentar ao PATH (no fim: ferramentas do sistema tem prioridade)."""
    dirs = []
    for tool in (find_arm_gcc(), find_ninja(), find_cmake()):
        if tool and not shutil.which(tool.name):
            dirs.append(str(tool.parent))
    return dirs


def append_tools_to_path() -> None:
    extra = [d for d in extra_tool_dirs() if d not in os.environ.get("PATH", "")]
    if extra:
        os.environ["PATH"] = os.pathsep.join([os.environ.get("PATH", ""), *extra])
