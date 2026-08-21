#!/usr/bin/env python3
"""gh_bootstrap — leva o backlog do ReflowOven para issues do GitHub.

O backlog nasceu como dois CSVs de export do Jira (33 defeitos, 46 casos de
teste) mais decisoes de triagem que viviam num documento. Este script cria as
labels e importa tudo como issues, uma vez, de forma idempotente.

    python .flow/bin/gh_bootstrap.py labels            # so mostra
    python .flow/bin/gh_bootstrap.py labels --apply
    python .flow/bin/gh_bootstrap.py issues            # so mostra
    python .flow/bin/gh_bootstrap.py issues --apply

Sem `--apply` nada e escrito: imprime o que faria. Idempotente — uma issue cujo
titulo ja comeca com o mesmo `[RFO-XXX]` e pulada, entao rodar de novo depois de
uma interrupcao retoma de onde parou.

Precisa de `gh auth login` feito antes.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = "GustavoCCosta/ReflowOven"


def achar_seed() -> Path:
    """Onde estao os CSVs do export do Jira.

    Eles vivem em `<workspace>/flow/seed/`, um nivel ACIMA do repositorio —
    a pasta `flow/` e registro historico e nao e versionada aqui. Subir a
    arvore evita chutar quantos `parents[]` sao, que e o tipo de constante que
    quebra silenciosamente quando alguem move o script.
    """
    if os.environ.get("REFLOW_SEED"):
        return Path(os.environ["REFLOW_SEED"])
    for base in Path(__file__).resolve().parents:
        cand = base / "flow" / "seed"
        if (cand / "jira_bugs.csv").exists():
            return cand
    return Path("flow/seed")          # deixa o erro de "nao encontrado" falar


SEED = achar_seed()

# --------------------------------------------------------------------- labels

LABELS = [
    # estado — exatamente uma por issue; nenhuma significa `triagem`
    ("estado:pronto",      "0e8a16", "Pronto para o Dev puxar"),
    ("estado:fazendo",     "1d76db", "Dev trabalhando nisto"),
    ("estado:revisao",     "fbca04", "PR aberto, aguardando Q.A."),
    ("estado:ajustes",     "d93f0b", "Q.A. reprovou; retrabalho tem prioridade"),
    # prioridade
    ("prio:critico",       "b60205", "Critico"),
    ("prio:alto",          "d93f0b", "Alto"),
    ("prio:medio",         "fbca04", "Medio"),
    ("prio:baixo",         "c2e0c6", "Baixo"),
    # tipo
    ("tipo:defeito",       "ee0701", "Defeito do relatorio de Q.A."),
    ("tipo:teste",         "5319e7", "Cobertura de teste"),
    ("tipo:processo",      "006b75", "Ferramental e processo"),
    ("tipo:feature",       "0052cc", "Escopo novo pedido pelo dono"),
    # bloqueio e gate
    ("gate",               "000000", "Gate de seguranca: barra teste com rede eletrica"),
    ("precisa-hardware",   "5319e7", "Nenhum agente pode concluir: exige bancada"),
    ("precisa-humano",     "5319e7", "Exige decisao do Gustavo"),
    # area
    ("area:core",          "c5def5", "Nucleo de controle"),
    ("area:net",           "c5def5", "Rede, HTTP, pagina"),
    ("area:ui",            "c5def5", "Painel e encoder"),
    ("area:hw",            "c5def5", "Devicetree, pinctrl, hardware"),
    ("area:build",         "c5def5", "Build e geracao"),
    ("area:test",          "c5def5", "Testes e ferramental de teste"),
    ("area:sys",           "c5def5", "Sistema, endurance"),
    # rotulos de risco herdados do relatorio de Q.A. — preservam rastreabilidade
    ("reflow-seguranca",   "b60205", "Rotulo do relatorio: seguranca"),
    ("reflow-robustez",    "e99695", "Rotulo do relatorio: robustez"),
    ("reflow-funcional",   "bfd4f2", "Rotulo do relatorio: funcional"),
    ("reflow-desempenho",  "bfd4f2", "Rotulo do relatorio: desempenho"),
    ("reflow-configuracao","bfd4f2", "Rotulo do relatorio: configuracao"),
    ("reflow-build",       "bfd4f2", "Rotulo do relatorio: build"),
    ("reflow-teste",       "bfd4f2", "Rotulo do relatorio: teste"),
    ("reflow-unit",        "bfd4f2", "Rotulo do relatorio: unitario"),
    ("reflow-integracao",  "bfd4f2", "Rotulo do relatorio: integracao"),
    ("reflow-modularidade","bfd4f2", "Rotulo do relatorio: modularidade"),
    ("reflow-endurance",   "bfd4f2", "Rotulo do relatorio: endurance"),
]

PRIO = {"Highest": "prio:critico", "High": "prio:alto",
        "Medium": "prio:medio", "Low": "prio:baixo"}

# ------------------------------------------------------ decisoes de triagem
#
# Ja na `main` — nao vira issue. O commit e o registro; abrir issue fechada so
# para arquivar historico polui a lista sem destravar trabalho nenhum.
JA_NA_MAIN = {"RFO-B01", "RFO-B02", "RFO-B03",
              "RFO-G01", "RFO-G02", "RFO-G03",
              "RFO-T31", "RFO-T32", "RFO-T44"}
# Cobertura que ja existe na suite hoje.
JA_COBERTO = {"RFO-T01", "RFO-T02"}
# Retirado: a premissa que o originou era falsa (ver historico do backlog).
RETIRADO = {"RFO-G04"}

# Gate de seguranca. B01/B02/B03 fechados; sobram estes dois.
GATE = {"RFO-B05", "RFO-B06"}
# O que sai de triagem agora. O resto entra sem `estado:*` e eu promovo depois,
# mantendo 2 a 4 itens em `pronto` para o Dev nao ficar ocioso nem eu priorizar
# cedo demais.
PRONTO = {"RFO-B05", "RFO-B06", "RFO-G07"}

BLOQUEIO = {
    "RFO-B18": "precisa-hardware",   # cs-gpios do MAX6675 vs pinctrl do SPI2
    "RFO-B31": "precisa-humano",     # decisao de produto sobre transporte da pagina
    "RFO-T03": "precisa-hardware",   # HIL, 30 min
    "RFO-T06": "precisa-hardware",   # puxar o termopar fisicamente
    "RFO-T13": "precisa-hardware",   # osciloscopio no gate do SSR desde o reset
    "RFO-T16": "precisa-hardware",   # osciloscopio na janela de PWM
    "RFO-T45": "precisa-hardware",   # endurance de 12 h
    "RFO-T46": "precisa-hardware",   # checklist de bancada do README
}

# Casos de teste que viram issue propria.
#
# Decisao de Gerente: caso que existe SO para validar um defeito NAO vira issue
# separada — o teste faz parte do PR que corrige o defeito, e duas issues para
# uma unidade de trabalho briga com "um PR fecha uma issue" (PROCESSO secao 5).
# Viram issue apenas os casos de cobertura INDEPENDENTE (coluna de defeito vazia
# no catalogo) e os que exigem bancada, que sao trabalho do Gustavo, nao do Dev.
TESTES_PROPRIOS = {
    # cobertura independente, acionavel em software
    "RFO-T09", "RFO-T10", "RFO-T11", "RFO-T12",
    "RFO-T17", "RFO-T18", "RFO-T19",
    # exigem bancada
    "RFO-T03", "RFO-T06", "RFO-T13", "RFO-T16", "RFO-T45", "RFO-T46",
}

# ------------------------------------------- itens que nunca estiveram nos CSVs
#
# RFO-G* de processo e RFO-F* de escopo novo nasceram durante o trabalho, fora do
# export do Jira. Ficam aqui para o import ser a fonte unica.
EXTRAS = [
    dict(id="RFO-G05", prio="prio:baixo", area="area:test", tipo="tipo:processo",
         titulo="Comentario de boards/native_sim.overlay descreve o mecanismo errado de falha",
         corpo="""**Arquivo:** `boards/native_sim.overlay`

