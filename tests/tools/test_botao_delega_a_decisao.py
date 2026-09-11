#!/usr/bin/env python3
"""RFO-B13: a decisao do botao vive na funcao pura, nao no input_ui.c.

Rode com:  python -m unittest discover -s tests/tools -v

**Por que este guarda existe.** O `tests/logic/` prova a tabela de decisao, e a
mutacao do corpo de `reflow_button_decide()` deixa aquelas asserções vermelhas.
O que nenhuma delas ve e se o `input_ui.c` ainda **chama** a funcao: alguem que
reintroduza o `if` no callback - por pressa, ou "consertando" uma leitura
atomica - deixa a suite inteira verde com o defeito de volta na placa.

E o mesmo buraco que o Q.A. achou no RFO-B44 (#132): a propriedade estava
provada dentro da imagem de teste e nada verificava a fiacao da imagem que vai
para o forno. Aqui a fiacao e mais curta - uma chamada -, e por isso mesmo mais
facil de desfazer sem ninguem ver.

**O que se afirma, e o que nao.** Que o `input_ui.c` delega: chama
`reflow_button_decide()` e nao nomeia nenhum dos tres comandos que o botao
decide. Nao se afirma que a delegacao esta correta - isso e o `tests/logic/`.
`REFLOW_CMD_SELECT_PROFILE` fica de fora da proibicao de proposito: ele e do
`on_rotate()`, que nao e este ticket.
"""

import re
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
INPUT_UI = RAIZ / "src" / "ui" / "input_ui.c"
BUTTONMAP = RAIZ / "src" / "ui" / "buttonmap.c"

DECISOR = "reflow_button_decide"

# Os tres que a decisao do botao escolhe. SELECT_PROFILE nao entra: e do rotate.
COMANDOS_DO_BOTAO = (
    "REFLOW_CMD_START",
    "REFLOW_CMD_STOP",
    "REFLOW_CMD_CLEAR_FAULT",
)


def sem_comentarios(texto):
    """Sem comentarios de C, para prosa nao disparar nem calar o guarda.

    Os comentarios do `input_ui.c` citam os comandos ao explicar o RFO-B13, e
    citar nao e decidir. Na outra direcao, um `post(REFLOW_CMD_CLEAR_FAULT, 0)`
    comentado tambem nao deve absolver.
    """
    texto = re.sub(r"/\*.*?\*/", "", texto, flags=re.S)
    return re.sub(r"//[^\n]*", "", texto)


class TestBotaoDelegaADecisao(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.input_ui = sem_comentarios(INPUT_UI.read_text(encoding="utf-8"))
        cls.buttonmap_existe = BUTTONMAP.is_file()

    def test_a_funcao_pura_existe(self):
        """Sem ela nao ha o que delegar, e o tests/logic/ nao compila."""
        self.assertTrue(
            self.buttonmap_existe,
            "%s ausente: a decisao do botao voltou para dentro do input_ui.c e "
            "deixa de ser exercitavel sem imagem (RFO-B13)" % BUTTONMAP)

    def test_o_input_ui_chama_o_decisor(self):
        """A fiacao que nenhum teste de comportamento ve."""
        self.assertRegex(
            self.input_ui, r"\b" + DECISOR + r"\s*\(",
            "input_ui.c nao chama %s(). A tabela de decisao continua provada em "
            "tests/logic/, e o firmware nao a usa - que e o defeito do RFO-B13 "
            "de volta com a prova intacta" % DECISOR)

    def test_o_input_ui_nao_decide_por_conta_propria(self):
        """Decisao duplicada no callback e decisao que diverge da testada."""
        achados = sorted({c for c in COMANDOS_DO_BOTAO if c in self.input_ui})

        self.assertEqual(
            [], achados,
            "input_ui.c nomeia %r. Os tres comandos que o botao escolhe sao "
            "decididos em buttonmap.c, que e o arquivo puro que tests/logic/ "
            "percorre inteiro; nomea-los aqui significa que ha uma segunda "
            "decisao, nao coberta, no caminho de parada do operador (RFO-B13). "
            "REFLOW_CMD_SELECT_PROFILE nao entra nesta lista: e do on_rotate()."
            % (achados,))


if __name__ == "__main__":
    unittest.main()
