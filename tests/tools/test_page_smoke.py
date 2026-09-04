#!/usr/bin/env python3
"""RFO-B23: poe o `tools/test_page.js` dentro de uma suite que alguem roda.

Rode com:  python -m unittest discover -s tests/tools -v

O defeito da issue era uma asserção que nunca podia falhar. Consertada, ela
continuava tendo um problema mais simples: **nada roda esse arquivo**. O
`tools/test_page.js` e um dev tool opcional (`node tools/test_page.js` no
README), o job do CI que mede ferramental descobre testes de `unittest` em
`tests/tools`, e a suite do twister nao ve JavaScript. Uma cobertura que
depende de alguem lembrar de rodar a mao e a mesma classe de verde falso, um
nivel acima.

Este wrapper e a ponte mais curta entre os dois: o veredito continua sendo o do
`test_page.js` (rc != 0), e ele passa a ser medido pelo job que ja existe, sem
tocar em `.github/workflows/` — que a §9 do PROCESSO reserva ao humano.
"""

import shutil
import subprocess
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
SCRIPT = RAIZ / "tools" / "test_page.js"


@unittest.skipUnless(shutil.which("node"), "node nao esta instalado")
class TestPaginaWebSmoke(unittest.TestCase):
    def test_test_page_js_passa(self):
        """
        Sem asserção propria de proposito: o que se afirma aqui e o rc do
        script. Reimplementar as checagens dele em Python seria uma segunda
        opiniao capaz de divergir da primeira, que e o defeito do RFO-B24.
        """
        self.assertTrue(SCRIPT.is_file(), f"{SCRIPT} nao existe")

        proc = subprocess.run(
            ["node", str(SCRIPT)],
            cwd=RAIZ,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "tools/test_page.js reprovou. Saida:\n"
                + proc.stdout
                + proc.stderr
            ),
        )


if __name__ == "__main__":
    unittest.main()
