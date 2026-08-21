#!/usr/bin/env bash
# Prepara o ambiente Zephyr da sessao (Dev ou Q.A.).
#
# Por padrao instala o suficiente para `west twister -p native_sim` e para
# `.flow/bin/flow matrix`, que usam o gcc do host — sem baixar o Zephyr SDK
# (~1,5 GB). Isso valida toda a logica, os Kconfig e a matriz de modularidade.
#
#   --sdk   baixa tambem o Zephyr SDK (necessario so para compilar para
#           rpi_pico / esp32 de verdade). Em geral nao precisa: as builds de
#           board real acontecem no GitHub Actions, que ja tem o SDK na imagem.
#
# Idempotente: rodar de novo nao refaz o que ja esta pronto.
#
# ---------------------------------------------------------------------------
# RFO-G02. Quatro defeitos consertados aqui, e todos com a mesma assinatura: uma
# guarda que testava a coisa errada, ou uma dependencia que ninguem checava.
#
#   1. `west update` nunca rodava, porque a guarda testava `$WS/zephyr` — que o
#      `west init` acabou de criar. Agora a guarda e um stamp escrito DEPOIS de
#      um update bem sucedido: ela testa o resultado, nao o passo anterior.
#   2. O `apt-get install` inteiro estava atras de `command -v cmake || ninja`.
#      No container das sessoes os dois ja existem, entao NADA era instalado —
#      inclusive `gcc-multilib`, sem o qual `native_sim/native` (que e -m32) nao
#      compila. A guarda passou a ser por pacote.
#   3. `nrf_hw_models` vem no grupo default do manifesto e depende de
#      `hal_nordic`, que o filtro de grupos remove. Toda build abortava com
#      "Unmet or cyclic dependencies in modules". Resolvido com
#      `manifest.project-filter`, que exclui o projeto sem mexer no filtro de
#      grupos (que existe por um motivo legitimo: sem ele o update passa de 6 GB).
#   4. SDL2 de 32 bits. Sem ele quatro das oito combinacoes da matriz falham em
#      `ld: cannot find -lSDL2`, por AMBIENTE e nao pelo firmware — e 6/8 se le
#      como regressao do RFO-G01. O pacote `libsdl2-dev:i386` nao instala neste
#      Ubuntu, entao os cabecalhos saem do `.deb` extraido a mao.
#
# A ultima secao e uma autoverificacao: mede cada um dos quatro e recusa terminar
# em silencio se algum falhou. Um setup que imprime "pronto." sem estar pronto foi
# o defeito original.
# ---------------------------------------------------------------------------
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WS="${ZEPHYR_WS:-$HOME/zephyrproject}"
COM_SDK=0
[[ "${1:-}" == "--sdk" ]] && COM_SDK=1

log()   { printf '\033[1m[setup]\033[0m %s\n' "$*"; }
aviso() { printf '\033[1;33m[setup]\033[0m %s\n' "$*" >&2; }
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo

# Stamp do `west update`. Fica no workspace, some com ele, e e a unica coisa que
# autoriza pular o update. Apague para forcar um update novo.
STAMP_UPDATE="$WS/.flow-west-updated"

# ---------------------------------------------------------------- pacotes
# Guarda por PACOTE, nao por `cmake`/`ninja`: era isso que fazia o bloco inteiro
# ser pulado num container que ja tinha os dois. `apt-get install` e idempotente,
# entao o pior caso de listar demais e alguns segundos.
PACOTES=(
  git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget
  python3-dev python3-venv python3-pip xz-utils file make
  gcc g++ gcc-multilib g++-multilib libc6-dev-i386
  libsdl2-dev libmagic1
)

APT_ATUALIZADO=0
apt_update_uma_vez() {
  [[ $APT_ATUALIZADO -eq 1 ]] && return 0
  $SUDO apt-get update -qq
  APT_ATUALIZADO=1
}

# Um pacote conta como presente se o dpkg o conhece pelo nome, OU pelo nome com
# sufixo `t64`. Isso nao e capricho: na transicao de time_t do Ubuntu 24.04 vários
# pacotes foram renomeados (libmagic1 -> libmagic1t64) e o nome antigo virou
# virtual, que `dpkg -s` nunca acha. Sem esta segunda tentativa o script
# reinstalava o mesmo pacote e batia na rede em TODA execucao — quebrando a
# idempotencia que ele promete no cabecalho.
pkg_presente() {
  dpkg -s "$1" >/dev/null 2>&1 || dpkg -s "${1}t64" >/dev/null 2>&1
}

FALTANDO=()
for p in "${PACOTES[@]}"; do
  pkg_presente "$p" || FALTANDO+=("$p")
