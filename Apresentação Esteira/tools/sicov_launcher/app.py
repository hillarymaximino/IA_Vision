"""Janela principal do SICOV Launcher."""

from __future__ import annotations

import html
import os
import shutil
import sys
import zipfile
from pathlib import Path

from PySide6.QtCore import QSettings, Qt
from PySide6.QtGui import QAction, QCloseEvent, QFontDatabase
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFileDialog, QFormLayout, QGroupBox, QHBoxLayout, QLabel,
    QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QScrollArea, QSplitter,
    QToolButton, QVBoxLayout, QWidget,
)

from . import __version__
from .checks import CheckResult, LauncherConfig, Scope, blocking_failures, run_all
from .firmware import append_tools_to_path, find_flasher
from .installer import GALAXY_HINT, build_plan
from .project import IS_WINDOWS, MODEL_FILENAME, ProjectLayout, list_serial_ports
from .runner import Exchange, ExpectScript, LogKind, ProcessRunner, Step

_LOG_COLORS = {
    LogKind.INFO: "#1e88e5",
    LogKind.SUCCESS: "#43a047",
    LogKind.WARNING: "#fb8c00",
    LogKind.ERROR: "#e53935",
}

# Textos impressos pelo bridge_example (bridge_host/example/demo_bridge.cpp)
_BRIDGE_PROMPT = "bridge >"
_BRIDGE_OK = "Handshake Mútuo Concluído com Sucesso"
_BRIDGE_FAIL = "Falha no Handshake"
_BRIDGE_NOT_CONNECTED = "Placa não conectada"

_FLASH_HINT = ("Gravação falhou. Confira: ST-Link no USB e ligado ao SWD da placa (SWDIO, SWCLK, GND); "
               "placa alimentada; nenhuma sessão de debug do STM32CubeIDE aberta. No Linux, após instalar "
               "o stlink-tools, desconecte e reconecte o ST-Link para aplicar as regras udev.")


class _PathField(QWidget):
    """QLineEdit + botao '...' para escolher arquivo ou pasta."""

    def __init__(self, pick_directory: bool, file_filter: str = "", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.edit = QLineEdit()
        button = QToolButton(text="...")
        button.clicked.connect(self._browse)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.edit)
        layout.addWidget(button)
        self._pick_directory = pick_directory
        self._filter = file_filter

    def _browse(self) -> None:
        start = self.edit.text() or str(Path.home())
        if self._pick_directory:
            chosen = QFileDialog.getExistingDirectory(self, "Selecionar pasta", start)
        else:
            chosen, _ = QFileDialog.getOpenFileName(self, "Selecionar arquivo", start, self._filter)
        if chosen:
            self.edit.setText(chosen)

    def text(self) -> str:
        return self.edit.text().strip()

    def setText(self, value: str) -> None:  # noqa: N802 (API Qt)
        self.edit.setText(value)


