#!/usr/bin/env python3
"""RFO-B44: o handler de erro fatal tem de estar na imagem da APLICACAO.

Rode com:  python -m unittest discover -s tests/tools -v

**Por que este teste e sobre o build e nao sobre o comportamento.** O corte do
gate no caminho fatal tem prova comportamental: `tests/fatal` monta uma imagem,
leva o gate a alto e afirma que ele vai a baixo, e a mutacao do corpo de
`reflow_heater_emergency_off()` deixa aquelas asserções vermelhas.

O que aquela suite NAO pode provar e que o handler esta no firmware que vai para
a placa. `tests/fatal/CMakeLists.txt` lista o `fatal.c` por conta propria, como
toda suite de teste lista os fontes que quer — entao as duas cenas continuam
verdes mesmo que o arquivo saia do `target_sources()` da aplicacao. Foi assim que
o Q.A. reprovou o #132: tirou UMA linha do `CMakeLists.txt` da aplicacao, o
firmware inteiro voltou ao defeito da #130, e a suite inteira ficou verde
(`101 of 101 executed test cases passed`).

A matriz de placas tambem nao pega: ela constroi, e construir continua
funcionando sem o arquivo.

Entao a propriedade fica guardada aqui, no unico lugar onde e observavel sem
imagem construida — a fiacao do build. E a mesma familia do
`test_httpd_snap_lock.py` ao lado: propriedade que nao produz vermelho em teste
de comportamento, guardada no fonte.

**O que se guarda e a propriedade, nao a linha.** O teste descobre no `src/` qual
arquivo define o handler, em vez de procurar o nome `fatal.c`: renomear o arquivo
ou mudar o handler de lugar dentro do nucleo continua passando, e tira-lo do
build da aplicacao — ou torna-lo condicional a uma folha — fica vermelho.
"""

import re
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
CMAKE = RAIZ / "CMakeLists.txt"
SRC = RAIZ / "src"

HANDLER = "k_sys_fatal_error_handler"

# Definicao, nao declaracao: o nome seguido de lista de parametros e `{` antes
# de qualquer `;`. Uma declaracao em header termina em `;` e nao casa.
DEFINICAO = re.compile(
    r"\b" + HANDLER + r"\s*\([^;{]*\)\s*\{", re.S)


def bloco_target_sources_incondicional(texto):
    """O conteudo do `target_sources(app PRIVATE ...)` sem `_ifdef`.

    Varre parenteses para achar o fim do bloco, em vez de assumir que ele cabe
    numa linha ou que termina na primeira `)` — os fontes da aplicacao estao
    numa lista de varias linhas.
    """
    marca = re.search(r"target_sources\s*\(\s*app\s+PRIVATE", texto)
    if marca is None:
        return None

    profundidade = 0
    inicio = texto.index("(", marca.start())
    for i in range(inicio, len(texto)):
        if texto[i] == "(":
            profundidade += 1
        elif texto[i] == ")":
            profundidade -= 1
            if profundidade == 0:
                return texto[inicio + 1:i]
    return None


class TestHandlerFatalEstaNoBuildDaAplicacao(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = CMAKE.read_text(encoding="utf-8")
        cls.fontes = sorted(SRC.rglob("*.c"))

    def definidores(self):
        """Os .c de src/ que DEFINEM o handler."""
        achados = []
        for c in self.fontes:
            if DEFINICAO.search(c.read_text(encoding="utf-8")):
                achados.append(c.relative_to(RAIZ).as_posix())
        return achados

    def test_exatamente_um_arquivo_define_o_handler(self):
        """Dois definidores nao linkam; zero e o defeito da #130 de volta."""
        achados = self.definidores()

        self.assertEqual(
            1, len(achados),
            "esperado exatamente um arquivo em src/ definindo %s, achei %r. "
            "Zero significa que o firmware voltou ao handler default do Zephyr, "
            "que halta sem cortar o gate (RFO-B44); dois nao linkam."
            % (HANDLER, achados))

    def test_o_definidor_esta_nos_fontes_incondicionais_da_aplicacao(self):
        """Sem esta linha o firmware perde o corte e nada mais fica vermelho."""
        achados = self.definidores()
        self.assertTrue(achados, "nenhum arquivo define %s" % HANDLER)
        definidor = achados[0]

        bloco = bloco_target_sources_incondicional(self.cmake)
        self.assertIsNotNone(
            bloco,
            "nao achei target_sources(app PRIVATE ...) no CMakeLists.txt")

        # O caminho aparece no CMakeLists relativo a raiz da aplicacao.
        self.assertIn(
            definidor, bloco,
            "%s define %s e NAO esta no target_sources(app PRIVATE) "
            "incondicional. A imagem que vai para a placa perde o handler e "
            "qualquer erro fatal volta a deixar o gate do SSR no ultimo nivel "
            "(RFO-B44, #130). tests/fatal continua verde nesse estado porque "
            "lista o arquivo por conta propria - foi por isso que este guarda "
            "existe." % (definidor, HANDLER))

    def test_o_definidor_nao_depende_de_folha_nenhuma(self):
        """O handler e do nucleo: `target_sources_ifdef` o tornaria removivel."""
        achados = self.definidores()
        self.assertTrue(achados, "nenhum arquivo define %s" % HANDLER)
        definidor = achados[0]

        condicionais = [
            linha.strip()
            for linha in self.cmake.splitlines()
            if "target_sources_ifdef" in linha and definidor in linha
        ]
        self.assertEqual(
            [], condicionais,
            "%s aparece num target_sources_ifdef: o corte do caminho fatal "
            "passaria a depender de uma feature opcional estar ligada, e "
            "desliga-la devolveria o defeito da #130. Achei: %r"
            % (definidor, condicionais))


if __name__ == "__main__":
    unittest.main()