## O defeito

Duas coisas, ambas de diagnostico enganoso:

1. O comentario do overlay descreve um mecanismo de falha que nao e o que
   realmente acontece. Quem lê para entender por que a build quebrava aprende a
   coisa errada.
2. Token com mais de 128 caracteres torna o endpoint inalcancavel **sem dizer
   por que** — falha silenciosa, que e a pior forma de falhar em algo que
   controla um forno.

## Critério de aceite

- [ ] O comentario descreve o mecanismo real de falha
- [ ] Token acima do limite produz erro explicito, nao silencio
"""),
    dict(id="RFO-G06", prio="prio:alto", area="area:test", tipo="tipo:processo",
         titulo="Termopar com valor injetavel no native_sim",
         corpo="""## O que falta

Nao existe forma de injetar temperatura na suite. Por isso `temp.c`, `heater.c` e
`controller.c` tem **cobertura zero** — e e onde moram dois dos itens de gate que
ainda estao abertos (B05, B06).

## Por que isto e alavanca, nao conforto

Destrava de uma vez a Etapa 4 do roteiro: T09-T12 e T17-T19 (`controller.c`),
T04-T06 (`temp.c`), T15-T16 (`heater.c`). Enquanto nao existir, todo PR nessas
tres areas prova menos do que deveria.

