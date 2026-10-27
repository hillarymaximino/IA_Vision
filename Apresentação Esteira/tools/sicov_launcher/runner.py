"""Execucao assincrona de comandos (QProcess) em fila, com dialogo automatico opcional."""

from __future__ import annotations

import codecs
import re
import shlex
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, QTimer, Signal

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


class LogKind(Enum):
    OUTPUT = "output"
    INFO = "info"
    SUCCESS = "success"
    WARNING = "warning"
    ERROR = "error"


@dataclass
class Exchange:
    """Uma rodada do dialogo: envia `send` e espera `success` ou `failure` na saida."""

    send: str
    success: str
    failure: str


@dataclass
class ExpectScript:
    """Conversa automatica com um processo interativo (ex: bridge_example)."""

    ready: str                              # texto que indica que o processo aceita comandos
    exchanges: list[Exchange]
    abort_markers: list[str] = field(default_factory=list)
    quit_command: str = "q"
    timeout_ms: int = 8000


@dataclass
class Step:
    title: str
    program: str
    args: list[str]
    cwd: Path
    env: dict[str, str] = field(default_factory=dict)
    interactive: bool = False               # habilita a caixa de entrada do log
    script: ExpectScript | None = None
    failure_hint: str = ""                  # exibido se este passo falhar
    # Avaliado na hora de executar (nao ao montar a fila): permite decidir com base
    # no resultado de passos anteriores, ex: gravar so se o firmware compilado mudou.
    should_run: Callable[[], bool] | None = None
    skip_message: str = "nada a fazer"
    on_success: Callable[[], None] | None = None   # chamado apos terminar com codigo 0


class _ScriptState:
    def __init__(self, script: ExpectScript) -> None:
        self.script = script
        self.buffer = ""
        self.started = False
        self.index = 0
        self.passed = 0
        self.failed_reason = ""

    @property
    def done(self) -> bool:
        return bool(self.failed_reason) or self.index >= len(self.script.exchanges)

    @property
    def ok(self) -> bool:
        return not self.failed_reason and self.passed == len(self.script.exchanges)


