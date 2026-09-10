#!/usr/bin/env python3
"""RFO-G37: `reflow_heater_emergency_off()` nao pode tomar lock.

Rode com:  python -m unittest discover -s tests/tools -v

**A propriedade.** O corte do caminho de erro fatal (RFO-B44, #130) e o unico
escritor do gate que roda depois de a CPU ter decidido parar. O cenario que a
#130 exigia analisar e erro fatal disparado DENTRO do proprio `heater.c`, ou numa
ISR que interrompeu uma secao critica dele: nesse contexto o spinlock do modulo
ja esta tomado, e num core so esperar por ele nunca termina. Tomar o lock ali
seria travar em vez de cortar — com a resistencia no nivel que a janela de PWM
deixou.

Por isso `reflow_heater_emergency_off()` escreve o pino sem lock, e paga por isso
a contabilidade (`req_permille`, `output_on`) podendo ficar incoerente. Aceitavel
so ali, porque o caminho termina em halt e ninguem le aquilo de novo.

**Por que guarda de fonte, e nao ztest.** Nao ha como disparar erro fatal com o
lock tomado em `native_sim`/`qemu_x86` sem vazar o `static struct k_spinlock lock`
do `heater.c` para a API — superficie de producao a servico do teste, num arquivo
cuja unica razao de existir e nao falhar. O Dev declarou a fraqueza no #132, o
Q.A. tentou mecanizar e tambem nao conseguiu, e o veredito daquele PR decidiu:
guarda de fonte, nao acessador de teste.

Entao o que este arquivo defende NAO e que a decisao basta — e que ela nao seja
removida sem ninguem ver. Mesma familia do `test_httpd_snap_lock.py` ao lado.

**Casa o CORPO da funcao, nao o arquivo.** O `heater.c` esta cheio de spinlock
legitimo: `reflow_heater_set_duty()`, `reflow_heater_off()` e
`reflow_heater_tick()` tomam o lock e devem tomar. Um guarda que procurasse
`k_spin_lock` no arquivo reprovaria sempre e nao mediria nada.
"""

import re
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
HEATER = RAIZ / "src" / "core" / "heater.c"

FUNCAO = "reflow_heater_emergency_off"

# Qualquer coisa que possa esperar por outro dono. `k_spin_unlock` entra na lista
# de proposito: aparecer um unlock no corpo significa que um lock apareceu junto.
BLOQUEANTES = (
    "k_spin_lock",
    "k_spin_unlock",
    "k_mutex_lock",
    "k_mutex_unlock",
    "k_sem_take",
    "k_condvar_wait",
    "k_mutex_init",
    "irq_lock",
)


def sem_comentarios(texto):
    """Sem comentarios de C, para prosa nao disparar nem calar o guarda.

    Nos dois sentidos: um comentario que diga "nao toma k_spin_lock" nao deve
    reprovar, e um `k_spin_lock` comentado nao deve absolver — o segundo caso e
    o mesmo engano que a ponta 1 do RFO-G37 conserta no guarda do CMakeLists.
    """
    texto = re.sub(r"/\*.*?\*/", "", texto, flags=re.S)
    return re.sub(r"//[^\n]*", "", texto)


def corpo_da_funcao(fonte, nome):
    """O corpo de `void nome(...)`, casando chaves.

    Devolve None se a funcao nao existir - que e falha por si, porque o corte
    ter desaparecido do `heater.c` e um estado sobre o qual este guarda tem
    opiniao.
    """
    marca = re.search(r"\b" + re.escape(nome) + r"\s*\([^;{]*\)\s*\{", fonte)
    if marca is None:
        return None

    inicio = fonte.index("{", marca.start())
    profundidade = 0
    for i in range(inicio, len(fonte)):
        if fonte[i] == "{":
            profundidade += 1
        elif fonte[i] == "}":
            profundidade -= 1
            if profundidade == 0:
                return fonte[inicio + 1:i]
    return None