## Critério de aceite

- [ ] A suite consegue fixar a leitura do termopar num valor arbitrario
- [ ] Consegue simular termopar aberto e erro de SPI
- [ ] Consegue simular leitura travada (o cenario do RFO-B05)
- [ ] Pelo menos um teste novo de `controller.c` usando o mecanismo
"""),
    dict(id="RFO-G07", prio="prio:critico", area="area:test", tipo="tipo:processo",
         titulo="extrai_diff corrompe o patch na ultima hunk; flow apply e flow base culpam a pessoa errada",
         corpo="""**Arquivo:** `.flow/bin/flow` (`extrai_diff`)

## O defeito

`extrai_diff` corrompe a ultima hunk do patch. O efeito colateral e pior que o
defeito: as mensagens de erro **mentem sobre de quem e a culpa**.

- `flow apply` diz *"devolva ao Dev pedindo regeracao"* quando o problema e a
  propria ferramenta.
- `flow base --mainpatch` diz *"provavelmente o Gustavo empurrou algo novo"*
  quando o Gustavo nao fez nada.

Foi exatamente isso que quase reprovou um PR correto: o Q.A. viu a mensagem, viu
que a instrucao do papel dele era reprovar, e so escapou porque teve o cuidado de
medir antes de obedecer.

## Nota sobre a v2 do processo

Com o fluxo em PR de verdade, `flow apply` e `flow base` sairam do caminho
critico — o codigo circula como commit, nao como diff colado. **Mas
`flow mutate` continua sendo o item 4 da Definition of Done**, e ele vive no
mesmo arquivo. Conserte o `extrai_diff` e, no mesmo PR, adapte o `mutate` para
usar `origin/main` (ou o merge-base do PR) como BASE, em vez da branch
`flow-base` que o fluxo antigo criava.

## Contornos, enquanto estiver aberto

- aplicar: `git apply --3way --whitespace=nowarn`
- gerar: `git diff --binary -U6`

Nenhum dos dois e desculpa para deixar de conferir.

## Critério de aceite

- [ ] Teste que falha com o `extrai_diff` atual: patch cuja ultima hunk sobrevive intacta
- [ ] Mensagem de erro de `flow apply` nao atribui culpa que nao pode verificar
- [ ] `flow mutate` funciona com `origin/main` como BASE, sem depender da branch `flow-base`
- [ ] `flow mutate` verificado mordendo: a suite fica vermelha com o codigo revertido
"""),
    dict(id="RFO-G08", prio="prio:baixo", area="area:test", tipo="tipo:processo",
         titulo="Assercao decorativa em test_single_byte_corruption_is_survivable",
         corpo="""**Arquivo:** `tests/` — `test_single_byte_corruption_is_survivable`

## O defeito

A assercao e decorativa: o teste **nao pode falhar**. Verde falso, mesma familia
do RFO-B23.

Um teste que nao pode falhar e pior que teste ausente, porque compra confianca
sem entregar nada — e aqui ele cobre corrupcao de byte, que e caminho de
seguranca.

## Critério de aceite

- [ ] O teste falha quando a corrupcao que ele afirma sobreviver e introduzida
- [ ] Verificado com `flow mutate`, nao presumido
"""),
    dict(id="RFO-G09", prio="prio:baixo", area="area:build", tipo="tipo:processo",
         titulo="Autoverificacao do setup_zephyr.sh: falha nao aponta o stamp; ZEPHYR_WS relativo nao e normalizado",
         corpo="""**Arquivo:** `.flow/bin/setup_zephyr.sh`

