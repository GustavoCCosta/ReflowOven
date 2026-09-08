#!/usr/bin/env python3
"""RFO-G36: label `estado:*` vai na ISSUE, nunca no PR.

Rode com:  python -m unittest discover -s tests/tools -v

**O defeito que isto guarda.** A secao 3 do PROCESSO poe o estado na issue, e a
secao 2 diz que descobrir trabalho e `gh issue list --label estado:pronto`. O
`QA.md` mandava `gh pr edit <N> --add-label estado:ajustes`, ou seja punha a
label num objeto que nada le. A consequencia nao e cosmetica: a ordem de puxada
da secao 4 comeca em `estado:ajustes` exatamente para retrabalho vir antes de
trabalho novo, e com a label no PR aquele primeiro item nunca casa. O PR devolvido
fica invisivel para o Dev, que abre frente nova.

Aconteceu tres vezes num dia (#122 duas, #130 uma), e o dano de segunda ordem foi
o quadro mentir para o Gerente: ele reportou atrito de disciplina do Q.A. que nao
existia, porque o Q.A. estava seguindo o `QA.md` a risca.

**Por que um teste, e por que sobre os tres arquivos.** As transicoes de estado
sao instrucao em documento, e instrucao errada em documento nao produz vermelho
em nenhum teste de comportamento — a mesma familia do
`test_papeis_identidade.py` ao lado, que varre o `QA.md` pela disciplina de
identidade. Este guarda varre os **tres** papeis: o defeito estava so no `QA.md`,
mas o `DEV.md` e o `GERENTE.md` tambem editam label de estado, e nada os impedia
de escorregar do mesmo jeito. O guarda e prospectivo para os dois.

O que ele NAO afirma: que cada papel edite as labels certas, ou que as edite na
ordem certa. Afirma so o objeto — issue, nao PR.
"""

import re
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
PAPEIS = RAIZ / ".flow" / "roles"

# `gh pr edit ... --add-label estado:algo` / `--remove-label estado:algo`.
# Ancorado em `gh pr edit` para nao casar com `gh issue edit`, e o `estado:`
# tem de estar na MESMA linha do comando: uma linha de prosa que mencione
# `estado:ajustes` nao e instrucao de comando.
PR_EDIT_ESTADO = re.compile(
    r"gh\s+pr\s+edit\b[^\n]*--(?:add|remove)-label\s+[\"']?estado:")


def linhas(caminho):
    return caminho.read_text(encoding="utf-8").splitlines()


class TestEstadoVaiNaIssue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.arquivos = sorted(PAPEIS.glob("*.md"))

    def test_os_tres_papeis_existem(self):
        """Se o glob nao achar nada, os testes abaixo passam sem medir."""
        nomes = [p.name for p in self.arquivos]

        for esperado in ("DEV.md", "GERENTE.md", "QA.md"):
            self.assertIn(esperado, nomes,
                          "%s ausente em %s; o guarda abaixo nao mediria nada"
                          % (esperado, PAPEIS))

    def test_nenhum_papel_manda_por_estado_no_pr(self):
        """`gh pr edit --add-label estado:` poe o estado onde nada o le."""
        achados = []
        for caminho in self.arquivos:
            for i, linha in enumerate(linhas(caminho)):
                if PR_EDIT_ESTADO.search(linha):
                    achados.append((caminho.name, i + 1, linha.strip()))

        self.assertEqual(
            [], achados,
            "label estado:* indo para o PR em vez da issue. A secao 3 do "
            "PROCESSO poe o estado na issue e a ordem de puxada da secao 4 le "
            "`gh issue list --label estado:ajustes`, entao uma label no PR faz "
            "o retrabalho ficar invisivel para o Dev (RFO-G36). Use "
            "`gh issue edit <numero da issue>`. Achei: %r" % (achados,))

    def test_cada_papel_edita_estado_pela_issue(self):
        """O contrapositivo: quem move estado tem de faze-lo por `gh issue edit`.

        Sem isto, apagar a linha inteira faria o teste acima passar - e um papel
        que nao diz como mover o estado e tao inutil quanto um que diz errado.
        """
        sem_issue_edit = []
        for caminho in self.arquivos:
            texto = caminho.read_text(encoding="utf-8")

            if "estado:" not in texto:
                continue
            if not re.search(r"gh\s+issue\s+edit\b[^\n]*--(?:add|remove)-label"
                             r"\s+[\"']?estado:", texto):
                sem_issue_edit.append(caminho.name)

        self.assertEqual(
            [], sem_issue_edit,
            "papel que fala de estado:* e nao mostra `gh issue edit "
            "--add-label estado:...`: %r" % (sem_issue_edit,))


if __name__ == "__main__":
    unittest.main()
