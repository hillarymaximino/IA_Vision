"""Ponto de entrada: ``python3 tools/sicov_launcher`` (a partir de "Apresentação Esteira")."""

import sys
from pathlib import Path

# Executado como diretorio, o Python nao trata a pasta como pacote; colocamos
# o pai no sys.path para que os imports relativos de sicov_launcher funcionem.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from sicov_launcher.bootstrap import ensure_environment  # noqa: E402

# Instala o PySide6 num venv local na primeira execucao (somente stdlib ate aqui)
ensure_environment(Path(__file__).resolve().parent)

from sicov_launcher.app import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
