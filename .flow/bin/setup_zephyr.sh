#!/usr/bin/env bash
# Prepara o ambiente Zephyr da sessao (Dev ou Q.A.).
#
# Por padrao instala o suficiente para `west twister -p native_sim`, que usa o
# gcc do host — sem baixar o Zephyr SDK (~1,5 GB). Isso valida toda a logica,
# os Kconfig e a matriz de modularidade.
#
#   --sdk   baixa tambem o Zephyr SDK (necessario so para compilar para
#           rpi_pico / esp32 de verdade). Em geral nao precisa: as builds de
#           board real acontecem no GitHub Actions, que ja tem o SDK na imagem.
#
# Idempotente: rodar de novo nao refaz o que ja esta pronto.
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WS="${ZEPHYR_WS:-$HOME/zephyrproject}"
COM_SDK=0
[[ "${1:-}" == "--sdk" ]] && COM_SDK=1

log() { printf '\033[1m[setup]\033[0m %s\n' "$*"; }
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo

# ---------------------------------------------------------------- pacotes
if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
  log "instalando dependencias de sistema"
  $SUDO apt-get update -qq
  $SUDO apt-get install -y --no-install-recommends \
    git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget \
    python3-dev python3-venv python3-pip xz-utils file make gcc gcc-multilib \
    g++-multilib libsdl2-dev libmagic1 >/dev/null
fi

# ---------------------------------------------------------------- venv+west
# Zephyr >= 4.x exige Python 3.12+. O `python3` do PATH pode ser mais antigo:
# escolha explicitamente o mais novo disponivel, ou o CMake do Zephyr aborta
# com "Could NOT find Python3: Found unsuitable version".
PY=""
for v in 3.13 3.12; do
  command -v "python$v" >/dev/null && { PY="python$v"; break; }
done
if [[ -z "$PY" ]]; then
  if python3 -c 'import sys; sys.exit(0 if sys.version_info>=(3,12) else 1)'; then
    PY=python3
  else
    log "instalando python3.12"
    $SUDO apt-get install -y python3.12 python3.12-venv python3.12-dev >/dev/null
    PY=python3.12
  fi
fi
log "usando $PY ($($PY --version))"

if [[ ! -d "$WS/.venv" ]]; then
  log "criando venv em $WS/.venv"
  mkdir -p "$WS"
  "$PY" -m venv "$WS/.venv"
fi
# shellcheck disable=SC1091
source "$WS/.venv/bin/activate"
pip install -q --upgrade pip
command -v west >/dev/null || { log "instalando west"; pip install -q west; }

# ---------------------------------------------------------------- workspace
if [[ ! -d "$WS/.west" ]]; then
  if [[ -f "$APP_DIR/west.yml" ]]; then
    log "workspace T2 a partir do west.yml da aplicacao"
    west init -l "$APP_DIR" 2>/dev/null || west init -l --mf west.yml "$APP_DIR"
  else
    log "aplicacao sem west.yml — criando workspace padrao do Zephyr"
    west init "$WS"   # --narrow e opcao de `west update`, nao de `west init`
  fi
fi

cd "$WS"
if [[ $COM_SDK -eq 0 ]]; then
  # native_sim nao usa HAL de vendor. Sem este filtro o `west update` baixa
  # ~30 modulos e passa de 6 GB; com ele fica na casa de 1 GB.
  log "filtrando grupos do manifesto (hal/tools/debug/optional/bsim)"
  west config manifest.group-filter -- -hal,-tools,-debug,-optional,-bsim || true
fi
if [[ ! -d "$WS/zephyr" ]]; then
  log "west update (so isto demora alguns minutos)"
  west update --narrow -o=--depth=1
fi
west zephyr-export >/dev/null 2>&1 || true

if [[ -f "$WS/zephyr/scripts/requirements.txt" ]]; then
  log "requisitos python do Zephyr"
  pip install -q -r "$WS/zephyr/scripts/requirements.txt"
fi

# ---------------------------------------------------------------- toolchain
ENVFILE="$WS/.flow-env"
{
  echo "export ZEPHYR_BASE=\"$WS/zephyr\""
  echo "source \"$WS/.venv/bin/activate\""
} > "$ENVFILE"

if [[ $COM_SDK -eq 1 ]]; then
  SDK_VER="${ZEPHYR_SDK_VERSION:-0.17.0}"
  if [[ ! -d "$HOME/zephyr-sdk-$SDK_VER" ]]; then
    log "baixando Zephyr SDK $SDK_VER (grande)"
    wget -q -O /tmp/sdk.tar.xz \
      "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz"
    tar xf /tmp/sdk.tar.xz -C "$HOME"
    rm -f /tmp/sdk.tar.xz
    "$HOME/zephyr-sdk-$SDK_VER/setup.sh" -t all -c >/dev/null
  fi
  echo "export ZEPHYR_TOOLCHAIN_VARIANT=zephyr" >> "$ENVFILE"
  echo "export ZEPHYR_SDK_INSTALL_DIR=\"$HOME/zephyr-sdk-$SDK_VER\"" >> "$ENVFILE"
else
  # native_sim compila com o gcc do host; dispensa o SDK
  echo "export ZEPHYR_TOOLCHAIN_VARIANT=host" >> "$ENVFILE"
fi

log "pronto."
cat <<EOF

Carregue o ambiente em cada shell nova:

    source "$ENVFILE"

Depois, na raiz da aplicacao ($APP_DIR):

    west twister -T tests -p native_sim
    .flow/bin/flow matrix

EOF