class MainWindow(QMainWindow):
    def __init__(self, layout: ProjectLayout) -> None:
        super().__init__()
        self.layout_ = layout
        self.settings = QSettings("ASSERT-IFPB", "SICOV Launcher")
        self.runner = ProcessRunner(self)
        self.runner.log.connect(self._append_log)
        self.runner.step_started.connect(self._on_step_started)
        self.runner.busy_changed.connect(self._on_busy_changed)

        self.setWindowTitle(f"SICOV Launcher {__version__} — Esteira de Inspeção")
        self.resize(1200, 760)
        self._build_ui()
        self._load_settings()
        self.refresh_ports()
        self._append_log(f"Projeto: {layout.root}", LogKind.INFO)
        self._append_log("Comece por 'Verificar ambiente'.", LogKind.INFO)

    # ================================================================== UI
    def _build_ui(self) -> None:
        # ---- Configuracao
        self.galaxy_field = _PathField(pick_directory=True)
        self.opencv_field = _PathField(pick_directory=True)
        self.model_field = _PathField(pick_directory=False, file_filter="Modelo ONNX (*.onnx)")
        self.port_combo = QComboBox(editable=True)
        self.port_combo.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
        refresh_ports = QToolButton(text="↻", toolTip="Atualizar portas")
        refresh_ports.clicked.connect(self.refresh_ports)
        port_row = QWidget()
        port_layout = QHBoxLayout(port_row)
        port_layout.setContentsMargins(0, 0, 0, 0)
        port_layout.addWidget(self.port_combo, 1)
        port_layout.addWidget(refresh_ports)
        self.camera_edit = QLineEdit()
        self.camera_edit.setPlaceholderText("vazio = primeira câmera encontrada")

        config_box = QGroupBox("Configuração")
        form = QFormLayout(config_box)
        form.addRow("Galaxy SDK:", self.galaxy_field)
        form.addRow("OpenCV_DIR (opcional):", self.opencv_field)
        form.addRow("Modelo ONNX:", self.model_field)
        form.addRow("Porta do STM32:", port_row)
        form.addRow("SN da câmera:", self.camera_edit)

        # ---- Grupos de acoes
        self._action_buttons: list[QPushButton] = []
        env_box = self._button_group("1. Ambiente", [
            ("Verificar ambiente", self.check_environment),
            ("Instalar dependências", self.install_dependencies),
            ("Extrair modelo do zip", self.extract_model),
        ])
        build_box = self._button_group("2. Build", [
            ("Compilar tudo (visão + bridge)", lambda: self.build(vision=True)),
            ("Compilar só a bridge", lambda: self.build(vision=False)),
            ("Limpar build", self.clean_build),
        ])
        run_box = self._button_group("3. Executar", [
            ("Rodar visão (VisionTest)", self.run_vision),
            ("Terminal da bridge", self.run_bridge_terminal),
        ])
        firmware_box = self._button_group("5. Firmware STM32", [
            ("Compilar firmware", self.build_firmware),
            ("Gravar no STM32 (compila + grava)", self.flash_firmware),
        ])
        test_box = self._button_group("4. Testes", [
            ("Testar handshake com o STM32", self.test_handshake),
            ("Fluxo completo (verificar → compilar → testar)", self.full_flow),
        ])

        self.stop_button = QPushButton("■ Parar processo")
        self.stop_button.setEnabled(False)
        self.stop_button.clicked.connect(self.runner.stop)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        for widget in (config_box, env_box, build_box, run_box, test_box, firmware_box, self.stop_button):
            left_layout.addWidget(widget)
        left_layout.addStretch(1)
        scroll = QScrollArea(widgetResizable=True)
        scroll.setWidget(left)
        # Os campos se ajustam a largura; so a rolagem vertical faz sentido
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setMinimumWidth(460)

        # ---- Log + entrada interativa
        self.log_view = QPlainTextEdit(readOnly=True)
        self.log_view.setMaximumBlockCount(20000)
        mono = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        mono.setPointSize(max(mono.pointSize(), 10))
        self.log_view.setFont(mono)

        self.input_edit = QLineEdit(placeholderText="Comando para o processo interativo (Enter envia)")
        self.input_edit.setEnabled(False)
        self.input_edit.returnPressed.connect(self._send_input)

        clear_log = QPushButton("Limpar log")
        clear_log.clicked.connect(self.log_view.clear)
        save_log = QPushButton("Salvar log...")
        save_log.clicked.connect(self._save_log)

        input_row = QHBoxLayout()
        input_row.addWidget(self.input_edit, 1)
        input_row.addWidget(clear_log)
        input_row.addWidget(save_log)

        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.addWidget(QLabel("<b>Saída</b>"))
        right_layout.addWidget(self.log_view, 1)
        right_layout.addLayout(input_row)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(scroll)
        splitter.addWidget(right)
        splitter.setStretchFactor(1, 1)
        self.setCentralWidget(splitter)

        quit_action = QAction("Sair", self, shortcut="Ctrl+Q", triggered=self.close)
        self.addAction(quit_action)
        self.statusBar().showMessage("Pronto")

    def _button_group(self, title: str, actions: list[tuple[str, object]]) -> QGroupBox:
        box = QGroupBox(title)
        layout = QVBoxLayout(box)
        for text, slot in actions:
            button = QPushButton(text)
            button.clicked.connect(slot)
            layout.addWidget(button)
            self._action_buttons.append(button)
        return box

    # ============================================================ settings
    def _load_settings(self) -> None:
        defaults = LauncherConfig()
        self.galaxy_field.setText(self.settings.value("galaxy_sdk_dir", defaults.galaxy_sdk_dir))
        self.opencv_field.setText(self.settings.value("opencv_dir", os.environ.get("OpenCV_DIR", "")))
        self.model_field.setText(self.settings.value("model_path", str(self.layout_.default_model)))
        self.camera_edit.setText(self.settings.value("camera_sn", defaults.camera_sn))
        self._saved_port = self.settings.value("serial_port", "")

    def _save_settings(self) -> None:
        cfg = self.config()
        for key, value in vars(cfg).items():
            self.settings.setValue(key, value)

    def config(self) -> LauncherConfig:
        return LauncherConfig(
            galaxy_sdk_dir=self.galaxy_field.text(),
            opencv_dir=self.opencv_field.text(),
            serial_port=self.port_combo.currentText().split("  ")[0].strip(),
            camera_sn=self.camera_edit.text().strip(),
            model_path=self.model_field.text(),
        )

    # =============================================================== log
    def _append_log(self, text: str, kind: LogKind) -> None:
        escaped = html.escape(text) or "&nbsp;"
        color = _LOG_COLORS.get(kind)
        line = f'<span style="color:{color}">{escaped}</span>' if color else escaped
        self.log_view.appendHtml(f'<pre style="margin:0">{line}</pre>')

    def _save_log(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Salvar log", "sicov_launcher.log", "Log (*.log *.txt)")
        if path:
            Path(path).write_text(self.log_view.toPlainText(), encoding="utf-8")

    def _send_input(self) -> None:
        text = self.input_edit.text()
        self.input_edit.clear()
        self.runner.send_line(text)

    def _on_step_started(self, title: str, interactive: bool) -> None:
        self.statusBar().showMessage(f"Executando: {title}")
        self.input_edit.setEnabled(interactive)
        if interactive:
            self.input_edit.setFocus()

    def _on_busy_changed(self, busy: bool) -> None:
        for button in self._action_buttons:
            button.setEnabled(not busy)
        self.stop_button.setEnabled(busy)
        if not busy:
            self.input_edit.setEnabled(False)
            self.statusBar().showMessage("Pronto")

    # ============================================================ acoes
    def refresh_ports(self) -> None:
        current = self.port_combo.currentText() or getattr(self, "_saved_port", "")
        self.port_combo.clear()
        for port, description in list_serial_ports():
            self.port_combo.addItem(f"{port}  {description}".strip())
        if current:
            index = self.port_combo.findText(current, Qt.MatchFlag.MatchStartsWith)
            if index >= 0:
                self.port_combo.setCurrentIndex(index)
            else:
                self.port_combo.setEditText(current)
        if self.port_combo.count() == 0 and not current:
            hint = "COM5" if IS_WINDOWS else "/dev/ttyACM0"
            self.port_combo.lineEdit().setPlaceholderText(f"nenhuma porta encontrada (ex: {hint})")

    def _report_checks(self, results: list[CheckResult]) -> None:
        self._append_log("== Verificação do ambiente", LogKind.INFO)
        for result in results:
            mark, kind = ("✓", LogKind.SUCCESS) if result.ok else ("✗", LogKind.ERROR)
            self._append_log(f"{mark} {result.name}: {result.detail}", kind)
            if result.hint:
                self._append_log(f"    → {result.hint}", LogKind.WARNING)
        failures = [r for r in results if not r.ok]
        if any(r.name == "Galaxy SDK" for r in failures):
            self._append_log(f"    → {GALAXY_HINT}", LogKind.WARNING)
        summary = "Ambiente completo." if not failures else f"{len(failures)} item(ns) pendente(s)."
        self._append_log(summary, LogKind.SUCCESS if not failures else LogKind.WARNING)

    def check_environment(self) -> list[CheckResult]:
        self._save_settings()
        results = run_all(self.config(), self.layout_)
        self._report_checks(results)
        return results

    def _confirm_install(self, results: list[CheckResult]) -> list[Step] | None:
        fixes = {r.fix for r in results if not r.ok and r.fix is not None}
        plan = build_plan(fixes, self.layout_.root)
        for line in plan.manual:
            self._append_log(line, LogKind.WARNING)
        if not plan.steps:
            if not plan.manual:
                self._append_log("Nada para instalar automaticamente.", LogKind.INFO)
            return None
        text = "Serão executados:\n\n• " + "\n• ".join(plan.description)
        answer = QMessageBox.question(self, "Instalar dependências", text + "\n\nContinuar?")
        return plan.steps if answer == QMessageBox.StandardButton.Yes else None

    def install_dependencies(self) -> None:
        results = self.check_environment()
        steps = self._confirm_install(results)
        if steps:
            self.runner.run(steps)

    def extract_model(self) -> None:
        zip_path = self.layout_.model_zip
        if not zip_path.exists():
            chosen, _ = QFileDialog.getOpenFileName(self, "Selecionar o zip com o modelo", str(self.layout_.root),
                                                    "Zip (*.zip)")
            if not chosen:
                return
            zip_path = Path(chosen)
        target = self.layout_.default_model
        try:
            with zipfile.ZipFile(zip_path) as archive:
                member = next((n for n in archive.namelist() if n.endswith(f"models/{MODEL_FILENAME}")), None)
                if member is None:
                    self._append_log(f"{MODEL_FILENAME} não está em {zip_path.name}.", LogKind.ERROR)
                    return
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)
        except (OSError, zipfile.BadZipFile) as error:
            self._append_log(f"Falha ao extrair o modelo: {error}", LogKind.ERROR)
            return
        self.model_field.setText(str(target))
        self._append_log(f"Modelo extraído em {target}", LogKind.SUCCESS)

    def _build_steps(self, vision: bool) -> list[Step]:
        cfg = self.config()
        preset = self.layout_.preset(vision)
        configure = ["--preset", preset, f"-DSICOV_BUILD_VISION={'ON' if vision else 'OFF'}"]
        if vision and cfg.galaxy_sdk_dir:
            configure.append(f"-DGALAXY_SDK_DIR={cfg.galaxy_sdk_dir}")
        if vision and cfg.opencv_dir:
            configure.append(f"-DOpenCV_DIR={cfg.opencv_dir}")
        jobs = str(os.cpu_count() or 2)
        root = self.layout_.root
        return [
            Step(f"Configurar ({preset})", "cmake", configure, root),
            Step(f"Compilar ({preset})", "cmake", ["--build", "--preset", preset, "--parallel", jobs], root),
        ]

    def _preflight(self, scopes: set[Scope]) -> bool:
        """Roda as checagens e bloqueia se faltar algo necessario para `scopes`."""
        results = self.check_environment()
        failures = blocking_failures(results, scopes)
        if not failures:
            return True
        names = ", ".join(f.name for f in failures)
        if any(f.fix for f in failures):
            self._append_log(f"Pendente: {names}. Use 'Instalar dependências'.", LogKind.ERROR)
        else:
            self._append_log(f"Pendente: {names}. Corrija a configuração acima.", LogKind.ERROR)
        return False

    def build(self, vision: bool) -> None:
        scopes = {Scope.BRIDGE, Scope.VISION} if vision else {Scope.BRIDGE}
        if self._preflight(scopes):
            self.runner.run(self._build_steps(vision))

    def clean_build(self) -> None:
        build_root = self.layout_.root / "build"
        if not build_root.exists():
            self._append_log("Não há pasta build/ para limpar.", LogKind.INFO)
            return
        answer = QMessageBox.question(self, "Limpar build", f"Apagar {build_root}?")
        if answer == QMessageBox.StandardButton.Yes:
            shutil.rmtree(build_root, ignore_errors=True)
            self._append_log(f"Removido {build_root}", LogKind.SUCCESS)

    def _vision_step(self) -> Step | None:
        exe = self.layout_.vision_executable()
        if not exe.exists():
            self._append_log(f"{exe} não existe. Compile primeiro (2. Build).", LogKind.ERROR)
            return None
        cfg = self.config()
        args = ["--model", cfg.model_path or str(self.layout_.default_model), "--camera-sn", cfg.camera_sn]
        if cfg.serial_port:
            args += ["--port", cfg.serial_port]
        return Step("Visão (VisionTest)", str(exe), args, self.layout_.visao_dir)

    def run_vision(self) -> None:
        self._save_settings()
        if not self._preflight({Scope.MODEL}):
            return
        step = self._vision_step()
        if step:
            self._append_log("Dica: abra o VisionTest e depois aperte RESET no STM32 (0xB1 do boot).",
                             LogKind.WARNING)
            self.runner.run([step])

    def _bridge_step(self, scripted: bool, require_built: bool = True) -> Step | None:
        exe = self.layout_.bridge_executable()
        if require_built and not exe.exists():
            self._append_log(f"{exe} não existe. Compile primeiro (2. Build).", LogKind.ERROR)
            return None
        port = self.config().serial_port
        args = [port] if port else []
        if not scripted:
            return Step("Terminal da bridge", str(exe), args, self.layout_.root, interactive=True)
        script = ExpectScript(
            ready=_BRIDGE_PROMPT,
            exchanges=[Exchange("1", _BRIDGE_OK, _BRIDGE_FAIL),   # 0xA1 -> PD7 = 0
                       Exchange("0", _BRIDGE_OK, _BRIDGE_FAIL)],  # 0xA0 -> PD7 = 1
            abort_markers=[_BRIDGE_NOT_CONNECTED],
        )
        return Step("Teste de handshake (0xA1 / 0xA0)", str(exe), args, self.layout_.root, script=script)

    def run_bridge_terminal(self) -> None:
        self._save_settings()
        step = self._bridge_step(scripted=False)
        if step:
            self._append_log("Comandos: 1 (0xA1), 0 (0xA0), v (varredura), s (status), q (sair).", LogKind.INFO)
            self.runner.run([step])

    def test_handshake(self) -> None:
        self._save_settings()
        if not self._preflight({Scope.SERIAL}):
            return
        step = self._bridge_step(scripted=True)
        if step:
            self.runner.run([step])

    def _firmware_build_steps(self) -> list[Step]:
        cwd = self.layout_.firmware_dir
        return [
            Step("Configurar firmware", "cmake", ["--preset", "firmware"], cwd),
            Step("Compilar firmware", "cmake", ["--build", "--preset", "firmware"], cwd),
        ]

    def build_firmware(self) -> None:
        if self._preflight({Scope.FIRMWARE}):
            self.runner.run(self._firmware_build_steps())

    def flash_firmware(self) -> None:
        """Compila o firmware e grava pelo ST-Link (SWD); o STM32 reinicia ao final."""
        if not self._preflight({Scope.FIRMWARE, Scope.FLASH}):
            return
        flasher = find_flasher()
        assert flasher is not None  # garantido pelo preflight
        answer = QMessageBox.question(
            self, "Gravar firmware",
            f"Compilar e gravar o firmware no STM32 usando {flasher.name}?\n\n"
            "O programa atual da placa será substituído. Feche sessões de debug do "
            "STM32CubeIDE antes de continuar.")
        if answer != QMessageBox.StandardButton.Yes:
            return
        elf = self.layout_.firmware_artifact(".elf")
        binary = self.layout_.firmware_artifact(".bin")
        steps = self._firmware_build_steps()
        steps.append(Step(f"Gravar no STM32 ({flasher.name})", str(flasher.program),
                          flasher.command(elf, binary), self.layout_.firmware_dir,
                          failure_hint=_FLASH_HINT))
        self._append_log("Após gravar, o STM32 reinicia e envia 0xB1 (varredura liberada).", LogKind.INFO)
        self.runner.run(steps)

    def full_flow(self) -> None:
        """Verifica, instala o que for automatizavel, compila tudo e testa o STM32."""
        self._save_settings()
        results = self.check_environment()

        manual_blockers = [r for r in blocking_failures(results, {Scope.BRIDGE, Scope.VISION})
                           if r.fix is None]
        if manual_blockers:
            names = ", ".join(r.name for r in manual_blockers)
            self._append_log(f"Fluxo interrompido: {names} precisa(m) de configuração manual.", LogKind.ERROR)
            return

        steps: list[Step] = []
        if any(not r.ok and r.fix for r in results):
            install = self._confirm_install(results)
            if install is None:
                self._append_log("Fluxo cancelado: dependências não instaladas.", LogKind.WARNING)
                return
            steps += install

        steps += self._build_steps(vision=True)

        if not blocking_failures(results, {Scope.SERIAL}):
            # O executavel ainda nao existe: sera gerado pelos passos de build acima
            steps.append(self._bridge_step(scripted=True, require_built=False))
        else:
            self._append_log("Teste do STM32 pulado: porta serial indisponível (veja os itens acima).",
                             LogKind.WARNING)
        self.runner.run(steps)

    # ============================================================ janela
    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 (API Qt)
        self._save_settings()
        if self.runner.busy:
            answer = QMessageBox.question(self, "Sair", "Há um processo em execução. Encerrar e sair?")
            if answer != QMessageBox.StandardButton.Yes:
                event.ignore()
                return
            self.runner.stop()
        event.accept()


def _prepend_venv_to_path() -> None:
    """Ferramentas instaladas via pip no venv (ex: cmake) ficam visiveis aos processos."""
    scripts = Path(sys.executable).parent
    os.environ["PATH"] = f"{scripts}{os.pathsep}{os.environ.get('PATH', '')}"


def main() -> int:
    _prepend_venv_to_path()
    append_tools_to_path()  # ferramentas do STM32CubeIDE como reserva
    app = QApplication(sys.argv)
    app.setApplicationName("SICOV Launcher")
    window = MainWindow(ProjectLayout.detect())
    window.show()
    return app.exec()
