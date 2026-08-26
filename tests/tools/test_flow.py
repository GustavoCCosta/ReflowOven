#!/usr/bin/env python3
"""Testes de `.flow/bin/flow` — a parte que decide se um patch chega inteiro.

Rode com:  python -m unittest discover -s tests/tools -v

Não é ztest: `flow` é Python e não entra na suíte do twister. O CI não tem job
para isto (adicionar um exige mexer em .github/workflows/, que o PROCESSO §9
reserva ao humano), então este arquivo é medido à mão — e é por isso que ele usa
`git apply --check` de verdade em vez de asserção sobre string: o veredito vem do
mesmo git que o `flow apply` usa, não da minha leitura do formato.
"""

import os
import importlib.machinery
import importlib.util
import subprocess
import sys
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


class TestBaseNaoBloqueiaRestauracao(unittest.TestCase):
    """RFO-G07: recuperação não pode ter mais pré-condição que a operação.

    `flow mutate` apaga arquivos de código-fonte, então ele exige a BASE e morre
    alto quando não consegue determiná-la. `flow mutate --restaurar` é o botão de
    desfazer dessa operação, e o estado que ele existe para recuperar é
    justamente aquele em que os arquivos já foram apagados — então ele não pode
    exigir a mesma coisa.

    Um refactor que mexia em outra parte deste mesmo PR quebrou isso: bastou
    resolver a BASE no topo de cmd_mutate(), antes de olhar `--restaurar`. Foi
    corrigido, e este é o teste que impede a volta. O invariante estava escrito
    em prosa em três lugares e prosa não segurou.
    """

    def repo_sem_origin(self):
        """Repo com dois commits, árvore limpa, e SEM `origin/main`.

        Sem remote, `git merge-base origin/main HEAD` falha, que é a condição
        em que base_ref() não tem o que devolver. Dois commits porque a mutação
        recusa HEAD == BASE.
        """
        d = Path(tempfile.mkdtemp())
        git("init", "--quiet", ".", cwd=d)
        git("config", "user.email", "t@t", cwd=d)
        git("config", "user.name", "t", cwd=d)
        (d / "src").mkdir()
        (d / "src" / "a.c").write_text("int a;\n", newline="\n")
        git("add", "-A", cwd=d)
        git("commit", "--quiet", "-m", "base", cwd=d)
        (d / "src" / "a.c").write_text("int a; int b;\n", newline="\n")
        (d / "src" / "novo.c").write_text("int c;\n", newline="\n")
        git("add", "-A", cwd=d)
        git("commit", "--quiet", "-m", "patch", cwd=d)

        # pré-condição do teste: a BASE realmente não é determinável
        p = git("merge-base", "origin/main", "HEAD", cwd=d)
        self.assertNotEqual(p.returncode, 0,
                            "este repo não deveria ter origin/main")
        return d

    def flow(self, d, *args):
        env = dict(os.environ, REFLOW_DIR=str(d))
        return subprocess.run([sys.executable, str(FLOW), *args],
                              capture_output=True, text=True, env=env,
                              cwd=str(RAIZ))

    def test_restaurar_funciona_sem_base_determinavel(self):
        d = self.repo_sem_origin()

        p = self.flow(d, "mutate", "--restaurar")

        self.assertEqual(p.returncode, 0,
                         "o desfazer recusou-se a rodar sem BASE:\n"
                         + (p.stderr or p.stdout or ""))

    def test_mutar_sem_base_determinavel_morre_alto(self):
        """O outro lado: a operação que apaga arquivo continua exigindo a BASE.

        Sem este par, o teste acima poderia ser satisfeito afrouxando as duas —
        que é o oposto do que a issue quer.
        """
        d = self.repo_sem_origin()

        p = self.flow(d, "mutate")

        self.assertEqual(p.returncode, 1,
                         "a mutação rodou sem saber a BASE; ela apaga arquivos")
        self.assertIn("BASE", p.stderr or "",
                      "a mensagem de erro não diz que o problema é a BASE")

if __name__ == "__main__":
    unittest.main()
