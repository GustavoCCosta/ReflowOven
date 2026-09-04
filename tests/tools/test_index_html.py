#!/usr/bin/env python3
"""RFO-B25: a pagina servida pelo firmware e gerada pelo build, nao commitada.

Rode com:  python -m unittest discover -s tests/tools -v

**O que este arquivo guarda, e o que NAO guarda.** A propriedade que importa —
editar `src/net/index.html`, rodar `west build` e o firmware servir a pagina
nova — e de build, e foi medida como tal: com o patch, uma sentinela posta no
`index.html` aparece no ELF; sem ele, o ELF fica com a pagina antiga e o build
passa. Isso esta colado no PR e nao cabe aqui: o job de ferramental nao tem
Zephyr nem SDK, entao ele nao pode compilar nada.

O que cabe aqui e a regressao que devolveria o defeito em silencio: alguem
commitar `src/net/index_html.h` de novo. Um `#include` entre aspas procura
primeiro ao lado do arquivo que inclui, entao uma copia em `src/net/` **vence**
a gerada no diretorio de build — o firmware voltaria a servir um header
versionado, e o build continuaria dizendo que gerou o outro.
"""

import re
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
HEADER = RAIZ / "src" / "net" / "index_html.h"
REGRA = RAIZ / "cmake" / "index_html.cmake"
GITIGNORE = RAIZ / ".gitignore"


class TestPaginaGeradaPeloBuild(unittest.TestCase):
    def test_o_header_nao_esta_na_arvore(self):
        """Uma copia em src/net/ sombreia a gerada e o defeito volta calado."""
        self.assertFalse(
            HEADER.exists(),
            msg=(
                f"{HEADER} existe. Um #include entre aspas procura primeiro ao "
                "lado do arquivo que inclui, entao esta copia vence a que o "
                "build gera: o firmware serve o header versionado e ninguem ve "
                "diferenca na saida do build (RFO-B25). Apague o arquivo; o "
                "build o gera."
            ),
        )

    def test_o_gitignore_impede_o_retorno(self):
        self.assertIn(
            "src/net/index_html.h",
            GITIGNORE.read_text(encoding="utf-8"),
            msg=".gitignore parou de proteger contra o header versionado",
        )

    def test_a_regra_de_build_depende_da_pagina_e_do_gerador(self):
        """
        A dependencia e o conserto inteiro: sem ela o header nao e refeito
        quando a pagina muda, que era o defeito. O gerador tambem entra, senao
        mudar o escape espera a pagina mudar tambem.
        """
        regra = REGRA.read_text(encoding="utf-8")

        self.assertRegex(
            regra,
            r"OUTPUT\s+\$\{header\}",
            msg="a regra deixou de declarar o header como saida",
        )
        deps = re.search(r"DEPENDS\s+([^\n]*)", regra)
        self.assertIsNotNone(deps, "a regra ficou sem DEPENDS")
        self.assertIn("${page}", deps.group(1),
                      "a regra nao depende mais de index.html: editar a pagina "
                      "deixaria de regenerar o header (RFO-B25)")
        self.assertIn("${script}", deps.group(1),
                      "a regra nao depende mais do gen_page.py")


if __name__ == "__main__":
    unittest.main()
