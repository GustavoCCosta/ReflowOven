#!/usr/bin/env python3
"""RFO-B14: `snap_lock` do httpd.c tem de ser inicializado em tempo de build.

Rode com:  python -m unittest discover -s tests/tools -v

**Por que este teste e sobre o fonte e nao sobre o comportamento.** O defeito e
de ordem de inicializacao: `telemetry_cb()` roda na thread de controle, que
publica desde a primeira amostra, enquanto `httpd_thread()` so comeca depois do
atraso de 1000 ms do seu `K_THREAD_DEFINE`. Um `k_mutex_init()` ali dentro
rodava DEPOIS de o listener ja ter travado o mutex — num struct zerado cuja
dlist de wait_q nao esta encadeada — e ainda zerava `lock_count` e `owner` de um
objeto em uso.

Isso nao produz vermelho em teste de comportamento num alvo de um nucleo: o
caminho sem contencao nunca toca `wait_q`, e nenhuma asserção do kernel confere
inicializacao (`kernel/mutex.c` so afirma `!arch_is_in_isr()`). A suite ztest
inteira passa com o defeito presente, e passava — foi assim que ele chegou a
esta issue por leitura de codigo, e nao por teste. O que da vermelho e a
contencao real, ou um build SMP como os alvos ESP32, e nenhuma das duas coisas
esta ao alcance da suite simulada de um nucleo.

Entao o que se guarda aqui e a **propriedade que o conserto estabelece**, no
unico lugar onde ela e observavel sem hardware: o mutex e um objeto de kernel
definido em tempo de build, e nada o reinicializa em tempo de execucao.
"""

import re
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
HTTPD = RAIZ / "src" / "net" / "httpd.c"


class TestSnapLockEstaticamenteInicializado(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fonte = HTTPD.read_text(encoding="utf-8")

    def test_snap_lock_e_definido_com_k_mutex_define(self):
        """K_MUTEX_DEFINE inicializa no link; `struct k_mutex` cru, nao."""
        # re.M porque a definicao esta no meio do arquivo, e assertTrue em vez de
        # assertRegex para a mensagem de falha nao despejar o httpd.c inteiro.
        achou = re.search(r"^static\s+K_MUTEX_DEFINE\(snap_lock\);\s*$",
                          self.fonte, re.M)
        self.assertTrue(
            achou,
            msg=(
                "snap_lock nao esta definido com K_MUTEX_DEFINE. Um "
                "'static struct k_mutex snap_lock;' comeca zerado e so vira um "
                "mutex valido quando alguem chamar k_mutex_init() -- e o "
                "listener de telemetria trava esse mutex antes de a thread do "
                "servidor existir (RFO-B14)."
            ),
        )

    def test_nada_reinicializa_snap_lock_em_tempo_de_execucao(self):
        """A init em tempo de execucao e o defeito, nao o conserto."""
        chamadas = re.findall(r"k_mutex_init\s*\(\s*&\s*snap_lock\s*\)", self.fonte)
        self.assertEqual(
            chamadas,
            [],
            msg=(
                "k_mutex_init(&snap_lock) voltou ao httpd.c. Chamada depois do "
                "boot, ela zera lock_count e owner de um mutex que o listener "
                "de telemetria ja usa, e o K_FOREVER de build_state_json() e "
                "onde isso termina (RFO-B14)."
            ),
        )

    def test_o_mutex_ainda_e_usado_pelos_dois_lados(self):
        """
        Guarda-trilho contra o falso verde obvio: apagar o mutex tambem faria os
        dois testes acima passarem. Os dois caminhos que compartilham o snapshot
        continuam tendo de travar alguma coisa.
        """
        self.assertIn("k_mutex_lock(&snap_lock, K_MSEC(5))", self.fonte,
                      "o listener de telemetria deixou de travar o snapshot")
        self.assertIn("k_mutex_lock(&snap_lock, K_FOREVER)", self.fonte,
                      "o lado que serve /api/state deixou de travar o snapshot")


if __name__ == "__main__":
    unittest.main()