def funcoes_do_arquivo(fonte):
    """{nome: corpo} de cada funcao definida no arquivo.

    Ancorado em inicio de linha: definicao de funcao comeca na coluna 0 neste
    projeto, e statement dentro de funcao vem indentado, entao `if (...) {` nao
    entra. `BUILD_ASSERT(...)` e `SYS_INIT(...)` tambem nao, porque terminam em
    `;` e a busca proibe `;` antes da chave.
    """
    saida = {}
    padrao = r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([a-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{"
    for m in re.finditer(padrao, fonte, re.M):
        nome = m.group(1)
        corpo = corpo_da_funcao(fonte, nome)
        if corpo is not None:
            saida[nome] = corpo
    return saida


def funcoes_que_tomam_lock(fonte):
    """Os nomes que o corte nao pode chamar, DERIVADOS do fonte.

    Derivados e nao fixados numa lista aqui (RFO-G37, achado do Q.A. na review
    do #139): uma funcao nova que passe a tomar o lock entra na proibicao sozinha,
    sem ninguem lembrar de atualizar o teste. Lista fixa envelhece exatamente no
    dia em que o modulo cresce.
    """
    return sorted(nome for nome, corpo in funcoes_do_arquivo(fonte).items()
                  if any(b in corpo for b in BLOQUEANTES))


class TestCorteFatalNaoTomaLock(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fonte = sem_comentarios(HEATER.read_text(encoding="utf-8"))

    def test_a_funcao_de_corte_existe(self):
        """Sem ela o firmware nao tem corte no caminho fatal (RFO-B44)."""
        self.assertIsNotNone(
            corpo_da_funcao(self.fonte, FUNCAO),
            "%s() nao existe em %s; o caminho de erro fatal fica sem corte e "
            "qualquer erro fatal volta a deixar o gate do SSR no ultimo nivel "
            "(RFO-B44, #130)" % (FUNCAO, HEATER.name))

    def test_o_corpo_do_corte_nao_toma_lock(self):
        """Lock ali travaria em vez de cortar, com o elemento ligado."""
        corpo = corpo_da_funcao(self.fonte, FUNCAO)
        self.assertIsNotNone(corpo, "%s() nao existe" % FUNCAO)

        achados = [b for b in BLOQUEANTES if b in corpo]

        self.assertEqual(
            [], achados,
            "%s() passou a chamar %r. Um erro fatal levantado dentro do "
            "heater.c chega com o spinlock do modulo tomado, e num core so "
            "esperar por ele nunca termina: o corte travaria com o gate no "
            "nivel que a janela de PWM deixou, que e o defeito da #130 de "
            "volta pela porta do conserto dele (RFO-B44). O corte escreve o "
            "pino sem lock de proposito, e paga com contabilidade possivelmente "
            "incoerente - aceitavel porque o caminho termina em halt."
            % (FUNCAO, achados))

    def test_o_corpo_do_corte_nao_chama_quem_toma_lock(self):
        """Lock indireto trava igual, e chegar nele e o refactor mais natural.

        O corte duplica em tres linhas o que `reflow_heater_off()` faz, entao a
        primeira coisa que uma limpeza de duplicacao propoe e chamar a funcao
        existente - que abre com `k_spin_lock(&lock)`. Nao ha lock literal no
        corpo, e o deadlock e inteiro: erro fatal dentro do heater.c, lock ja
        tomado, o corte espera para sempre e o gate fica onde a janela de PWM o
        deixou.

        A mutacao anterior era alguem ACRESCENTANDO um lock; esta e alguem
        REMOVENDO duplicacao, com intencao melhor e por isso mais provavel
        (RFO-G37, achado do Q.A. na review do #139).

        Cobre chamada indireta dentro do MODULO, que e onde o lock mora. Nao e
        analise de grafo de chamadas e nao pretende ser.
        """
        corpo = corpo_da_funcao(self.fonte, FUNCAO)
        self.assertIsNotNone(corpo, "%s() nao existe" % FUNCAO)

        travantes = [n for n in funcoes_que_tomam_lock(self.fonte) if n != FUNCAO]

        # Fixture: se a derivacao nao achar nada, o teste abaixo nao mede nada.
        self.assertTrue(
            travantes,
            "nenhuma funcao do heater.c aparece como tomadora de lock. Ou o "
            "modulo perdeu o lock que protege o duty, ou o recorte de funcoes "
            "parou de funcionar - nos dois casos este guarda ficou vacuo")

        achados = [n for n in travantes
                   if re.search(r"\b" + re.escape(n) + r"\s*\(", corpo)]

        self.assertEqual(
            [], achados,
            "%s() chama %r, e essas funcoes tomam o spinlock do modulo. Um erro "
            "fatal levantado dentro do heater.c chega com o lock tomado, entao "
            "chamar qualquer uma delas ali trava em vez de cortar - com a "
            "resistencia no nivel que a janela de PWM deixou (RFO-B44, #130). "
            "O corte escreve o pino direto de proposito, e a duplicacao das tres "
            "linhas e o preco disso. Tomadoras de lock derivadas do fonte: %r"
            % (FUNCAO, achados, travantes))

    def test_o_guarda_olha_o_corpo_e_nao_o_arquivo(self):
        """Fixture: o resto do heater.c TEM de ter spinlock, e legitimo.

        Se este teste falhar, o guarda acima virou vacuo - ou a regex de corpo
        parou de recortar, ou o modulo perdeu o lock que protege o duty. Nos
        dois casos o vermelho e aqui, nao numa suposicao silenciosa.
        """
        corpo = corpo_da_funcao(self.fonte, FUNCAO)
        self.assertIsNotNone(corpo, "%s() nao existe" % FUNCAO)

        fora = self.fonte.replace(corpo, "", 1)

        self.assertIn(
            "k_spin_lock", fora,
            "o resto do heater.c nao tem k_spin_lock nenhum. O duty e a janela "
            "de PWM sao lidos e escritos por threads diferentes e precisam do "
            "lock; se ele desapareceu, o guarda acima nao esta medindo o que "
            "diz medir")
        # E o recorte tem de ser so a funcao. Aferido por um vizinho, e NAO
        # por `k_spin_lock` no corpo: isso duplicaria o guarda acima e, sob a
        # mutacao dele, daria o diagnostico errado (RFO-G37).
        self.assertNotIn(
            "reflow_heater_tick", corpo,
            "o recorte do corpo pegou a funcao vizinha tambem; a regex de "
            "corpo parou de recortar e o guarda acima vira uma varredura do "
            "arquivo inteiro")
        self.assertLess(
            len(corpo), len(self.fonte) // 2,
            "o corpo recortado e metade do arquivo ou mais: o recorte falhou")


if __name__ == "__main__":
    unittest.main()