## O defeito

1. A mensagem de falha da autoverificacao nao aponta **qual** stamp falhou, o que
   obriga quem monta o ambiente a ler o script para descobrir.
2. `ZEPHYR_WS` relativo nao e normalizado, entao o mesmo workspace referenciado
   de dois diretorios diferentes vira dois caminhos e a autoverificacao decide
   errado.

## Critério de aceite

- [ ] Falha de autoverificacao nomeia o stamp que falhou
- [ ] `ZEPHYR_WS=./algo` e `ZEPHYR_WS=/abs/algo` produzem o mesmo resultado
"""),
    dict(id="RFO-F01", prio="prio:medio", area="area:core", tipo="tipo:feature",
         titulo="Perfis: layout serializavel (struct sem ponteiro, nome em buffer fixo)",
         corpo="""Pedido pelo Gustavo em 19/08/2026. **Nao e defeito** — e a primeira das tres
fatias de perfis editaveis com persistencia. Nao entra antes do gate fechar.

## O que muda

`struct reflow_profile` guarda `const char *` para o nome. **Refactor puro:**
nome vira buffer de tamanho fixo, struct fica sem ponteiro, e portanto
serializavel.

## Por que isto vem primeiro

Persistir a struct como ela esta hoje gravaria um **endereco** na flash. O pior
caso nao e perder o nome: e **servir nome de perfil lido de memoria arbitraria**
na pagina web, depois de um reboot ou de uma atualizacao de firmware.

## Depende de

- gate de seguranca fechado (B05, B06)
- RFO-B04 — `grace_ms` editavel e o caminho natural para calar o alarme de
  timeout em vez de consertar a causa

## Invariante das tres fatias

`CONFIG_REFLOW_ABS_MAX_TEMP_C` e de compilacao, continua acima de qualquer perfil
e **nao** vira editavel por API.

## Critério de aceite

- [ ] `struct reflow_profile` sem nenhum ponteiro
- [ ] Nome em buffer de tamanho fixo, com o limite declarado em Kconfig ou header
- [ ] Nome no limite exato do buffer continua terminado em NUL
- [ ] Os perfis embutidos continuam identicos em comportamento (teste de regressao)
- [ ] `reflow_profile_validate()` existe e rejeita perfil incoerente
"""),
    dict(id="RFO-F02", prio="prio:medio", area="area:core", tipo="tipo:feature",
         titulo="Perfis: persistencia em settings/NVS com registro versionado",
         corpo="""Segunda fatia de perfis editaveis. **Depende do RFO-F01.**

## O que muda

Perfil passa a ser gravado com `settings` sobre NVS, com registro **versionado**,
e carregado no boot com fallback para os embutidos.

## Os tres riscos que ditam o desenho

1. **O RP2350 executa por XIP da mesma flash que estaria escrevendo.** Isto nao e
   detalhe de implementacao, e a restricao central.
2. Um blob armazenado precisa de **magic, versao e CRC** — senao uma atualizacao
   de firmware le bytes antigos como perfil valido.
3. `reflow profile reset` de volta aos embutidos **nao e opcional**: sem ele um
   perfil armazenado ruim inutiliza o forno ate reflash.

## Critério de aceite

- [ ] Registro tem magic, versao e CRC, conferidos na carga
- [ ] Registro de versao anterior e recusado com fallback para os embutidos
- [ ] Registro corrompido em um byte e recusado com fallback
- [ ] Flash cheia nao deixa o forno sem perfil valido
- [ ] `reflow profile reset` volta aos embutidos
- [ ] Perfil carregado passa por `reflow_profile_validate()` antes de ser usado
"""),
    dict(id="RFO-F03", prio="prio:medio", area="area:net", tipo="tipo:feature",
         titulo="Perfis: CRUD no shell, endpoint HTTP autenticado e UI",
         corpo="""Terceira fatia de perfis editaveis. **Depende do RFO-F02.**

## O que muda

Editar perfil pelo shell, por endpoint HTTP **autenticado**, e pela pagina.

## Pre-requisitos que nao sao negociaveis

Esta fatia abre a superficie que estes defeitos tornam explorável. Todos tem que
estar fechados antes:

