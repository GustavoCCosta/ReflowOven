#!/usr/bin/env python3
"""Testes dos arquivos de papel — a parte que decide de qual conta sai o veredito.

Rode com:  python -m unittest discover -s tests/tools -v

Por que isto é teste e não revisão de texto: desde o RFO-G13 (#57) a `main`
exige 1 aprovação, e ela só conta se vier da conta de Q.A. (`QualityAssurance2007`),
que não é a autora das PRs. Um `gh pr review` sem `GH_TOKEN` no `QA.md` sai na
conta ativa do `gh` — que é a do Dev — e o GitHub recusa com 422 no meio de uma
revisão. E um `gh auth switch` no lugar do `GH_TOKEN` troca a conta ativa da
máquina inteira, derrubando a sessão de Gerente por baixo (RFO-G18, #74).

As duas falhas são de instrução, não de código: só um teste sobre o arquivo de
papel as pega antes de custarem uma sessão.
"""

import re
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
QA_MD = RAIZ / ".flow" / "roles" / "QA.md"

CONTA_QA = "QualityAssurance2007"
PREFIXO = "GH_TOKEN=$(gh auth token --user %s)" % CONTA_QA


def linhas(caminho):
    return caminho.read_text(encoding="utf-8").splitlines()


class TestIdentidadeDoQA(unittest.TestCase):
    def setUp(self):
        self.assertTrue(QA_MD.is_file(), "%s ausente" % QA_MD)
        self.linhas = linhas(QA_MD)
        self.texto = "\n".join(self.linhas)

    def test_declara_a_conta_de_qa(self):
        """O papel diz de qual conta o veredito sai, com o comando pronto."""
        self.assertIn(CONTA_QA, self.texto,
                      "QA.md nao nomeia a conta de Q.A.; a sessao vai revisar "
                      "como o autor da PR e tomar 422")
        self.assertIn(PREFIXO, self.texto,
                      "QA.md nao mostra como passar a identidade por chamada")

    def test_todo_veredito_passa_a_identidade(self):
        """`gh pr review` sem GH_TOKEN sai na conta ativa — a do Dev."""
        sem_identidade = []
        for i, linha in enumerate(self.linhas):
            if "gh pr review" not in linha:
                continue
            anterior = self.linhas[i - 1] if i > 0 else ""
            contexto = anterior + "\n" + linha
            if PREFIXO not in contexto:
                sem_identidade.append((i + 1, linha.strip()))
        self.assertEqual([], sem_identidade,
                         "gh pr review sem %s: %r" % (PREFIXO, sem_identidade))

    def test_nao_instrui_auth_switch(self):
        """Trocar a conta ativa e estado global da maquina: proibido instruir."""
        instrucoes = [
            (i + 1, l.strip())
            for i, l in enumerate(self.linhas)
            if re.search(r"^\s*(?:\$\s*)?gh auth switch", l)
        ]
        self.assertEqual([], instrucoes,
                         "QA.md manda rodar gh auth switch (RFO-G18): %r"
                         % instrucoes)

    def test_nao_usa_add_label_no_pr_review(self):
        """`gh pr review` nao aceita --add-label; o comando falha como digitado."""
        ruins = [
            (i + 1, l.strip())
            for i, l in enumerate(self.linhas)
            if "gh pr review" in l and "--add-label" in l
        ]
        self.assertEqual([], ruins,
                         "--add-label num gh pr review (use gh pr edit): %r"
                         % ruins)


if __name__ == "__main__":
    unittest.main()
