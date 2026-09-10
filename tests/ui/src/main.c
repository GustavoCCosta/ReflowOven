/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B12: painel indisponivel nao pode custar o timeout de publicacao.
 *
 * `ZBUS_SUBSCRIBER_DEFINE` + `ZBUS_CHAN_ADD_OBS` anexam o observador em tempo de
 * COMPILACAO, independentemente da thread. Quando `device_is_ready()` falha, a
 * thread do display retorna - e antes deste patch o observador ficava anexado,
 * habilitado, com fila de 4 e ninguem chamando `zbus_sub_wait()`. Da quinta
 * publicacao em diante o `zbus_chan_pub(..., K_MSEC(20))` do `controller.c`
 * pagava os 20 ms inteiros, a cada 500 ms e em toda transicao de estado ou
 * falta. Com periodo de controle de 100 ms, 20 % de estouro - exatamente quando
 * o hardware do painel falhou.
 *
 * DUAS CENAS, e a divisao e forcada, nao preferencia: a thread do display
 * consulta `device_is_ready()` uma vez, 500 ms depois do boot. Negar a prontidao
 * depois disso nao a faz voltar atras, e negar antes impede de medir o caminho
 * saudavel. Entao a diferenca vive no boot, no `CONFIG_REFLOW_TEST_DENY_PANEL`.
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app.h"

ZBUS_CHAN_DECLARE(reflow_telemetry_chan);

/* O observador do modulo sob teste, para afirmar sobre o estado dele. */
extern const struct zbus_observer reflow_display_sub;

static const struct device *const panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* O mesmo timeout que o controller.c usa ao publicar. */
#define PUB_TIMEOUT K_MSEC(20)

/*
 * Mais que a fila do observador (4). Antes do patch, e a partir da quinta que o
 * timeout comeca a ser pago inteiro - medir seis mostra o regime, nao a borda.
 */
#define PUBS 6

/* Folga sobre o atraso de 500 ms do K_THREAD_DEFINE da thread do display. */
#define THREAD_SETTLE K_MSEC(900)

#if defined(CONFIG_REFLOW_TEST_DENY_PANEL)
/*
 * POST_KERNEL, antes de qualquer thread da aplicacao rodar: quando a thread do
 * display acordar, o painel ja esta indisponivel. `device_is_ready()` le
 * exatamente estes dois campos (`kernel/device.c`), entao negar aqui e o mesmo
 * que um driver cuja init falhou - sem precisar de painel de verdade nem de
 * driver de mentira.
 */
static int deny_panel(void)
{
	panel->state->initialized = false;
	panel->state->init_res = 1;
	return 0;
}
SYS_INIT(deny_panel, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif

/* Custo, em ms, de PUBS publicacoes seguidas com o timeout do controller.c. */
static void medir(int64_t *pior, int64_t *total)
{
	struct reflow_telemetry t = {
		.temp_mc = 25000,
		.temp_valid = true,
	};

	*pior = 0;
	*total = 0;

	for (int i = 0; i < PUBS; i++) {
		int64_t t0 = k_uptime_get();
		int64_t dt;

		(void)zbus_chan_pub(&reflow_telemetry_chan, &t, PUB_TIMEOUT);
		dt = k_uptime_get() - t0;

		*total += dt;
		if (dt > *pior) {
			*pior = dt;
		}
	}
}

#if defined(CONFIG_REFLOW_TEST_DENY_PANEL)

ZTEST(reflow_ui, test_painel_indisponivel_nao_custa_o_timeout)
{
	int64_t pior, total;
	bool habilitado = true;

	/* Fixture: o painel tem de estar mesmo indisponivel, ou nada e medido. */
	zassert_false(device_is_ready(panel),
		      "fixture quebrada: o painel esta pronto, entao a thread do "
		      "display nao tomou o ramo de falha e este teste passaria "
		      "por drenagem, nao pelo patch");

	k_sleep(THREAD_SETTLE);

	zassert_ok(zbus_obs_is_enabled(&reflow_display_sub, &habilitado));
	zassert_false(habilitado,
		      "o observador do display continua habilitado depois de a "
		      "thread desistir; a fila enche em 4 e toda publicacao "
		      "passa a pagar os 20 ms (RFO-B12)");

	medir(&pior, &total);

	zassert_true(pior < 5,
		     "publicacao custou %lld ms com o painel indisponivel "
		     "(total %lld ms em %d publicacoes). O observador ficou "
		     "anexado sem ninguem drenar, entao o zbus_chan_pub paga o "
		     "timeout de 20 ms inteiro - 20 %% do periodo de controle "
		     "de %d ms (RFO-B12)",
		     pior, total, PUBS, CONFIG_REFLOW_CTRL_PERIOD_MS);
}

#else

ZTEST(reflow_ui, test_painel_pronto_continua_recebendo)
{
	int64_t pior, total;
	bool habilitado = false;

	zassert_true(device_is_ready(panel), "fixture: painel deveria estar pronto");

	k_sleep(THREAD_SETTLE);

	/*
	 * O caminho saudavel: o conserto desabilita o observador SO no ramo de
	 * falha. Se alguem o desabilitar cedo demais, ou de forma incondicional,
	 * e aqui que aparece - a UI pararia de receber telemetria com o painel
	 * funcionando.
	 */
	zassert_ok(zbus_obs_is_enabled(&reflow_display_sub, &habilitado));
	zassert_true(habilitado,
		     "o observador do display esta desabilitado com o painel "
		     "PRONTO: o conserto do RFO-B12 vazou para o caminho "
		     "saudavel e a UI nao recebe mais telemetria");

	/* E a thread esta drenando, entao publicar tambem e barato aqui. */
	medir(&pior, &total);
	zassert_true(pior < 5,
		     "publicacao custou %lld ms com o painel pronto (total %lld "
		     "ms): a thread do display nao esta drenando a fila",
		     pior, total);
}

#endif /* CONFIG_REFLOW_TEST_DENY_PANEL */

ZTEST_SUITE(reflow_ui, NULL, NULL, NULL, NULL, NULL);