done
if ((${#FALTANDO[@]})); then
  log "instalando dependencias de sistema: ${FALTANDO[*]}"
  apt_update_uma_vez
  $SUDO apt-get install -y --no-install-recommends "${FALTANDO[@]}" >/dev/null
else
  log "dependencias de sistema ja presentes"
fi

# ------------------------------------------------------------ SDL2 32 bits
# `native_sim/native` compila -m32 e o painel SDL do board exige
# /usr/include/i386-linux-gnu/SDL2/_real_SDL_config.h, que so vem no
# libsdl2-dev:i386. Esse pacote nao resolve dependencias neste Ubuntu (o i386 tem
# um subconjunto do arquivo), mas a BIBLIOTECA de runtime resolve e traz a cadeia
# inteira; os cabecalhos a gente extrai do .deb sem instalar.
sdl32_ok() {
  local t rc=0
  t="$(mktemp -d)"
  printf '#include <SDL2/SDL.h>\nint main(void){return SDL_Init(0);}\n' >"$t/t.c"
  gcc -m32 "$t/t.c" -o "$t/t" -lSDL2 >/dev/null 2>&1 || rc=1
  rm -rf "$t"
  return $rc
}

instalar_sdl32() {
  local deb tmp
  log "SDL2 de 32 bits (necessario para 4 das 8 combinacoes da matriz)"

  if ! dpkg --print-foreign-architectures | grep -qx i386; then
    $SUDO dpkg --add-architecture i386
    APT_ATUALIZADO=0
  fi
  apt_update_uma_vez
  # A lib de runtime instala normalmente e arrasta a cadeia i386 (X11, wayland,
  # drm, pulse, mesa). E o `-dev` que nao instala.
  $SUDO apt-get install -y --no-install-recommends libsdl2-2.0-0:i386 >/dev/null

  tmp="$(mktemp -d)"
  ( cd "$tmp" && apt-get download libsdl2-dev:i386 >/dev/null 2>&1 ) || true
  deb="$(find "$tmp" -name 'libsdl2-dev_*_i386.deb' -print -quit)"
  if [[ -z "$deb" ]]; then
    rm -rf "$tmp"
    aviso "nao consegui baixar libsdl2-dev:i386"
    return 1
  fi
  dpkg-deb -x "$deb" "$tmp/root"
  # O deb poe a config especifica de arquitetura em
  # usr/include/i386-linux-gnu/SDL2/, que e onde o gcc -m32 procura
  # <SDL2/_real_SDL_config.h>. Copiar o CONTEUDO (o `/.` no fim) e nao o
  # diretorio: `cp -a dir /usr/include/` com o destino ja existente aninharia
  # um i386-linux-gnu dentro do outro e o cabecalho continuaria sem ser achado.
  $SUDO mkdir -p /usr/include/i386-linux-gnu/SDL2
  $SUDO cp -a "$tmp/root/usr/include/i386-linux-gnu/SDL2/." \
              /usr/include/i386-linux-gnu/SDL2/
  # Os .a vem do -dev; o .so de runtime veio do pacote instalado acima, e o
  # link sem versao (que e o que `-lSDL2` procura) nao vem em nenhum dos dois.
  $SUDO cp -a "$tmp/root/usr/lib/i386-linux-gnu/"libSDL2*.a /usr/lib/i386-linux-gnu/ 2>/dev/null || true
  [[ -e /usr/lib/i386-linux-gnu/libSDL2.so ]] || \
    $SUDO ln -sf libSDL2-2.0.so.0 /usr/lib/i386-linux-gnu/libSDL2.so
  $SUDO ldconfig
  rm -rf "$tmp"
}

if sdl32_ok; then
  log "SDL2 de 32 bits ja utilizavel"
else
  instalar_sdl32 || true
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
    apt_update_uma_vez
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
  # O `west init` roda num subshell em `/` de proposito. Ele procura um workspace
  # subindo a partir do CWD, e se o script for chamado de dentro de outro
  # workspace (o caso normal de quem ja tem uma sessao montada e esta conferindo
  # o script) ele aborta com "already initialized in <outro dir>" mesmo com o
  # caminho de destino explicito. Sem isto o script nao e verificavel de dentro
  # de uma sessao — que e exatamente onde o Q.A. precisa verifica-lo.
  if [[ -f "$APP_DIR/west.yml" ]]; then
    log "workspace T2 a partir do west.yml da aplicacao"
    ( cd / && { west init -l "$APP_DIR" 2>/dev/null || west init -l --mf west.yml "$APP_DIR"; } )
  else
    log "aplicacao sem west.yml — criando workspace padrao do Zephyr"
    # --narrow e opcao de `west update`, nao de `west init`
    ( cd / && west init "$WS" )
  fi
fi

cd "$WS"
if [[ $COM_SDK -eq 0 ]]; then
  # native_sim nao usa HAL de vendor. Sem este filtro o `west update` baixa
  # ~30 modulos e passa de 6 GB; com ele fica na casa de 1 GB.
  #
  # ATENCAO ao mexer: LVGL fica no grupo DEFAULT do manifesto, nao em `optional`
  # (conferido em zephyr/west.yml — a entrada nao declara `groups:`). Ou seja,
  # este filtro NAO ameaca o LVGL, e os `configdefault LV_*` do Kconfig continuam
  # tendo tipo. O que faltava LVGL era o `west update` nao rodar.
  log "filtrando grupos do manifesto (hal/tools/debug/optional/bsim)"
  west config manifest.group-filter -- -hal,-tools,-debug,-optional,-bsim || true

  # `nrf_hw_models` esta no grupo default (nao declara `groups:`), entao o filtro
  # acima nao o alcanca — mas ele depende de `hal_nordic`, que o filtro remove.
  # Resultado: TODA build aborta em
  #   "Unmet or cyclic dependencies in modules: nrf_hw_models depends on
  #    ['hal_nordic']"
  # `manifest.project-filter` desativa o projeto sem tocar no filtro de grupos e
  # sem baixar o HAL da Nordic so para satisfazer uma dependencia que este projeto
  # nao usa. Requer west >= 1.2.
  log "desativando nrf_hw_models (depende de hal_nordic, que o filtro remove)"
  west config manifest.project-filter -- -nrf_hw_models || true
fi

# A guarda do update testa o RESULTADO do update, nao o do init. `west init` cria
# `$WS/zephyr`, entao a guarda antiga (`[[ ! -d "$WS/zephyr" ]]`) nascia falsa e o
# update era sempre pulado: `modules/` nunca existia e toda build morria em
# `LV_FONT_MONTSERRAT_28 defined without a type`. Este stamp so e escrito depois
# de um update que terminou bem.
if [[ ! -f "$STAMP_UPDATE" ]]; then
  log "west update (so isto demora alguns minutos)"
  west update --narrow -o=--depth=1
  date -u +'%Y-%m-%dT%H:%M:%SZ' >"$STAMP_UPDATE"
else
  log "west update ja feito ($(cat "$STAMP_UPDATE")); apague $STAMP_UPDATE para refazer"
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

# ------------------------------------------------------------ autoverificacao
# O defeito original nao foi so o `west update` pulado: foi o script imprimir
# "pronto." em cima de um ambiente que nao compilava. Cada item abaixo e uma das
# causas do RFO-G02, medida, e um FALHA aqui derruba o script.
falhas=0
checa() {  # checa "<descricao>" <comando...>
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then
    printf '  \033[32mok   \033[0m %s\n' "$desc"
  else
    printf '  \033[31mFALHA\033[0m %s\n' "$desc"
    falhas=$((falhas + 1))
  fi
}

multilib_ok() {
  local t rc=0
  t="$(mktemp -d)"
  printf 'int main(void){return 0;}\n' >"$t/t.c"
  gcc -m32 "$t/t.c" -o "$t/t" >/dev/null 2>&1 || rc=1
  rm -rf "$t"
  return $rc
}
modulos_ok()   { [[ -n "$(ls -A "$WS/modules" 2>/dev/null)" ]]; }
lvgl_ok()      { [[ -d "$WS/modules/lib/gui/lvgl" ]]; }
sem_ciclo_ok() { [[ ! -d "$WS/modules/bsim_hw_models/nrf_hw_models" ]]; }

echo
log "autoverificacao:"
checa "west list responde"                            west list
checa "modules/ populado (west update rodou)"         modulos_ok
checa "LVGL presente"                                 lvgl_ok
checa "nrf_hw_models ausente (sem ciclo de modulos)"  sem_ciclo_ok
checa "gcc -m32 compila (multilib)"                   multilib_ok
checa "gcc -m32 acha SDL2 (matriz 8/8)"               sdl32_ok

if ((falhas)); then
  echo
  aviso "$falhas verificacao(oes) falharam — o ambiente NAO esta pronto."
  aviso "nao rode 'flow matrix' e conclua que o firmware regrediu: conserte isto"
  aviso "primeiro, ou relate no doc do ticket RFO-G02."
  exit 1
fi

log "pronto."
cat <<EOF

Carregue o ambiente em cada shell nova:

    source "$ENVFILE"

Depois, na raiz da aplicacao ($APP_DIR):

    west twister -T tests -p native_sim
    .flow/bin/flow matrix

EOF
