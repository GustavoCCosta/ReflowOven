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


def comandos_logicos(ls):
    """Pares (numero da primeira linha, comando com as continuacoes juntas).

    A janela tem de ser o comando de shell, nao a linha fisica. Varrer linha a
    linha deixa passar exatamente a forma que o `QA.md` usa por convencao:

        gh pr edit <PR> \\
          --add-label estado:ajustes

    e o bloco guardado quebra linha assim tres linhas acima, para o scanner de
    identidade continuar verde. Ou seja: a edicao natural do documento derrotava
    o guarda (RFO-G37, achado do Q.A. na review do #135).

    Este e o mesmo raciocinio do `comando_logico()` do
    `test_papeis_identidade.py` ao lado, e as seis linhas estao duplicadas de
    proposito: importar entre dois modulos de teste faria renomear um quebrar o
    outro, e nenhum dos dois deve depender do vizinho para medir. Se um terceiro
    guarda precisar disto, ai vale modulo compartilhado.
    """
    saida = []
    i = 0
    while i < len(ls):
        inicio = i
        partes = [ls[i]]
        while ls[i].rstrip().endswith("\\") and i + 1 < len(ls):
            i += 1
            partes.append(ls[i])
        # As continuacoes viram uma linha so, para a regex de uma linha valer.
        junto = "\n".join(partes)
        saida.append((inicio + 1, re.sub(r"\\\s*\n\s*", " ", junto)))
        i += 1
    return saida


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
            for numero, comando in comandos_logicos(linhas(caminho)):
                if PR_EDIT_ESTADO.search(comando):
                    achados.append((caminho.name, numero, comando.strip()))

        self.assertEqual(
            [], achados,
            "label estado:* indo para o PR em vez da issue. A secao 3 do "
            "PROCESSO poe o estado na issue e a ordem de puxada da secao 4 le "
            "`gh issue list --label estado:ajustes`, entao uma label no PR faz "
            "o retrabalho ficar invisivel para o Dev (RFO-G36). Use "
            "`gh issue edit <numero da issue>`. Achei: %r" % (achados,))

    def test_o_detector_pega_as_tres_formas(self):
        """As sondas do Q.A. na review do #135, mecanizadas.

        Um guarda de fonte e tao bom quanto a sua janela, e a deste era a linha
        fisica: a forma B passava verde, e B nao e invencao - e a convencao do
        proprio bloco do `QA.md`, que quebra linha com `\\` tres linhas acima do
        comando corrigido. Ficam aqui para que a janela nao possa encolher de
        novo sem alguem ver.
        """
        casos = [
            # (rotulo, linhas, deve ser flagrado)
            ("A: uma linha, o defeito que existia",
             ["gh pr edit <N> --add-label estado:ajustes"], True),
            ("B: continuacao de linha, a forma que escapava",
             ["gh pr edit <PR> \\", "  --add-label estado:ajustes"], True),
            ("B2: continuacao com o label antes do numero",
             ["gh pr edit \\", "  --add-label estado:ajustes <PR>"], True),
            ("C: gh issue edit, a forma certa",
             ["gh issue edit <ISSUE> --add-label estado:ajustes"], False),
            ("D: prosa citando estado:ajustes e gh pr edit em linhas distintas",
             ["O Q.A. poe `estado:ajustes` na issue.",
              "Nao use `gh pr edit` para isso."], False),
        ]

        for rotulo, ls, esperado in casos:
            flagrado = any(PR_EDIT_ESTADO.search(cmd)
                           for _, cmd in comandos_logicos(ls))
            self.assertEqual(
                esperado, flagrado,
                "%s: esperava flagrado=%s, deu %s. Entrada: %r"
                % (rotulo, esperado, flagrado, ls))

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
