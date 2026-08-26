#!/usr/bin/env python3
"""Testes de `.flow/bin/flow` — a parte que decide se um patch chega inteiro.

Rode com:  python -m unittest discover -s tests/tools -v

Não é ztest: `flow` é Python e não entra na suíte do twister. O CI não tem job
para isto (adicionar um exige mexer em .github/workflows/, que o PROCESSO §9
reserva ao humano), então este arquivo é medido à mão — e é por isso que ele usa
`git apply --check` de verdade em vez de asserção sobre string: o veredito vem do
mesmo git que o `flow apply` usa, não da minha leitura do formato.
"""

import importlib.machinery
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
FLOW = RAIZ / ".flow" / "bin" / "flow"


def carregar_flow():
    """Importa o script `flow`, que não tem extensão .py."""
    loader = importlib.machinery.SourceFileLoader("flowmod", str(FLOW))
    spec = importlib.util.spec_from_loader("flowmod", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


flowmod = carregar_flow()


def git(*args, cwd, entrada=None):
    return subprocess.run(["git", *args], cwd=str(cwd), capture_output=True,
                          text=True, input=entrada)


class TestExtraiDiff(unittest.TestCase):
    """O defeito do RFO-G07: a última hunk chegava corrompida."""

    def repo_com_patch(self, conteudo_antes, conteudo_depois):
        """Cria um repo temporário e devolve (dir, patch real gerado pelo git)."""
        d = Path(tempfile.mkdtemp())
        git("init", "--quiet", cwd=d)
        git("config", "user.email", "t@t", cwd=d)
        git("config", "user.name", "t", cwd=d)
        alvo = d / "arquivo.txt"
        alvo.write_text(conteudo_antes, newline="\n")
        git("add", "-A", cwd=d)
        git("commit", "--quiet", "-m", "base", cwd=d)
        alvo.write_text(conteudo_depois, newline="\n")
        patch = git("diff", cwd=d).stdout
        git("checkout", "--", ".", cwd=d)
        return d, patch

    def test_ultima_hunk_com_linha_de_contexto_vazia_sobrevive(self):
        """O caso que corrompia: a última linha do patch é um único espaço.

        Uma linha em branco no arquivo vira uma linha de contexto que é
        exatamente ' '. Qualquer rstrip() sobre o patch inteiro come esse
        espaço, a contagem do cabeçalho @@ deixa de bater, e o git recusa o
        patch como corrompido — culpando quem o gerou.
        """
        d, patch = self.repo_com_patch("a\nb\nc\n\n", "A\nb\nc\n\n")

        self.assertTrue(patch.endswith(" \n"),
                        "pré-condição do teste: o patch do git termina em linha "
                        "de contexto vazia. Terminou em %r" % patch[-12:])

        extraido = flowmod.extrai_diff(patch)

        p = git("apply", "--check", "-", cwd=d, entrada=extraido)
        self.assertEqual(p.returncode, 0,
                         "o patch extraído não aplica:\n" + (p.stderr or ""))

    def test_patch_dentro_de_bloco_markdown_tambem_sobrevive(self):
        """Mesmo caso, mas embrulhado no doc — o modo como o fluxo v1 o entregava."""
        d, patch = self.repo_com_patch("a\nb\nc\n\n", "A\nb\nc\n\n")
        doc = ("# PR qualquer\n\nTexto antes.\n\n```diff\n" + patch +
               "```\n\nTexto depois.\n")

        extraido = flowmod.extrai_diff(doc)

        p = git("apply", "--check", "-", cwd=d, entrada=extraido)
        self.assertEqual(p.returncode, 0,
                         "o patch extraído do bloco não aplica:\n" + (p.stderr or ""))

    def test_preambulo_e_linhas_em_branco_do_documento_sao_descartados(self):
        """O que extrai_diff deve continuar jogando fora: prosa e linhas vazias."""
        d, patch = self.repo_com_patch("a\nb\n", "A\nb\n")
        doc = "Preâmbulo do documento.\nOutra linha.\n\n" + patch + "\n\n\n"

        extraido = flowmod.extrai_diff(doc)

        self.assertTrue(extraido.startswith("diff --git "),
                        "o preâmbulo não foi descartado: %r" % extraido[:40])
        self.assertFalse(extraido.endswith("\n\n"),
                         "linhas vazias do documento vazaram para o fim do patch")
        p = git("apply", "--check", "-", cwd=d, entrada=extraido)
        self.assertEqual(p.returncode, 0, p.stderr or "")



    def test_linha_final_so_de_espacos_e_tratada_como_conteudo(self):
        """Escolha deliberada, registrada como teste em vez de acidente.

        O laço descarta apenas linhas exatamente ''. Uma linha final com espaços
        — que o editor de quem montou o documento pode ter deixado — antes era
        comida pelo .rstrip() e agora chega ao git como linha de contexto.

        Preservar é o lado seguro de errar, porque ' ' (um espaço) é a linha de
        contexto de uma linha em branco e é exatamente o que o RFO-G07 quebrava.
        Distinguir '   ' de ' ' exigiria a ferramenta interpretar o formato do
        patch, que é a competência que ela não tem e não deve fingir ter.

        Este teste existe para que mudar isso seja uma decisão, não um efeito
        colateral: se alguém voltar a aparar espaços no fim, ele fala.
        """
        d, patch = self.repo_com_patch("a\nb\nc\n\n", "A\nb\nc\n\n")

        # o patch legítimo termina em ' ' (contexto da linha em branco); o
        # documento acrescenta ruído só-de-espaços depois dele
        doc = "```diff\n" + patch + "   \n```\n"

        extraido = flowmod.extrai_diff(doc)

        self.assertTrue(extraido.endswith("   \n"),
                        "a linha só-de-espaços deveria ter sido preservada como "
                        "conteúdo; fim do extraído: %r" % extraido[-8:])
        self.assertIn(" \n", extraido,
                      "a linha de contexto vazia do patch sumiu")

if __name__ == "__main__":
    unittest.main()
