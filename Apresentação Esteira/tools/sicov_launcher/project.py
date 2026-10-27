"""Layout do projeto, presets do CMake e descoberta de portas seriais."""

from __future__ import annotations

import glob
import os
import sys
from dataclasses import dataclass
from pathlib import Path

IS_WINDOWS = sys.platform.startswith("win")

MODEL_FILENAME = "yolo11_formas.onnx"
MODEL_ZIP_NAME = "Vision_Geometrica (Cópia).zip"


@dataclass(frozen=True)
class ProjectLayout:
    """Caminhos derivados da raiz "Apresentação Esteira" (onde fica o CMakePresets.json)."""

    root: Path

    @classmethod
    def detect(cls) -> "ProjectLayout":
        # tools/sicov_launcher/project.py -> raiz = dois niveis acima de tools/
        return cls(Path(__file__).resolve().parents[2])

    @property
    def visao_dir(self) -> Path:
        return self.root / "visao"

    @property
    def firmware_dir(self) -> Path:
        return self.root / "firmware_stm32"

    def firmware_artifact(self, suffix: str) -> Path:
        """Saida do preset 'firmware' (ex: suffix='.elf' -> build/comEdge.elf)."""
        return self.firmware_dir / "build" / f"comEdge{suffix}"

    @property
    def default_model(self) -> Path:
        return self.visao_dir / "models" / MODEL_FILENAME

    @property
    def model_zip(self) -> Path:
        # O zip de backup fica na raiz do repositorio IA_Vision
        return self.root.parent / MODEL_ZIP_NAME

    @staticmethod
    def preset(vision: bool) -> str:
        if IS_WINDOWS:
            return "windows" if vision else "windows-bridge"
        return "linux"

    def build_dir(self, preset: str) -> Path:
        return self.root / "build" / preset

    def _executable(self, preset: str, subdir: str, name: str) -> Path:
        base = self.build_dir(preset) / subdir
        # Gerador Visual Studio e multi-config: o binario fica em Release/
        return base / "Release" / f"{name}.exe" if IS_WINDOWS else base / name

    def vision_executable(self) -> Path:
        return self._executable(self.preset(vision=True), "visao", "VisionTest")

    def bridge_executable(self) -> Path:
        """Prefere o build completo; cai para o build so da bridge (Windows)."""
        candidates = [
            self._executable(self.preset(vision=True), "bridge_host", "bridge_example"),
            self._executable(self.preset(vision=False), "bridge_host", "bridge_example"),
        ]
        for path in candidates:
            if path.exists():
                return path
        return candidates[0]


def list_serial_ports() -> list[tuple[str, str]]:
    """Retorna (porta, descricao) das portas candidatas a STM32."""
    if IS_WINDOWS:
        try:
            from serial.tools import list_ports  # pyserial e opcional
        except ImportError:
            return []
        return [(p.device, p.description or "") for p in list_ports.comports()]

    # /dev/serial/by-id da nomes estaveis com fabricante (ex: usb-STMicroelectronics_STLINK...)
    by_id = {
        os.path.realpath(link): os.path.basename(link)
        for link in glob.glob("/dev/serial/by-id/*")
    }
    ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    return [(port, by_id.get(port, "")) for port in ports]
