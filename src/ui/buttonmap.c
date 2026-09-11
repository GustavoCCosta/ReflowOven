/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B13. A pressao longa era CLEAR_FAULT incondicional, e o handle_cmd()
 * descarta CLEAR_FAULT fora de FAULT sem log e sem mudar a UI. Entao o operador
 * que segurava o botao com a corrida em curso - com a intencao de PARAR - via o
 * forno continuar aquecendo e nada lhe dizia que nao foi obedecido. Terceiro
 * ticket da mesma familia (RFO-B19, RFO-B41), e este no unico controle que
 * existe quando nao ha rede.
 */

#include "buttonmap.h"

#include "../core/app.h"

int reflow_button_decide(bool state_known, uint8_t state, bool long_press)
{
	/*
	 * Estado desconhecido - antes da primeira telemetria - so pode produzir
	 * o comando que nao liga nada. CLEAR_FAULT aqui e o inverso de falhar
	 * seguro: e a acao que REARMA o forno, feita por padrao sobre um estado
	 * que ninguem leu ainda. START tambem nao serve: se o forno estiver
	 * ocioso, ele comeca uma corrida a partir de uma tela que ainda nao
	 * mostrou nada.
	 *
	 * STOP e o unico que nao pode energizar. No pior caso e no-op.
	 *
	 * A janela dura ate a primeira publicacao - CONFIG_REFLOW_PUBLISH_PERIOD_MS,
	 * 500 ms por padrao - e o preco dela e que uma pressao nesse instante nao
	 * inicia a corrida. O operador pressiona de novo.
	 */
	if (!state_known) {
		return REFLOW_CMD_STOP;
	}

	switch (state) {
	case REFLOW_STATE_RUNNING:
		/*
		 * O defeito do titulo. Com a corrida em curso, QUALQUER pressao
		 * e um pedido de parada: e o que o operador quer, e e o unico
		 * comando cuja recusa nao deixa a resistencia ligada.
		 */
		return REFLOW_CMD_STOP;

	case REFLOW_STATE_FAULT:
		if (long_press) {
			/* O unico uso legitimo da pressao longa, e o de hoje. */
			return REFLOW_CMD_CLEAR_FAULT;
		}
		/*
		 * Pressao curta em FAULT continua postando START, e isso e
		 * DELIBERADO (RFO-B13). Nao porque START seja util ali - o
		 * handle_cmd o recusa -, mas porque ele e o unico comando que a
		 * recusa EXPLICA: "start refused: clear the fault first"
		 * (controller.c). STOP fora de RUNNING e no-op silencioso, entao
		 * troca-lo por STOP apagaria a unica linha que diz ao operador o
		 * que fazer em seguida.
		 *
		 * Consertar o silencio do handle_cmd para comando invalido e
		 * defeito de verdade e da mesma familia, mas e do nucleo e tem
		 * alcance maior que este botao - fora de escopo aqui por decisao
		 * do ticket.
		 */
		return REFLOW_CMD_START;

	case REFLOW_STATE_IDLE:
	case REFLOW_STATE_DONE:
	default:
		/*
		 * Ocioso ou terminado: a pressao longa faz o MESMO que a curta,
		 * e comeca. Entre isto e nao postar nada, escolhi comecar porque
		 * "nao postar nada" reproduz exatamente o defeito que este ticket
		 * existe para remover - o operador age e nada acontece, sem
		 * retorno nenhum.
		 *
		 * O risco residual, dito em voz alta: o botao ja inicia a corrida
		 * com uma pressao curta, entao "botao pressionado com o forno
		 * parado inicia" ja e o contrato; o que muda e a pressao longa
		 * deixar de ser uma excecao surpresa. Um botao presso por
		 * acidente inicia uma corrida - e isso vale para a pressao curta
		 * desde sempre, e nao e introduzido aqui.
		 */
		return REFLOW_CMD_START;
	}
}
