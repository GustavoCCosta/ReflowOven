#!/usr/bin/env python3
"""Importa jira_bugs.csv e jira_testcases.csv para issues do GitHub.

Uso:
    python3 .flow/seed/import_jira_csv.py jira_bugs.csv jira_testcases.csv
    python3 .flow/seed/import_jira_csv.py jira_bugs.csv --dry-run

Preserva a rastreabilidade bidirecional bug<->teste do relatorio inicial:
depois de criar tudo, reescreve as referencias RFO-Bxx / RFO-Txx para os
numeros reais das issues do GitHub.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from pathlib import Path

PRIO = {
    "highest": "prio:critico", "critical": "prio:critico", "critico": "prio:critico",
    "crítico": "prio:critico", "blocker": "prio:critico",
    "high": "prio:alto", "alto": "prio:alto", "major": "prio:alto",
    "medium": "prio:medio", "medio": "prio:medio", "médio": "prio:medio",
    "low": "prio:baixo", "baixo": "prio:baixo", "lowest": "prio:baixo", "minor": "prio:baixo",
}

AREA = [
    (r"segur|safety|fail-?safe|aquec|heater|ssr|overtemp|sobretemp", "area:seguranca"),
    (r"\brede\b|net|http|sse|socket|web|wifi|api", "area:rede"),
    (r"\bui\b|display|encoder|shell|interface|botao|botão", "area:ui"),
    (r"teste|test|ztest|cobertura|twister", "area:testes"),
    (r"build|cmake|kconfig|overlay|ferrament|tool", "area:build"),
    (r"\bhw\b|hardware|devicetree|pinctrl|gpio|spi|max6675|eletric|elétric", "area:hw"),
    (r"pid|perfil|profile|termopar|thermo|core|controller|estado", "area:core"),
]


def run(cmd, check=True):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if check and p.returncode != 0:
        print(f"erro: {' '.join(cmd)}\n{p.stderr}", file=sys.stderr)
        sys.exit(1)
    return p


def ler_csv(caminho: Path) -> list[dict]:
    # o exportador do Jira grava UTF-8 com BOM
    with open(caminho, newline="", encoding="utf-8-sig") as fh:
        return [{(k or "").strip(): (v or "") for k, v in row.items()}
                for row in csv.DictReader(fh)]


def labels_de(row: dict, tipo: str) -> list[str]:
    texto = " ".join([row.get("Summary", ""), row.get("Description", ""),
                      row.get("Component", ""), row.get("Labels", "")]).lower()
    out = {"status:triagem"}

    p = row.get("Priority", "").strip().lower()
    out.add(PRIO.get(p, "prio:medio"))

    comp = row.get("Component", "").strip().lower()
    achou = False
    for padrao, lab in AREA:
        if re.search(padrao, comp or texto):
            out.add(lab)
            achou = True
            break
    if not achou:
        out.add("area:core")

    jl = row.get("Labels", "").lower()
    if "seguranca" in jl or "segurança" in jl or "safety" in jl:
        out.add("gate-seguranca")
        out.add("area:seguranca")
    if re.search(r"\bhil\b|bancada|hardware|precisa-hw", jl) or "hw" in jl.split():
        out.add("precisa-hardware")

    if tipo == "teste":
        out.add("area:testes")
    return sorted(out)


def corpo_de(row: dict, tipo: str) -> str:
    partes = [row.get("Description", "").strip()]
    if row.get("Environment"):
        partes.append(f"\n---\n\n**Ambiente**\n\n{row['Environment'].strip()}")
    if "aceite" not in " ".join(partes).lower():
        partes.append(
            "\n---\n\n## Criterio de aceite\n\n"
            "<!-- OBRIGATORIO. O Gerente preenche antes de promover para status:pronto. -->\n"
            "- [ ] \n"
        )
    partes.append(
        "\n---\n"
        f"\n_Importado do relatorio inicial de Q.A. ({'defeito' if tipo == 'bug' else 'caso de teste'})._"
    )
    return "\n".join(partes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    mapa: dict[str, int] = {}   # RFO-B01 -> numero da issue
    criadas: list[tuple[str, int]] = []

    for caminho in a.csvs:
        if not caminho.exists():
            print(f"pulando (nao existe): {caminho}", file=sys.stderr)
            continue
        tipo = "teste" if "test" in caminho.name.lower() else "bug"
        linhas = ler_csv(caminho)
        print(f"\n{caminho.name}: {len(linhas)} linhas ({tipo})")
        for row in linhas:
            titulo = row.get("Summary", "").strip()
            if not titulo:
                continue
            labs = labels_de(row, tipo)
            corpo = corpo_de(row, tipo)
            ident = (re.search(r"RFO-[BT]\d+", titulo) or [None])
            ident = ident.group(0) if hasattr(ident, "group") else None

            if a.dry_run:
                print(f"  [dry] {titulo[:60]:<62} {' '.join(labs)}")
                continue

            cmd = ["gh", "issue", "create", "--title", titulo, "--body", corpo]
            for lb in labs:
                cmd += ["--label", lb]
            p = run(cmd)
            url = p.stdout.strip().splitlines()[-1]
            num = int(url.rstrip("/").split("/")[-1])
            if ident:
                mapa[ident] = num
            criadas.append((titulo, num))
            print(f"  #{num:<4} {titulo[:66]}")

    if a.dry_run or not mapa:
        return

    # segunda passada: RFO-Bxx -> #N nos corpos, restaurando a rastreabilidade
    print("\nreligando referencias RFO-* -> #N ...")
    padrao = re.compile("|".join(sorted(map(re.escape, mapa), key=len, reverse=True)))
    for _, num in criadas:
        p = run(["gh", "issue", "view", str(num), "--json", "body"])
        corpo = json.loads(p.stdout)["body"] or ""
        novo = padrao.sub(lambda m: f"{m.group(0)} (#{mapa[m.group(0)]})", corpo)
        novo = re.sub(r"\(#(\d+)\) \(#\1\)", r"(#\1)", novo)
        if novo != corpo:
            run(["gh", "issue", "edit", str(num), "--body", novo])
    print(f"pronto: {len(criadas)} issues, {len(mapa)} identificadores mapeados.")
    print("\nproximo passo: sessao do Gerente roda `.flow/bin/flow triage`.")


if __name__ == "__main__":
    main()
