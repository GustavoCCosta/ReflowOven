/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * O que o botao do encoder manda, dado o estado do forno e a duracao da
 * pressao. Pura, sem Zephyr, testada em tests/logic/ - e esta no formato que o
 * CLAUDE.md reserva para "logica que decide se a resistencia liga", ao lado de
 * net/cmdparse.c e net/httpgate.c.
 *
 * Por que separada do input_ui.c (RFO-B13): a decisao e o caminho de parada do
 * operador quando nao ha rede. Dentro de um callback do subsistema de input ela
 * so seria exercitavel com imagem, driver e eventos; aqui e uma tabela que o
 * teste percorre inteira.
 */

#ifndef REFLOW_BUTTONMAP_H_
#define REFLOW_BUTTONMAP_H_

#include <stdbool.h>
#include <stdint.h>

/* Nenhum comando a postar. Distinto de qualquer REFLOW_CMD_*, que sao >= 0. */
#define REFLOW_BUTTON_NONE (-1)

/*
 * `state` e um REFLOW_STATE_*, valido somente quando `state_known`. Antes da
 * primeira telemetria o estado do forno e desconhecido, e essa e a distincao
 * que decide seguranca - ver o comentario no .c.
 *
 * Devolve um REFLOW_CMD_* ou REFLOW_BUTTON_NONE.
 */
int reflow_button_decide(bool state_known, uint8_t state, bool long_press);

#endif /* REFLOW_BUTTONMAP_H_ */