class ProcessRunner(QObject):
    """Executa uma fila de Steps em sequencia; aborta no primeiro erro."""

    log = Signal(str, LogKind)
    step_started = Signal(str, bool)        # titulo, interativo
    finished = Signal(bool, str)            # sucesso, resumo
    busy_changed = Signal(bool)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._queue: list[Step] = []
        self._current: Step | None = None
        self._process: QProcess | None = None
        self._decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self._pending_line = ""
        self._script: _ScriptState | None = None
        self._stopped_by_user = False

        # Prompts sem '\n' (ex: "bridge > ") aparecem apos um curto silencio
        self._flush_timer = QTimer(self, singleShot=True, interval=150)
        self._flush_timer.timeout.connect(self._flush_pending)
        self._script_timer = QTimer(self, singleShot=True)
        self._script_timer.timeout.connect(self._on_script_timeout)

    # ------------------------------------------------------------------ API
    @property
    def busy(self) -> bool:
        return self._current is not None

    def run(self, steps: list[Step]) -> None:
        if self.busy:
            self.log.emit("Ja existe um processo em execucao. Pare-o antes.", LogKind.WARNING)
            return
        self._queue = list(steps)
        self._stopped_by_user = False
        self.busy_changed.emit(True)
        self._start_next()

    def stop(self) -> None:
        self._queue.clear()
        if self._process and self._process.state() != QProcess.ProcessState.NotRunning:
            self._stopped_by_user = True
            self.log.emit("Encerrando processo...", LogKind.WARNING)
            self._process.terminate()
            self._schedule_kill(self._process)

    def send_line(self, text: str) -> None:
        if self._process and self._process.state() == QProcess.ProcessState.Running:
            self.log.emit(f"> {text}", LogKind.INFO)
            self._process.write((text + "\n").encode("utf-8"))

    # ------------------------------------------------------------ internos
    def _start_next(self) -> None:
        if not self._queue:
            self._finish(True, "Concluido com sucesso.")
            return

        step = self._queue.pop(0)
        if step.should_run is not None and not step.should_run():
            self.log.emit(f"== {step.title}: pulado ({step.skip_message})", LogKind.INFO)
            self._start_next()
            return

        self._current = step
        self._decoder.reset()
        self._pending_line = ""
        self._script = _ScriptState(step.script) if step.script else None

        process = QProcess(self)
        process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        process.setWorkingDirectory(str(step.cwd))
        env = QProcessEnvironment.systemEnvironment()
        for key, value in step.env.items():
            env.insert(key, value)
        process.setProcessEnvironment(env)
        process.readyReadStandardOutput.connect(self._on_output)
        process.finished.connect(self._on_finished)
        process.errorOccurred.connect(self._on_error)
        self._process = process

        self.step_started.emit(step.title, step.interactive)
        self.log.emit(f"== {step.title}", LogKind.INFO)
        self.log.emit(f"$ {shlex.join([step.program, *step.args])}   (em {step.cwd})", LogKind.INFO)
        if self._script:
            self._script_timer.start(self._script.script.timeout_ms)
        process.start(step.program, step.args)

    def _on_output(self) -> None:
        assert self._process is not None
        text = self._decoder.decode(bytes(self._process.readAllStandardOutput()))
        text = ANSI_ESCAPE.sub("", text).replace("\r", "")
        if self._script:
            self._feed_script(text)

        lines = (self._pending_line + text).split("\n")
        self._pending_line = lines.pop()
        for line in lines:
            self.log.emit(line, LogKind.OUTPUT)
        if self._pending_line:
            self._flush_timer.start()

    def _flush_pending(self) -> None:
        if self._pending_line:
            self.log.emit(self._pending_line, LogKind.OUTPUT)
            self._pending_line = ""

    def _feed_script(self, text: str) -> None:
        state = self._script
        assert state is not None
        state.buffer += text

        for marker in state.script.abort_markers:
            if marker in state.buffer and not state.done:
                state.failed_reason = f"processo reportou: '{marker}'"
                self._end_script()
                return

        if not state.started:
            if state.script.ready not in state.buffer:
                return
            state.started = True
            state.buffer = ""
            self._send_current_exchange()
            return

        if state.done:
            return
        exchange = state.script.exchanges[state.index]
        if exchange.success in state.buffer:
            state.passed += 1
            self.log.emit(f"OK: '{exchange.send}' confirmado", LogKind.SUCCESS)
            state.index += 1
            state.buffer = ""
            if state.done:
                self._end_script()
            else:
                self._send_current_exchange()
        elif exchange.failure in state.buffer:
            state.failed_reason = f"comando '{exchange.send}' falhou"
            self._end_script()

    def _send_current_exchange(self) -> None:
        assert self._script is not None
        self._script_timer.start(self._script.script.timeout_ms)
        self.send_line(self._script.script.exchanges[self._script.index].send)

    def _end_script(self) -> None:
        assert self._script is not None
        self._script_timer.stop()
        self.send_line(self._script.script.quit_command)
        if self._process:
            self._schedule_kill(self._process)

    def _on_script_timeout(self) -> None:
        if self._script and not self._script.done:
            stage = "aguardando o processo ficar pronto" if not self._script.started else \
                f"aguardando resposta de '{self._script.script.exchanges[self._script.index].send}'"
            self._script.failed_reason = f"timeout {stage}"
            self._end_script()

    def _on_error(self, error: QProcess.ProcessError) -> None:
        if error == QProcess.ProcessError.FailedToStart and self._current:
            self.log.emit(f"Nao foi possivel iniciar '{self._current.program}'. "
                          "O programa existe? Ja foi compilado?", LogKind.ERROR)
            self._queue.clear()
            self._finish(False, f"Falha ao iniciar: {self._current.title}")

    def _on_finished(self, exit_code: int, exit_status: QProcess.ExitStatus) -> None:
        if self._current is None:
            return  # ja tratado em _on_error
        self._flush_timer.stop()
        self._flush_pending()
        step = self._current

        if self._stopped_by_user:
            self._finish(False, f"{step.title}: interrompido pelo usuario.")
            return

        if self._script:
            self._script_timer.stop()
            if self._script.ok:
                self.log.emit(f"{step.title}: {self._script.passed} troca(s) confirmada(s).", LogKind.SUCCESS)
                self._start_next()
            else:
                reason = self._script.failed_reason or "processo terminou antes do fim do teste"
                self._queue.clear()
                self._finish(False, f"{step.title}: FALHOU ({reason})")
            return

        if exit_status == QProcess.ExitStatus.NormalExit and exit_code == 0:
            self.log.emit(f"{step.title}: ok", LogKind.SUCCESS)
            if step.on_success is not None:
                step.on_success()
            self._start_next()
        else:
            self._queue.clear()
            if step.failure_hint:
                self.log.emit(step.failure_hint, LogKind.WARNING)
            self._finish(False, f"{step.title}: terminou com codigo {exit_code}.")

    def _schedule_kill(self, process: QProcess, delay_ms: int = 3000) -> None:
        """Mata `process` se ainda estiver vivo apos o prazo.

        O alvo e fixado agora: se outro passo comecar nesse intervalo, o timer
        nao pode atingir o processo novo.
        """
        def kill() -> None:
            if process.state() != QProcess.ProcessState.NotRunning:
                process.kill()
        QTimer.singleShot(delay_ms, process, kill)

    def _finish(self, ok: bool, summary: str) -> None:
        self._script_timer.stop()
        self._current = None
        self._script = None
        self.log.emit(summary, LogKind.SUCCESS if ok else LogKind.ERROR)
        self.finished.emit(ok, summary)
        self.busy_changed.emit(False)