- **RFO-B09** — `send_profiles()` estoura o buffer da pilha quando os nomes
  crescem (subfluxo de `size_t`). Hoje nao e acionavel porque a tabela e fixa e
  curta; com nomes editaveis, passa a ser.
- **RFO-B30** — retorno de `snprintk` usado como `body_len` sem clamp.
- **RFO-B10** — indice de perfil nao validado na API.
- RFO-B02 (autenticacao do endpoint) — ✅ ja na `main`.

## Relogio andando

O commit `a089dad` (quarto perfil, *Bake / Dry 200C*) levou o JSON de
`/api/profiles` a **87 de 256 bytes**. Folga de 169 — cabem ~9 perfis a mais. A
partir de 6 ou 7 perfis acrescentados a mao, o B09 sobe de prioridade sem esperar
esta fatia.

## Critério de aceite

- [ ] Criar, editar e apagar perfil pelo shell
- [ ] Endpoint HTTP exige autenticacao (o mesmo gate do B02)
- [ ] Nome no limite do buffer nao estoura `send_profiles()`
- [ ] Indice fora de faixa e recusado com erro, nao silenciosamente tratado como 0
- [ ] `abort_mc` acima do corte absoluto de compilacao e recusado
- [ ] Perfil invalido nunca chega a ser executavel
"""),
]

# ------------------------------------------------------------------ utilidades


def gh(*args, entrada=None, check=True):
    p = subprocess.run(["gh", *map(str, args)], capture_output=True, text=True,
                       input=entrada, encoding="utf-8", errors="replace")
    if check and p.returncode != 0:
        print(f"erro: gh {' '.join(map(str, args))}\n{(p.stderr or p.stdout).strip()}",
              file=sys.stderr)
        sys.exit(1)
    return p


def exige_auth():
    if subprocess.run(["gh", "auth", "status"], capture_output=True).returncode != 0:
        print("erro: `gh` nao esta autenticado. rode `gh auth login` primeiro.",
              file=sys.stderr)
        sys.exit(1)


def jira_para_markdown(texto: str) -> str:
    """Converte a marcacao do export do Jira para Markdown.

    O export usa `*Titulo*` como cabecalho de bloco, linhas com um unico espaco
    como separador de paragrafo, e indentacao para sub-itens. Sem conversao, o
    Markdown junta tudo num paragrafo so e a issue fica ilegivel.
    """
    saida = []
    for linha in texto.replace("\r\n", "\n").split("\n"):
        if not linha.strip():
            saida.append("")
            continue
        # `*Criterio de aceite*` vira secao de verdade: o PROCESSO exige que ela
        # exista e seja verificavel, e o Dev precisa achá-la sem procurar.
        if re.fullmatch(r"\*Crit[eé]rio de aceite\*", linha.strip()):
            saida += ["", "## Critério de aceite", ""]
            continue
        # `*Titulo*` sozinho na linha -> negrito em paragrafo proprio
        m = re.fullmatch(r"\*([^*]+)\*", linha.strip())
        if m:
            saida += ["", f"**{m.group(1)}**", ""]
            continue
        # `*Arquivo:* caminho` e afins -> negrito inline
        linha = re.sub(r"\*([^*\n]+)\*", r"**\1**", linha)
        # linha indentada: preserva a quebra, senao o Markdown colapsa
        if linha.startswith("  "):
            saida.append(linha.rstrip() + "  ")
        else:
            saida.append(linha.rstrip())
    # colapsa linhas vazias repetidas
    limpo = []
    for ln in saida:
        if ln == "" and limpo and limpo[-1] == "":
            continue
        limpo.append(ln)
    return "\n".join(limpo).strip() + "\n"


def ler_csv(nome: str) -> list[dict]:
    caminho = SEED / nome
    if not caminho.exists():
        print(f"erro: {caminho} nao encontrado", file=sys.stderr)
        sys.exit(1)
    with open(caminho, encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def rfo_id(summary: str) -> str | None:
    m = re.match(r"\[(RFO-[A-Z]\d+)\]", summary)
    return m.group(1) if m else None


# --------------------------------------------------------------------- comandos


def cmd_labels(a):
    exige_auth()
    existentes = {l["name"] for l in json.loads(
        gh("label", "list", "-R", REPO, "--limit", "200",
           "--json", "name").stdout or "[]")}
    for nome, cor, desc in LABELS:
        verbo = "atualizar" if nome in existentes else "criar"
        if not a.apply:
            print(f"  [{verbo}] {nome}")
            continue
        gh("label", "create", nome, "-R", REPO, "--color", cor,
           "--description", desc, "--force")
        print(f"  {verbo}: {nome}")
    print(f"\n{len(LABELS)} labels" + ("" if a.apply else "  (nada escrito — use --apply)"))


def montar_issues() -> list[dict]:
    """A lista final de issues a criar, com as labels ja resolvidas."""
    itens = []

    for nome, tipo in (("jira_bugs.csv", "tipo:defeito"),
                       ("jira_testcases.csv", "tipo:teste")):
        for r in ler_csv(nome):
            ident = rfo_id(r["Summary"])
            if not ident or ident in JA_NA_MAIN or ident in JA_COBERTO \
                    or ident in RETIRADO:
                continue
            if tipo == "tipo:teste" and ident not in TESTES_PROPRIOS:
                continue
            labels = [tipo, PRIO[r["Priority"]], f"area:{r['Component']}"]
            if r["Labels"]:
                labels.append(r["Labels"])
            if ident in GATE:
                labels.append("gate")
            if ident in PRONTO:
                labels.append("estado:pronto")
            if ident in BLOQUEIO:
                labels.append(BLOQUEIO[ident])
            corpo = jira_para_markdown(r["Description"])
            if r.get("Environment"):
                corpo += f"\n---\n\n**Ambiente:** {r['Environment'].strip()}\n"
            itens.append(dict(id=ident, titulo=r["Summary"],
                              labels=labels, corpo=corpo))

    for e in EXTRAS:
        if e["id"] in JA_NA_MAIN or e["id"] in RETIRADO:
            continue
        labels = [e["tipo"], e["prio"], e["area"]]
        if e["id"] in GATE:
            labels.append("gate")
        if e["id"] in PRONTO:
            labels.append("estado:pronto")
        if e["id"] in BLOQUEIO:
            labels.append(BLOQUEIO[e["id"]])
        itens.append(dict(id=e["id"], titulo=f"[{e['id']}] {e['titulo']}",
                          labels=labels, corpo=e["corpo"]))

    itens.sort(key=lambda i: (i["id"][4], int(i["id"][5:])))
    return itens


def cmd_issues(a):
    exige_auth()
    itens = montar_issues()
    if a.only:
        alvo = {s.strip().upper() for s in a.only.split(",")}
        itens = [i for i in itens if i["id"] in alvo]

    existentes = json.loads(gh("issue", "list", "-R", REPO, "--state", "all",
                               "--limit", "500", "--json", "title").stdout or "[]")
    ja = {rfo_id(i["title"]) for i in existentes}
    ja.discard(None)

    criadas = puladas = 0
    for it in itens:
        if it["id"] in ja:
            puladas += 1
            continue
        if not a.apply:
            print(f"  [criar] {it['id']:9s} {' '.join(sorted(it['labels']))}")
            criadas += 1
            continue
        # --body-file evita o limite de tamanho de linha de comando do Windows e
        # nao mexe em acento, aspas nem quebra de linha do corpo.
        with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False,
                                         encoding="utf-8") as tmp:
            tmp.write(it["corpo"])
            caminho = tmp.name
        try:
            p = gh("issue", "create", "-R", REPO, "--title", it["titulo"],
                   "--body-file", caminho,
                   *sum((["--label", l] for l in it["labels"]), []))
            print(f"  criada: {it['id']:9s} {p.stdout.strip()}")
            criadas += 1
        finally:
            Path(caminho).unlink(missing_ok=True)

    print(f"\n{criadas} a criar, {puladas} ja existem"
          + ("" if a.apply else "  (nada escrito — use --apply)"))


def main():
    ap = argparse.ArgumentParser(
        prog="gh_bootstrap", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("labels", help="cria/atualiza as labels")
    s.add_argument("--apply", action="store_true")
    s.set_defaults(f=cmd_labels)

    s = sub.add_parser("issues", help="importa o backlog como issues")
    s.add_argument("--apply", action="store_true")
    s.add_argument("--only", default=None, help="lista de IDs, separados por virgula")
    s.set_defaults(f=cmd_issues)

    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
